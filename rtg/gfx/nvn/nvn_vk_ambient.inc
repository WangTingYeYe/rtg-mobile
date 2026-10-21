/* Letterbox effect for the full-screen surface.
 *
 * A 64 x 36 light map (temporally smoothed by AMBIENT_TAU_S) is stretched over
 * the screen as a blurred ambient background. The bars show it directly and
 * the same pass feathers the picture's outer edge into it. Each swapchain image
 * owns its map pair, descriptor sets and uniforms, so a slot is not rewritten
 * while the GPU still uses it. */
#include "nvn_vk_ambient_spv.h"

#define AMBIENT_MAP_W 64
#define AMBIENT_MAP_H 36
#define AMBIENT_TAU_S 0.06

typedef struct { float rect[4], uv[4], edge[4], params[4], light[4]; } ambient_uniforms;

typedef struct {
    VkImage image[2];
    VkDeviceMemory memory[2];
    VkImageView view[2];
    VkFramebuffer framebuffer[2];
    VkImageLayout layout[2];
    VkDescriptorSet map_set, bar_set;
    VBuffer ubo;
    unsigned uses;
    uint64_t last_ns;
} ambient_slot;

static struct {
    int ready, gain_known, enabled;
    float gain;
    VkFormat map_format, bar_format;
    VkRenderPass map_pass, bar_pass;
    VkPipeline map_pipeline, bar_pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout set_layout;
    NvnVkDescriptorPools sets;
    VkSampler sampler;
    VkImageView swap_view[16];
    VkFramebuffer swap_framebuffer[16];
    ambient_slot slots[16];
} ambient;

static float ambient_gain(void) {
    if (!ambient.gain_known) {
        ambient.gain_known = 1;
        const char *e = getenv("DM_NVN_AMBIENT_BARS");
        float gain = e && *e ? strtof(e, NULL) : 0.0f;
        if (!(gain > 0.0f)) gain = 0.0f;
        if (gain > 1.0f) gain = 1.0f;
        ambient.gain = gain;
        ambient.enabled = gain > 0.0f;
        fprintf(stderr, "[nvn-vulkan] AMBIENT_BARS strength=%.2f (DM_NVN_AMBIENT_BARS)\n", gain);
    }
    return ambient.gain;
}

static VkFormat ambient_map_format(void) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(G.physical, VK_FORMAT_R16G16B16A16_SFLOAT, &p);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((p.optimalTilingFeatures & need) == need) return VK_FORMAT_R16G16B16A16_SFLOAT;
    vkGetPhysicalDeviceFormatProperties(G.physical, VK_FORMAT_R8G8B8A8_UNORM, &p);
    if ((p.optimalTilingFeatures & need) == need) return VK_FORMAT_R8G8B8A8_UNORM;
    return VK_FORMAT_UNDEFINED;
}

static void ambient_image_create(ambient_slot *s, int which) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = ambient.map_format, .extent = {AMBIENT_MAP_W, AMBIENT_MAP_H, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(G.device, &ci, NULL, &s->image[which]));
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(G.device, s->image[which], &r);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
        .memoryTypeIndex = memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(G.device, &ai, NULL, &s->memory[which]));
    CHECK(vkBindImageMemory(G.device, s->image[which], s->memory[which], 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->image[which],
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ambient.map_format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(G.device, &vi, NULL, &s->view[which]));
    VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = ambient.map_pass,
        .attachmentCount = 1, .pAttachments = &s->view[which], .width = AMBIENT_MAP_W, .height = AMBIENT_MAP_H, .layers = 1};
    CHECK(vkCreateFramebuffer(G.device, &fi, NULL, &s->framebuffer[which]));
    s->layout[which] = VK_IMAGE_LAYOUT_UNDEFINED;
}

static VkShaderModule ambient_module(const uint32_t *code, size_t bytes) {
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    CHECK(vkCreateShaderModule(G.device, &ci, NULL, &module));
    return module;
}

static VkPipeline ambient_pipeline(VkRenderPass pass, const uint32_t *frag, size_t frag_bytes,
                                   int dynamic, int alpha_blend, VkExtent2D extent) {
    VkShaderModule vert = ambient_module(filter_ambient_vert, sizeof filter_ambient_vert);
    VkShaderModule module = ambient_module(frag, frag_bytes);
    VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = module, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0, 0, (float)extent.width, (float)extent.height, 0, 1};
    VkRect2D scissor = {{0, 0}, extent};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_state = {.blendEnable = alpha_blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_state};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = dynamic ? 2u : 0u, .pDynamicStates = dynamic_states};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vin, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDepthStencilState = &ds, .pDynamicState = &dyn,
        .layout = ambient.layout, .renderPass = pass};
    VkPipeline pipeline = VK_NULL_HANDLE;
    CHECK(vkCreateGraphicsPipelines(G.device, VK_NULL_HANDLE, 1, &gi, NULL, &pipeline));
    return pipeline;
}

/* Build the objects that depend on the swapchain format. The light-map pass,
 * descriptor layout and sampler stay valid across surface replacement. */
static void ambient_build(void) {
    (void)ambient_gain();
    if (!ambient.enabled) return;
    if (!ambient.set_layout) {
        ambient.map_format = ambient_map_format();
        VkFormatProperties sp;
        vkGetPhysicalDeviceFormatProperties(G.physical, G.swap_format, &sp);
        if (ambient.map_format == VK_FORMAT_UNDEFINED || !swap_color_attachment ||
            !(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
            !(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) {
            ambient.enabled = 0;
            fprintf(stderr, "[nvn-vulkan] AMBIENT_BARS unavailable map=%d swap=%d\n", ambient.map_format, G.swap_format);
            return;
        }
        VkAttachmentDescription attachment = {.format = ambient.map_format, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref};
        VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
            .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &sub};
        CHECK(vkCreateRenderPass(G.device, &ri, NULL, &ambient.map_pass));

        VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 3, .pBindings = bindings};
        CHECK(vkCreateDescriptorSetLayout(G.device, &li, NULL, &ambient.set_layout));
        VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &ambient.set_layout};
        CHECK(vkCreatePipelineLayout(G.device, &pi, NULL, &ambient.layout));
        ambient.map_pipeline = ambient_pipeline(ambient.map_pass,
            filter_ambient_map_frag, sizeof filter_ambient_map_frag, 0, 0, (VkExtent2D){AMBIENT_MAP_W, AMBIENT_MAP_H});
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE};
        CHECK(vkCreateSampler(G.device, &si, NULL, &ambient.sampler));
    }
    if (ambient.bar_pipeline && ambient.bar_format != G.swap_format) {
        vkDestroyPipeline(G.device, ambient.bar_pipeline, NULL);
        vkDestroyRenderPass(G.device, ambient.bar_pass, NULL);
        ambient.bar_pipeline = VK_NULL_HANDLE;
        ambient.bar_pass = VK_NULL_HANDLE;
    }
    if (!ambient.bar_pipeline) {
        VkAttachmentDescription attachment = {.format = G.swap_format, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref};
        VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
            .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &sub};
        CHECK(vkCreateRenderPass(G.device, &ri, NULL, &ambient.bar_pass));
        ambient.bar_pipeline = ambient_pipeline(ambient.bar_pass,
            filter_ambient_bars_frag, sizeof filter_ambient_bars_frag, 1, 1, G.swap_extent);
        ambient.bar_format = G.swap_format;
    }
    ambient.ready = 1;
}

/* Swapchain image views and bar framebuffers are recreated with the surface. */
static void ambient_swapchain_reset(void) {
    for (unsigned i = 0; i < 16; i++) {
        if (ambient.swap_framebuffer[i]) vkDestroyFramebuffer(G.device, ambient.swap_framebuffer[i], NULL);
        if (ambient.swap_view[i]) vkDestroyImageView(G.device, ambient.swap_view[i], NULL);
        ambient.swap_framebuffer[i] = VK_NULL_HANDLE;
        ambient.swap_view[i] = VK_NULL_HANDLE;
    }
}

static void ambient_slot_create(ambient_slot *s) {
    if (s->image[0]) return;
    ambient_image_create(s, 0);
    ambient_image_create(s, 1);
    s->ubo = buffer_make(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    CHECK(nvn_vk_descriptor_allocate(&ambient.sets, G.device, ambient.set_layout, &s->map_set));
    CHECK(nvn_vk_descriptor_allocate(&ambient.sets, G.device, ambient.set_layout, &s->bar_set));
}

/* Both maps start black in SHADER_READ_ONLY, so the first frame can sample the
 * previous map even though its weight is 1. */
static void ambient_slot_init(VkCommandBuffer cmd, ambient_slot *s) {
    VkImageMemoryBarrier barriers[2];
    for (int i = 0; i < 2; i++) {
        barriers[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .image = s->image[i],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 2, barriers);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (int i = 0; i < 2; i++) vkCmdClearColorImage(cmd, s->image[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    for (int i = 0; i < 2; i++) {
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        s->layout[i] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 2, barriers);
}

/* Show the picture at its source aspect inside the full-screen surface. */
static void ambient_picture_rect(uint32_t src_width, uint32_t src_height, VkExtent2D target, int rect[4]) {
    float aspect = src_height ? (float)src_width / (float)src_height : 16.0f / 9.0f;
    float w = (float)target.width, h = w / aspect;
    if (h > (float)target.height) { h = (float)target.height; w = h * aspect; }
    rect[0] = (int)((target.width - w) * 0.5f);
    rect[1] = (int)((target.height - h) * 0.5f);
    rect[2] = (int)w;
    rect[3] = (int)h;
}

/* Returns the number of bar rectangles that the bar pass will draw (0 = none). */
static int ambient_bar_rects(VkExtent2D size, const int rect[4], VkRect2D out[2]) {
    int n = 0;
    if (rect[1] >= 2) out[n++] = (VkRect2D){{0, 0}, {size.width, (uint32_t)rect[1]}};
    if (rect[1] + rect[3] + 2 <= (int)size.height)
        out[n++] = (VkRect2D){{0, (int32_t)(rect[1] + rect[3])}, {size.width, (uint32_t)((int)size.height - rect[1] - rect[3])}};
    if (rect[0] >= 2) out[n++] = (VkRect2D){{0, 0}, {(uint32_t)rect[0], size.height}};
    if (rect[0] + rect[2] + 2 <= (int)size.width)
        out[n++] = (VkRect2D){{rect[0] + rect[2], 0}, {(uint32_t)((int)size.width - rect[0] - rect[2]), size.height}};
    return n;
}

static void ambient_fill(ambient_uniforms *u, VkExtent2D size, const int rect[4], const int crop[4],
                         uint32_t tex_w, uint32_t tex_h, int flip, float gain, float alpha) {
    memset(u, 0, sizeof *u);
    u->rect[0] = (float)rect[0]; u->rect[1] = (float)rect[1];
    u->rect[2] = (float)rect[2]; u->rect[3] = (float)rect[3];
    float u0 = (float)crop[0] / (float)tex_w, v0 = (float)crop[1] / (float)tex_h;
    float u1 = (float)(crop[0] + crop[2]) / (float)tex_w, v1 = (float)(crop[1] + crop[3]) / (float)tex_h;
    float du = u1 - u0, dv = v1 - v0;
    if (flip) { u->uv[0] = u0; u->uv[1] = v1; u->uv[2] = du; u->uv[3] = -dv; }
    else { u->uv[0] = u0; u->uv[1] = v0; u->uv[2] = du; u->uv[3] = dv; }
    u->edge[0] = u0; u->edge[1] = v0; u->edge[2] = u1; u->edge[3] = v1;
    u->params[0] = (float)size.width; u->params[1] = (float)size.height;
    u->params[2] = rect[1] >= 2 ? 0.0f : 1.0f;   /* 0 = top/bottom, 1 = left/right */
    u->params[3] = gain;
    u->light[0] = alpha; u->light[1] = AMBIENT_MAP_W; u->light[2] = AMBIENT_MAP_H;
}

static float ambient_alpha(uint64_t now, uint64_t *last) {
    if (!*last || now <= *last || now - *last > 500000000ull) { *last = now; return 1.0f; }
    double dt = (double)(now - *last) / 1e9;
    *last = now;
    return (float)(1.0 - exp(-dt / AMBIENT_TAU_S));
}

/* Record the light-map pass for this swapchain image. Must run before the blit
 * (the source image is transitioned to SHADER_READ_ONLY here). */
static int ambient_prepare(VkCommandBuffer cmd, VTexture *src, const int crop[4], uint32_t swap_index,
                           const int rect[4], VkRect2D bars[4], int flip, uint64_t now) {
    if (!ambient.enabled || !ambient.ready) return 0;
    int count = ambient_bar_rects(G.swap_extent, rect, bars);
    if (!count) return 0;
    ambient_slot *s = &ambient.slots[swap_index % 16];
    ambient_slot_create(s);
    if (s->uses == 0) ambient_slot_init(cmd, s);
    ambient_uniforms u;
    float alpha = ambient_alpha(now, &s->last_ns);
    ambient_fill(&u, G.swap_extent, rect, crop, src->width, src->height, flip, ambient.gain, alpha);
    int phase = (int)(s->uses & 1), previous = phase ^ 1;
    memcpy(s->ubo.map, &u, sizeof u);

    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = s->layout[phase], .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .image = s->image[phase], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    s->layout[phase] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    /* The previous map was written by an earlier frame. Order that colour
     * attachment write before this pass samples it. */
    VkImageMemoryBarrier previous_read = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .image = s->image[previous], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &previous_read);

    VkDescriptorBufferInfo buffer = {s->ubo.b, 0, sizeof u};
    VkDescriptorImageInfo source = {ambient.sampler, src->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo old_map = {ambient.sampler, s->view[previous], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo new_map = {ambient.sampler, s->view[phase], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[5] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->map_set, .dstBinding = 0,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &buffer},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->map_set, .dstBinding = 1,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &source},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->map_set, .dstBinding = 2,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &old_map},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->bar_set, .dstBinding = 0,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &buffer},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->bar_set, .dstBinding = 1,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &new_map}};
    vkUpdateDescriptorSets(G.device, 5, writes, 0, NULL);

    VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = ambient.map_pass,
        .framebuffer = s->framebuffer[phase], .renderArea = {{0, 0}, {AMBIENT_MAP_W, AMBIENT_MAP_H}}};
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ambient.map_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ambient.layout, 0, 1, &s->map_set, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    s->layout[phase] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return count;
}

/* Draw the ambient background and feather into the bars. The swapchain image
 * must be in COLOR_ATTACHMENT_OPTIMAL and keeps that layout on return. */
static void ambient_draw(VkCommandBuffer cmd, uint32_t swap_index, const int rect[4],
                         const VkRect2D *bars, int count) {
    if (!ambient.enabled || !ambient.ready || !count) return;
    ambient_slot *s = &ambient.slots[swap_index % 16];
    int phase = (int)(s->uses & 1);
    VkImageMemoryBarrier map_read = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .image = s->image[phase], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &map_read);
    if (!ambient.swap_framebuffer[swap_index]) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = G.swap_images[swap_index], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = G.swap_format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(G.device, &vi, NULL, &ambient.swap_view[swap_index]));
        VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = ambient.bar_pass,
            .attachmentCount = 1, .pAttachments = &ambient.swap_view[swap_index],
            .width = G.swap_extent.width, .height = G.swap_extent.height, .layers = 1};
        CHECK(vkCreateFramebuffer(G.device, &fi, NULL, &ambient.swap_framebuffer[swap_index]));
    }
    VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = ambient.bar_pass,
        .framebuffer = ambient.swap_framebuffer[swap_index], .renderArea = {{0, 0}, G.swap_extent}};
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = {0, 0, (float)G.swap_extent.width, (float)G.swap_extent.height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ambient.bar_pipeline);
    VkDescriptorSet set = s->bar_set;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ambient.layout, 0, 1, &set, 0, NULL);
    int feather = (int)(0.08f * (rect[2] < rect[3] ? rect[2] : rect[3]));
    for (int i = 0; i < count; i++) {
        /* Include the feathered picture edge in the same draw; the shader
         * blends the blurred background over it. */
        VkRect2D scissor = bars[i];
        if (scissor.extent.width == G.swap_extent.width) {
            if (scissor.offset.y == 0) scissor.extent.height += feather;
            else { scissor.offset.y -= feather; scissor.extent.height += feather; }
        } else {
            if (scissor.offset.x == 0) scissor.extent.width += feather;
            else { scissor.offset.x -= feather; scissor.extent.width += feather; }
        }
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
    s->uses++;
}
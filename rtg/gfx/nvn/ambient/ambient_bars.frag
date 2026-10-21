#version 450
/* Ambient letterbox background.
 *
 * A low-resolution, temporally smoothed copy of the whole scene fills the
 * screen. The sharp 16:9 picture sits on top; its outer edge fades into the
 * same blurred background, so the letterbox reads as the picture continuing
 * softly instead of a mirrored duplicate or a glow strip. */
layout(location = 0) out vec4 outColor;
layout(std140, set = 0, binding = 0) uniform A {
    vec4 rect; vec4 uv; vec4 edge; vec4 params; vec4 light;
} a;
layout(set = 0, binding = 1) uniform sampler2D mapTex;

void main() {
    vec2 size = a.params.xy, px = gl_FragCoord.xy;
    vec4 r = a.rect;
    vec3 bg = texture(mapTex, px / size).rgb;
    float gain = a.params.w;
    bool inside = px.x > r.x && px.x < r.x + r.z && px.y > r.y && px.y < r.y + r.w;
    if (inside) {
        float dl = px.x - r.x, dr = r.x + r.z - px.x;
        float dt = px.y - r.y, db = r.y + r.w - px.y;
        float dmin = min(min(dl, dr), min(dt, db));
        float feather = 0.08 * min(r.z, r.w);
        if (dmin < feather) {
            float alpha = 1.0 - smoothstep(0.0, 1.0, dmin / feather);
            vec3 c = bg * gain * 0.85;
            outColor = vec4(pow(clamp(c, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2)), alpha);
            return;
        }
        discard;
    }
    bool horiz = a.params.z < 0.5;
    bool first = horiz ? px.y < r.y : px.x < r.x;
    float d = horiz ? (first ? r.y - px.y : px.y - r.y - r.w)
                    : (first ? r.x - px.x : px.x - r.x - r.z);
    float bar = horiz ? (first ? r.y : size.y - r.y - r.w)
                      : (first ? r.x : size.x - r.x - r.z);
    float x = clamp(d / max(bar, 1.0), 0.0, 1.0);
    /* Keep the background close to the picture brightness and only fade at the
     * very screen edge, so wide phone bars do not read as a black frame. */
    float fade = 1.0 - smoothstep(0.88, 1.0, x);
    vec3 c = bg * gain * 0.85 * (1.0 - 0.30 * x) * fade;
    outColor = vec4(pow(clamp(c, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2)), 1.0);
}
#version 450
/* 64 x 36 light map: each cell averages 8 x 8 samples of the picture in
 * linear light, then blends into the previous frame's map. */
layout(location = 0) out vec4 outColor;
layout(std140, set = 0, binding = 0) uniform A {
    vec4 rect; vec4 uv; vec4 edge; vec4 params; vec4 light;
} a;
layout(set = 0, binding = 1) uniform sampler2D src;
layout(set = 0, binding = 2) uniform sampler2D prev;
void main() {
    vec2 cell = 1.0 / a.light.yz;
    vec2 p0 = floor(gl_FragCoord.xy) * cell;
    vec3 sum = vec3(0.0);
    for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 8; ++i) {
            vec2 q = p0 + (vec2(i, j) + 0.5) * (cell / 8.0);
            vec2 t = clamp(q * a.uv.zw + a.uv.xy, a.edge.xy, a.edge.zw);
            sum += pow(max(texture(src, t).rgb, vec3(0.0)), vec3(2.2));
        }
    vec3 current = sum / 64.0;
    vec3 previous = texture(prev, gl_FragCoord.xy * cell).rgb;
    outColor = vec4(mix(previous, current, a.light.x), 1.0);
}

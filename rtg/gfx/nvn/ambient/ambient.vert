#version 450
/* Fullscreen triangle for the light-map and bar passes. */
void main() {
    vec2 q = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(q * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}

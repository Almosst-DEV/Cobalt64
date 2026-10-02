#version 450
// N48 fallback vertex shader: no attribute input; every triangle of the draw covers the whole target (vertex id % 3).
void main() {
    int i = gl_VertexIndex % 3;
    vec2 p = vec2(float((i << 1) & 2), float(i & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

#version 450
layout(constant_id = 0) const int full = 0;
void main() {
    // full = 1: a triangle covering the whole target (every tile is touched on both sides of a
    // pass boundary, the G-buffer case); full = 0: a small triangle in one corner.
    const vec2 small[3] = vec2[](vec2(-1.0, -1.0), vec2(-0.9, -1.0), vec2(-1.0, -0.9));
    const vec2 big[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(full != 0 ? big[gl_VertexIndex % 3] : small[gl_VertexIndex % 3], 0.5, 1.0);
}

#version 450
// The copy shader's param0 (location 0), flat: the provoking vertex's value.
layout(location = 0) flat in vec4 color;
layout(location = 0) out vec4 result;
void main() { result = color; }

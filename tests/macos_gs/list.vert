#version 450
// Reference: the model's primitives as a plain list (position, param0).
layout(location = 0) in vec4 position;
layout(location = 1) in vec4 param0;
layout(location = 0) flat out vec4 color;
void main() {
    gl_Position = position;
    color = param0;
}

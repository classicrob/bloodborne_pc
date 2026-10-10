#version 450
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
void main() { o0 = vec4(1); o1 = vec4(0.5); o2 = vec4(0.25); o3 = vec4(0.125); }

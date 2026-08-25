#version 450

layout(location = 0) in vec3 a_Pos;
layout(location = 1) in vec2 a_UV;
layout(location = 2) in vec4 a_Color;

layout(location = 0) out vec2 v_UV;
layout(location = 1) out vec4 v_Color;

void main()
{
    gl_Position = vec4(a_Pos, 1.0);
    v_UV = a_UV;
    v_Color = a_Color;
}

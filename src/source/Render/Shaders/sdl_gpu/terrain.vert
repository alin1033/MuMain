#version 450

layout(location = 0) in vec3 a_Pos;
layout(location = 1) in vec3 a_Light;

layout(std140, set = 1, binding = 0) uniform GlobalMatrices {
    mat4 u_View;
    mat4 u_Proj;
    mat4 u_Model;
    mat4 u_MVP;
    vec4 u_Time;
};

layout(location = 0) out vec2 v_UV;
layout(location = 1) out vec4 v_Color;

void main()
{
    vec4 clip = u_MVP * vec4(a_Pos, 1.0);
    clip.y = -clip.y;
    gl_Position = clip;
    v_UV = a_Pos.xy * 0.5 + 0.5;
    v_Color = vec4(a_Light, 1.0);
}

#version 450

// SDL_GPU reserves descriptor sets 0/1 for vertex resources and 2/3 for
// fragment resources when consuming SPIR-V.
layout(set = 2, binding = 0) uniform sampler2D u_Texture;

layout(location = 0) in vec2 v_UV;
layout(location = 1) in vec4 v_Color;

layout(location = 0) out vec4 o_Color;

void main()
{
    o_Color = texture(u_Texture, v_UV) * v_Color;
}

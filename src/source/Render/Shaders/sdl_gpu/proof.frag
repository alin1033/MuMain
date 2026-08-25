#version 450

// SDL_GPU reserves descriptor sets 0/1 for vertex resources and 2/3 for
// fragment resources when consuming SPIR-V.
layout(set = 2, binding = 0) uniform sampler2D u_Texture;

layout(set = 3, binding = 0, std140) uniform PassthroughState {
    uint useTexture;
    uint useFog;
    float alphaRef;
    uint texCombineAdd;
};

layout(location = 0) in vec2 v_UV;
layout(location = 1) in vec4 v_Color;

layout(location = 0) out vec4 o_Color;

void main()
{
    vec4 color = clamp(v_Color, 0.0, 1.0);
    vec4 sampleColor = useTexture != 0u ? texture(u_Texture, v_UV) : vec4(1.0);
    o_Color = texCombineAdd != 0u
        ? vec4(sampleColor.rgb + color.rgb, sampleColor.a * color.a)
        : sampleColor * color;
    if (alphaRef >= 0.0 && o_Color.a <= alphaRef)
        discard;
}

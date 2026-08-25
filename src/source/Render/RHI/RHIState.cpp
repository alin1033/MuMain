#include "Render/RHI/RHIState.h"

namespace RHI {

BlendStateDesc DescribeBlendMode(BlendMode mode)
{
    switch (mode)
    {
    case BlendMode::Opaque:
        return {false, BlendFactor::One, BlendFactor::Zero, true, true, false, true};
    case BlendMode::LightMap:
        return {true, BlendFactor::Zero, BlendFactor::SrcColor, true, true, false, true};
    case BlendMode::AlphaTest:
        return {true, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha,
            false, true, true, true};
    case BlendMode::Additive:
        return {true, BlendFactor::One, BlendFactor::One, false, false, false, false};
    case BlendMode::Minus:
        return {true, BlendFactor::Zero, BlendFactor::OneMinusSrcColor,
            false, false, false, true};
    case BlendMode::Blend2:
        return {true, BlendFactor::OneMinusSrcColor, BlendFactor::One,
            false, false, false, true};
    case BlendMode::Blend3:
        return {true, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha,
            false, false, false, true};
    case BlendMode::Blend4:
        return {true, BlendFactor::One, BlendFactor::OneMinusSrcColor,
            false, false, false, true};
    }

    return {};
}

VertexLayoutDesc DescribeVertexLayout(VertexLayout layout)
{
    switch (layout)
    {
    case VertexLayout::PosUvColor:
    case VertexLayout::BMDMesh:
        return {
            36,
            3,
            {{{0, VertexElementFormat::Float3, 0},
              {1, VertexElementFormat::Float2, 12},
              {2, VertexElementFormat::Float4, 20}}},
        };
    case VertexLayout::Terrain:
        return {
            24,
            2,
            {{{0, VertexElementFormat::Float3, 0},
              {1, VertexElementFormat::Float3, 12}}},
        };
    case VertexLayout::PosOnly:
        return {
            12,
            1,
            {{{0, VertexElementFormat::Float3, 0}}},
        };
    }

    return {};
}

std::size_t HashPipelineKey(const PipelineKey& key)
{
    // The fields are all small enums/bools. Pack them explicitly so padding and
    // compiler ABI never affect cache identity or the focused unit tests.
    std::size_t value = static_cast<std::size_t>(key.shader);
    value = (value << 3U) | static_cast<std::size_t>(key.vertexLayout);
    value = (value << 2U) | static_cast<std::size_t>(key.topology);
    value = (value << 3U) | static_cast<std::size_t>(key.blendMode);
    value = (value << 1U) | static_cast<std::size_t>(key.depthTestEnabled);
    value = (value << 1U) | static_cast<std::size_t>(key.cullEnabled);
    value = (value << 1U) | static_cast<std::size_t>(key.depthWriteEnabled);
    value = (value << 1U) | static_cast<std::size_t>(key.polygonOffsetEnabled);
    return value;
}

} // namespace RHI

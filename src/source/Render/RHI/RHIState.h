#pragma once

#include "Render/RHI/RHI.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace RHI {

enum class BlendFactor : std::uint8_t {
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    SrcAlpha,
    OneMinusSrcAlpha,
};

struct BlendStateDesc {
    bool enabled = false;
    BlendFactor source = BlendFactor::One;
    BlendFactor destination = BlendFactor::Zero;
    bool cullEnabled = true;
    bool depthWriteEnabled = true;
    bool alphaTestEnabled = false;
    bool fogEnabled = true;

    bool operator==(const BlendStateDesc&) const = default;
};

BlendStateDesc DescribeBlendMode(BlendMode mode);

enum class VertexElementFormat : std::uint8_t {
    Float,
    Float2,
    Float3,
    Float4,
    Int,
};

struct VertexAttributeDesc {
    std::uint8_t location = 0;
    VertexElementFormat format = VertexElementFormat::Float;
    std::uint16_t offset = 0;

    bool operator==(const VertexAttributeDesc&) const = default;
};

struct VertexLayoutDesc {
    std::uint16_t stride = 0;
    std::uint8_t attributeCount = 0;
    std::array<VertexAttributeDesc, 5> attributes{};

    bool operator==(const VertexLayoutDesc&) const = default;
};

VertexLayoutDesc DescribeVertexLayout(VertexLayout layout);

// Every immutable choice that SDL_GPU bakes into a graphics pipeline. Dynamic
// viewport/scissor and resource bindings deliberately do not belong here.
struct PipelineKey {
    ShaderProgram shader = ShaderProgram::Passthrough;
    VertexLayout vertexLayout = VertexLayout::PosUvColor;
    Topology topology = Topology::TriangleList;
    BlendMode blendMode = BlendMode::Opaque;
    bool depthTestEnabled = true;
    bool cullEnabled = true;
    bool depthWriteEnabled = true;
    bool polygonOffsetEnabled = false;

    bool operator==(const PipelineKey&) const = default;
};

std::size_t HashPipelineKey(const PipelineKey& key);

} // namespace RHI

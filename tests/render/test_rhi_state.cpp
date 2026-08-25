#include "doctest.h"
#include "Render/RHI/RHIState.h"

TEST_CASE("all eight legacy blend modes translate to immutable RHI state")
{
    using namespace RHI;

    CHECK(DescribeBlendMode(BlendMode::Opaque) ==
        BlendStateDesc{false, BlendFactor::One, BlendFactor::Zero, true, true, false, true});
    CHECK(DescribeBlendMode(BlendMode::LightMap) ==
        BlendStateDesc{true, BlendFactor::Zero, BlendFactor::SrcColor, true, true, false, true});
    CHECK(DescribeBlendMode(BlendMode::AlphaTest) ==
        BlendStateDesc{true, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha,
            false, true, true, true});
    CHECK(DescribeBlendMode(BlendMode::Additive).destination == BlendFactor::One);
    CHECK(DescribeBlendMode(BlendMode::Minus).destination == BlendFactor::OneMinusSrcColor);
    CHECK(DescribeBlendMode(BlendMode::Blend2).source == BlendFactor::OneMinusSrcColor);
    CHECK(DescribeBlendMode(BlendMode::Blend3).source == BlendFactor::SrcAlpha);
    CHECK(DescribeBlendMode(BlendMode::Blend4).source == BlendFactor::One);
}

TEST_CASE("the four declared vertex layouts have stable strides and attributes")
{
    using namespace RHI;

    const auto immediate = DescribeVertexLayout(VertexLayout::PosUvColor);
    CHECK(immediate.stride == 36);
    CHECK(immediate.attributeCount == 3);
    CHECK(immediate.attributes[2] ==
        VertexAttributeDesc{2, VertexElementFormat::Float4, 20});

    CHECK(DescribeVertexLayout(VertexLayout::BMDMesh).stride == 36);
    CHECK(DescribeVertexLayout(VertexLayout::Terrain).stride == 24);
    CHECK(DescribeVertexLayout(VertexLayout::Terrain).attributeCount == 2);
    CHECK(DescribeVertexLayout(VertexLayout::PosOnly).stride == 12);
    CHECK(DescribeVertexLayout(VertexLayout::PosOnly).attributeCount == 1);
}

TEST_CASE("pipeline cache identity covers every immutable state field")
{
    using namespace RHI;

    const PipelineKey base{};
    PipelineKey changed = base;
    changed.shader = ShaderProgram::Terrain;
    CHECK(HashPipelineKey(changed) != HashPipelineKey(base));
    changed = base;
    changed.vertexLayout = VertexLayout::Terrain;
    CHECK(HashPipelineKey(changed) != HashPipelineKey(base));
    changed = base;
    changed.topology = Topology::LineList;
    CHECK(HashPipelineKey(changed) != HashPipelineKey(base));
    changed = base;
    changed.blendMode = BlendMode::Blend4;
    CHECK(HashPipelineKey(changed) != HashPipelineKey(base));
    changed = base;
    changed.depthTestEnabled = false;
    CHECK(HashPipelineKey(changed) != HashPipelineKey(base));
    changed = base;
    changed.cullEnabled = false;
    CHECK(HashPipelineKey(changed) != HashPipelineKey(base));
    changed = base;
    changed.depthWriteEnabled = false;
    CHECK(HashPipelineKey(changed) != HashPipelineKey(base));
    changed = base;
    changed.polygonOffsetEnabled = true;
    CHECK(HashPipelineKey(changed) != HashPipelineKey(base));
}

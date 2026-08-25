#include "stdafx.h"
#include "Render/RHI/RHI.h"
#include "Core/Utilities/Log/ErrorReport.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace RHI {

#if defined(MU_RENDER_BACKEND_SDL_GPU)

namespace {

struct PosUvColorVertex {
    float position[3];
    float uv[2];
    float color[4];
};

struct TerrainVertex {
    float position[3];
    float light[3];
};

struct PosOnlyVertex {
    float position[3];
};

struct GlobalMatrices {
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    std::array<float, 16> model{};
    std::array<float, 16> mvp{};
    std::array<float, 4> time{};
};

void MakeIdentity(std::array<float, 16>& matrix)
{
    matrix = {};
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

void LogSelfTestReady()
{
    g_ErrorReport.Write(L"> SDL_GPU full RHI self-test ready: layouts=4 blends=8 indexed=1.\r\n");
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "MuMainGPU",
        "full RHI self-test ready layouts=4 blends=8 indexed=1");
#endif
}

} // namespace

bool RunSdlGpuProof(void* nativeWindowHandle)
{
    auto* window = static_cast<SDL_Window*>(nativeWindowHandle);
    int width = 1;
    int height = 1;
    if (!window || !SDL_GetWindowSizeInPixels(window, &width, &height)
        || !Init(window, width, height))
        return false;

    constexpr std::array<PosUvColorVertex, 4> quad = {{
        {{-0.75f, -0.75f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{ 0.75f, -0.75f, 0.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{ 0.75f,  0.75f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {{-0.75f,  0.75f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    }};
    constexpr std::array<std::uint32_t, 6> indices = {0, 1, 2, 0, 2, 3};
    constexpr std::array<TerrainVertex, 3> terrainTriangle = {{
        {{-0.72f, -0.68f, 0.0f}, {1.0f, 0.35f, 0.35f}},
        {{-0.45f, -0.68f, 0.0f}, {0.35f, 1.0f, 0.35f}},
        {{-0.58f, -0.38f, 0.0f}, {0.35f, 0.55f, 1.0f}},
    }};
    constexpr std::array<PosOnlyVertex, 3> posOnlyTriangle = {{
        {{0.45f, -0.68f, 0.0f}},
        {{0.72f, -0.68f, 0.0f}},
        {{0.58f, -0.38f, 0.0f}},
    }};
    constexpr std::array<std::uint8_t, 16> checker = {
        255, 48, 48, 255, 48, 255, 96, 255,
        48, 96, 255, 255, 255, 224, 48, 255,
    };

    const BufferHandle quadBuffer = CreateVertexBuffer(
        quad.data(), sizeof(quad), BufferUsage::Static);
    const BufferHandle terrainBuffer = CreateVertexBuffer(
        terrainTriangle.data(), sizeof(terrainTriangle), BufferUsage::Static);
    const BufferHandle posOnlyBuffer = CreateVertexBuffer(
        posOnlyTriangle.data(), sizeof(posOnlyTriangle), BufferUsage::Static);
    const BufferHandle indexBuffer = CreateIndexBuffer(
        indices.data(), sizeof(indices), BufferUsage::Static);
    const TextureHandle texture = CreateTexture({2, 2, TexFilter::Nearest, TexWrap::Clamp},
        checker.data());

    GlobalMatrices matrices;
    MakeIdentity(matrices.view);
    MakeIdentity(matrices.projection);
    MakeIdentity(matrices.model);
    MakeIdentity(matrices.mvp);
    const BufferHandle global = CreateUniformBlock(sizeof(matrices), 0);
    UpdateUniformBlock(global, &matrices, sizeof(matrices));

    if (!quadBuffer.IsValid() || !terrainBuffer.IsValid() || !posOnlyBuffer.IsValid()
        || !indexBuffer.IsValid() || !texture.IsValid() || !global.IsValid())
    {
        Shutdown();
        return false;
    }

    LogSelfTestReady();
    bool running = true;
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT
                || (event.type == SDL_EVENT_KEY_DOWN
                    && (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_AC_BACK)))
                running = false;
        }
        if (!running) break;

        BeginFrame();
        SetViewport(0, 0, width, height);
        SetScissorEnabled(false);
        Clear(true, true, 0.025f, 0.04f, 0.09f, 1.0f);
        SetDepthTestEnabled(false);
        SetShaderProgram(ShaderProgram::Passthrough);
        BindTexture(texture, 0);
        BindVertexBuffer(quadBuffer, VertexLayout::PosUvColor);
        BindIndexBuffer(indexBuffer);

        // Exercise every immutable legacy blend translation. The final opaque
        // draw leaves a stable image while all eight pipelines remain cached.
        for (int mode = 0; mode < 8; ++mode)
        {
            SetBlendMode(static_cast<BlendMode>(mode));
            DrawIndexed(Topology::TriangleList, static_cast<std::uint32_t>(indices.size()));
        }
        SetBlendMode(BlendMode::Opaque);
        DrawIndexed(Topology::TriangleList, static_cast<std::uint32_t>(indices.size()));

        SetShaderProgram(ShaderProgram::BMDMesh);
        BindVertexBuffer(quadBuffer, VertexLayout::BMDMesh);
        Draw(Topology::TriangleList, 3);
        SetShaderProgram(ShaderProgram::Terrain);
        BindVertexBuffer(terrainBuffer, VertexLayout::Terrain);
        Draw(Topology::TriangleList, 3);
        SetShaderProgram(ShaderProgram::PlanarShadow);
        BindVertexBuffer(posOnlyBuffer, VertexLayout::PosOnly);
        Draw(Topology::TriangleList, 3);
        EndFrame();
    }

    DestroyUniformBlock(global);
    DestroyTexture(texture);
    DestroyBuffer(indexBuffer);
    DestroyBuffer(posOnlyBuffer);
    DestroyBuffer(terrainBuffer);
    DestroyBuffer(quadBuffer);
    Shutdown();
    return true;
}

#else

bool RunSdlGpuProof(void*)
{
    return false;
}

#endif

} // namespace RHI

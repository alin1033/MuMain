// RHI dispatch shim: this file owns the actual `namespace RHI { ... }` that satisfies
// Render/RHI/RHI.h's declarations. Compile-time backend selection keeps the
// legacy call surface stable while routing it to OpenGL or SDL_GPU.

#include "stdafx.h"
#include "Render/RHI/RHI.h"
#include "Render/Core/RenderConfig.h"
#include "Core/Utilities/FrameProfiler.h"

#if defined(MU_RENDER_BACKEND_SDL_GPU)
namespace RHI_SDL_GPU_Impl {
#define RHI_BACKEND RHI_SDL_GPU_Impl
#else
namespace RHI_GL_Impl {
#define RHI_BACKEND RHI_GL_Impl
#endif
    const RHI::Caps& GetCaps();

    bool Init(void* nativeWindowHandle, int width, int height);
    void Shutdown();
    void BeginFrame();
    void EndFrame();
    void SetViewport(int x, int y, int w, int h);
    void SetScissorEnabled(bool enabled);
    void SetScissorRect(int x, int y, int w, int h);
    void OnResize(int width, int height);
    void Clear(bool color, bool depth, float r, float g, float b, float a);

    RHI::BufferHandle CreateVertexBuffer(const void* initialData, size_t sizeBytes, RHI::BufferUsage usage);
    RHI::BufferHandle CreateIndexBuffer(const void* initialData, size_t sizeBytes, RHI::BufferUsage usage);
    void UpdateBuffer(RHI::BufferHandle handle, const void* data, size_t sizeBytes);
    size_t AppendBuffer(RHI::BufferHandle handle, const void* data, size_t sizeBytes);
    void DestroyBuffer(RHI::BufferHandle handle);

    RHI::BufferHandle CreateUniformBlock(size_t sizeBytes, int bindingSlot);
    void UpdateUniformBlock(RHI::BufferHandle handle, const void* data, size_t sizeBytes);
    void DestroyUniformBlock(RHI::BufferHandle handle);

    RHI::TextureHandle CreateTexture(const RHI::TextureDesc& desc, const void* initialPixelsRGBA);
    void UpdateTexture(RHI::TextureHandle handle, int x, int y, int w, int h, const void* pixelsRGBA);
    void DestroyTexture(RHI::TextureHandle handle);
    void BindTexture(RHI::TextureHandle handle, int slot);

    void SetBlendMode(RHI::BlendMode mode);
    void SetDepthTestEnabled(bool enabled);
    void SetCullEnabled(bool enabled);
    void SetDepthWriteEnabled(bool enabled);
    void SetFogEnabled(bool enabled);
    void SetPolygonOffset(bool enabled, float factor, float units);
    void SetShaderProgram(RHI::ShaderProgram shader);
    void SetPassthroughState(const RHI::PassthroughState& state);

    void BindVertexBuffer(RHI::BufferHandle handle, RHI::VertexLayout layout);
    void BindIndexBuffer(RHI::BufferHandle handle);

    void Draw(RHI::Topology topology, uint32_t vertexCount, uint32_t firstVertex);
    void DrawIndexed(RHI::Topology topology, uint32_t indexCount, uint32_t firstIndex);

    bool ReadDepthPixel(int x, int y, float* outDepth);
    bool ReadColorFramebuffer(int x, int y, int w, int h, void* outRGB);
}

namespace RHI {

Backend GetConfiguredBackend()
{
#if defined(MU_RENDER_BACKEND_OPENGL)
    return Backend::OpenGL;
#elif defined(MU_RENDER_BACKEND_SDL_GPU)
    return Backend::SdlGpu;
#else
#error "No MuMain renderer backend was selected by CMake."
#endif
}

const char* GetConfiguredBackendName()
{
    switch (GetConfiguredBackend())
    {
    case Backend::OpenGL:
        return "OpenGL";
    case Backend::SdlGpu:
        return "SDL_GPU";
    }

    return "Unknown";
}

const Caps& GetCaps()
{
    return RHI_BACKEND::GetCaps();
}

bool Init(void* nativeWindowHandle, int width, int height)
{
    return RHI_BACKEND::Init(nativeWindowHandle, width, height);
}

void Shutdown()
{
    RHI_BACKEND::Shutdown();
}

void BeginFrame()
{
    RHI_BACKEND::BeginFrame();
}

void EndFrame()
{
    RHI_BACKEND::EndFrame();
}

void SetViewport(int x, int y, int w, int h)
{
    RHI_BACKEND::SetViewport(x, y, w, h);
}

void SetScissorEnabled(bool enabled)
{
    RHI_BACKEND::SetScissorEnabled(enabled);
}

void SetScissorRect(int x, int y, int w, int h)
{
    RHI_BACKEND::SetScissorRect(x, y, w, h);
}

void OnResize(int width, int height)
{
    RHI_BACKEND::OnResize(width, height);
}

void Clear(bool color, bool depth, float r, float g, float b, float a)
{
    RHI_BACKEND::Clear(color, depth, r, g, b, a);
}

BufferHandle CreateVertexBuffer(const void* initialData, size_t sizeBytes, BufferUsage usage)
{
    if (initialData) FrameProfiler::CountSkip(FrameProfiler::Counter::UploadedBytes,
        static_cast<uint32_t>(sizeBytes));
    return RHI_BACKEND::CreateVertexBuffer(initialData, sizeBytes, usage);
}

BufferHandle CreateIndexBuffer(const void* initialData, size_t sizeBytes, BufferUsage usage)
{
    if (initialData) FrameProfiler::CountSkip(FrameProfiler::Counter::UploadedBytes,
        static_cast<uint32_t>(sizeBytes));
    return RHI_BACKEND::CreateIndexBuffer(initialData, sizeBytes, usage);
}

void UpdateBuffer(BufferHandle handle, const void* data, size_t sizeBytes)
{
    FrameProfiler::CountSkip(FrameProfiler::Counter::UploadedBytes,
        static_cast<uint32_t>(sizeBytes));
    RHI_BACKEND::UpdateBuffer(handle, data, sizeBytes);
}

size_t AppendBuffer(BufferHandle handle, const void* data, size_t sizeBytes)
{
    FrameProfiler::CountSkip(FrameProfiler::Counter::UploadedBytes,
        static_cast<uint32_t>(sizeBytes));
    return RHI_BACKEND::AppendBuffer(handle, data, sizeBytes);
}

void DestroyBuffer(BufferHandle handle)
{
    RHI_BACKEND::DestroyBuffer(handle);
}

BufferHandle CreateUniformBlock(size_t sizeBytes, int bindingSlot)
{
    return RHI_BACKEND::CreateUniformBlock(sizeBytes, bindingSlot);
}

void UpdateUniformBlock(BufferHandle handle, const void* data, size_t sizeBytes)
{
    FrameProfiler::CountSkip(FrameProfiler::Counter::UploadedBytes,
        static_cast<uint32_t>(sizeBytes));
    RHI_BACKEND::UpdateUniformBlock(handle, data, sizeBytes);
}

void DestroyUniformBlock(BufferHandle handle)
{
    RHI_BACKEND::DestroyUniformBlock(handle);
}

TextureHandle CreateTexture(const TextureDesc& desc, const void* initialPixelsRGBA)
{
    if (initialPixelsRGBA) FrameProfiler::CountSkip(FrameProfiler::Counter::UploadedBytes,
        static_cast<uint32_t>(desc.width * desc.height * 4));
    return RHI_BACKEND::CreateTexture(desc, initialPixelsRGBA);
}

void UpdateTexture(TextureHandle handle, int x, int y, int w, int h, const void* pixelsRGBA)
{
    FrameProfiler::CountSkip(FrameProfiler::Counter::UploadedBytes,
        static_cast<uint32_t>(w * h * 4));
    RHI_BACKEND::UpdateTexture(handle, x, y, w, h, pixelsRGBA);
}

void DestroyTexture(TextureHandle handle)
{
    RHI_BACKEND::DestroyTexture(handle);
}

void BindTexture(TextureHandle handle, int slot)
{
    RHI_BACKEND::BindTexture(handle, slot);
}

void SetBlendMode(BlendMode mode)
{
    RHI_BACKEND::SetBlendMode(mode);
}

void SetDepthTestEnabled(bool enabled)
{
    RHI_BACKEND::SetDepthTestEnabled(enabled);
}

void SetCullEnabled(bool enabled)
{
    RHI_BACKEND::SetCullEnabled(enabled);
}

void SetDepthWriteEnabled(bool enabled)
{
    RHI_BACKEND::SetDepthWriteEnabled(enabled);
}

void SetFogEnabled(bool enabled)
{
    RHI_BACKEND::SetFogEnabled(enabled);
}

void SetPolygonOffset(bool enabled, float factor, float units)
{
    RHI_BACKEND::SetPolygonOffset(enabled, factor, units);
}

void SetShaderProgram(ShaderProgram shader)
{
    RHI_BACKEND::SetShaderProgram(shader);
}

void SetPassthroughState(const PassthroughState& state)
{
    RHI_BACKEND::SetPassthroughState(state);
}

void BindVertexBuffer(BufferHandle handle, VertexLayout layout)
{
    RHI_BACKEND::BindVertexBuffer(handle, layout);
}

void BindIndexBuffer(BufferHandle handle)
{
    RHI_BACKEND::BindIndexBuffer(handle);
}

void Draw(Topology topology, uint32_t vertexCount, uint32_t firstVertex)
{
    RHI_BACKEND::Draw(topology, vertexCount, firstVertex);
}

void DrawIndexed(Topology topology, uint32_t indexCount, uint32_t firstIndex)
{
    RHI_BACKEND::DrawIndexed(topology, indexCount, firstIndex);
}

bool ReadDepthPixel(int x, int y, float* outDepth)
{
    return RHI_BACKEND::ReadDepthPixel(x, y, outDepth);
}

bool ReadColorFramebuffer(int x, int y, int w, int h, void* outRGB)
{
    return RHI_BACKEND::ReadColorFramebuffer(x, y, w, h, outRGB);
}

} // namespace RHI

#undef RHI_BACKEND

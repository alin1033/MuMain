#include "stdafx.h"
#include "Render/RHI/RHI.h"

#if defined(MU_RENDER_BACKEND_SDL_GPU)

#include "Render/RHI/RHIState.h"
#include "Render/SdlGpuProofShaders.h"
#include "Core/Utilities/FrameProfiler.h"
#include "Core/Utilities/Log/ErrorReport.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace RHI_SDL_GPU_Impl {

void Shutdown();

namespace {

using namespace RHI;

constexpr std::size_t kTextureSlotCount = 4;
constexpr std::size_t kUniformSlotCount = 8;

struct BufferRecord {
    SDL_GPUBuffer* gpu = nullptr;
    Uint32 capacity = 0;
    Uint32 appendOffset = 0;
    SDL_GPUBufferUsageFlags usage = 0;
    BufferUsage updateUsage = BufferUsage::Static;
};

struct TextureRecord {
    SDL_GPUTexture* gpu = nullptr;
    TextureDesc desc{};
    SDL_GPUSampler* sampler = nullptr;
};

struct UniformRecord {
    int bindingSlot = 0;
    std::vector<std::uint8_t> bytes;
};

struct PipelineKeyHasher {
    std::size_t operator()(const PipelineKey& key) const
    {
        return HashPipelineKey(key);
    }
};

struct DeviceState {
    SDL_Window* window = nullptr;
    SDL_GPUDevice* device = nullptr;
    SDL_GPUTexture* depthTarget = nullptr;
    SDL_GPUTexture* fallbackTexture = nullptr;
    SDL_GPUSampler* fallbackSampler = nullptr;
    std::array<SDL_GPUSampler*, 4> samplers{};
    std::array<SDL_GPUShader*, 3> vertexShaders{};
    SDL_GPUShader* fragmentShader = nullptr;

    SDL_GPUCommandBuffer* commandBuffer = nullptr;
    SDL_GPUTexture* swapchainTexture = nullptr;
    SDL_GPURenderPass* renderPass = nullptr;
    SDL_GPUCommandBuffer* uploadCommandBuffer = nullptr;
    SDL_GPUCopyPass* uploadCopyPass = nullptr;
    std::vector<SDL_GPUTransferBuffer*> pendingBufferTransfers;

    std::unordered_map<std::uint32_t, BufferRecord> buffers;
    std::unordered_map<std::uint32_t, TextureRecord> textures;
    std::unordered_map<std::uint32_t, UniformRecord> uniforms;
    std::unordered_map<PipelineKey, SDL_GPUGraphicsPipeline*, PipelineKeyHasher> pipelines;

    std::array<std::uint32_t, kUniformSlotCount> uniformSlots{};
    std::array<TextureHandle, kTextureSlotCount> boundTextures{};
    BufferHandle boundVertex{};
    BufferHandle boundIndex{};
    VertexLayout vertexLayout = VertexLayout::PosUvColor;
    ShaderProgram shader = ShaderProgram::Passthrough;
    BlendMode blendMode = BlendMode::Opaque;
    bool depthTestEnabled = true;
    bool cullEnabled = true;
    bool depthWriteEnabled = true;
    bool polygonOffsetEnabled = false;
    float polygonOffsetFactor = -1.0f;
    float polygonOffsetUnits = -1.0f;
    bool fogEnabled = true;
    PassthroughState passthroughState{};

    SDL_GPUViewport viewport{};
    SDL_Rect scissor{};
    bool scissorEnabled = false;
    bool clearColor = true;
    bool clearDepth = true;
    SDL_FColor clearValue{0.0f, 0.0f, 0.0f, 1.0f};
    bool colorHasContents = false;
    bool depthHasContents = false;
    int width = 1;
    int height = 1;
    std::uint32_t nextHandle = 1;
    bool initialized = false;
    bool windowClaimed = false;
};

DeviceState g;
Caps g_Caps{};

void LogInfo(const char* message)
{
    g_ErrorReport.Write(L"> SDL_GPU %hs.\r\n", message);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "MuMainGPU", "%s", message);
#endif
}

void LogFailure(const char* stage)
{
    const char* error = SDL_GetError();
    g_ErrorReport.Write(L"> SDL_GPU %hs failed: %hs.\r\n", stage,
        error ? error : "unknown SDL error");
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_ERROR, "MuMainGPU", "%s failed: %s",
        stage, error ? error : "unknown SDL error");
#endif
}

bool FitsUint32(std::size_t value, const char* stage)
{
    if (value <= std::numeric_limits<Uint32>::max()) return true;
    SDL_SetError("%s exceeds SDL_GPU's 32-bit resource size", stage);
    LogFailure(stage);
    return false;
}

std::uint32_t AllocateHandle()
{
    const std::uint32_t result = g.nextHandle++;
    if (g.nextHandle == 0) g.nextHandle = 1;
    return result;
}

SDL_GPUShader* CreateShader(const unsigned char* code, std::size_t codeSize,
    SDL_GPUShaderStage stage, Uint32 samplerCount, Uint32 uniformCount)
{
    SDL_GPUShaderCreateInfo info{};
    info.code = code;
    info.code_size = codeSize;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_samplers = samplerCount;
    info.num_uniform_buffers = uniformCount;
    return SDL_CreateGPUShader(g.device, &info);
}

SDL_GPUSampler* SamplerFor(TexFilter filter, TexWrap wrap)
{
    const std::size_t index = (filter == TexFilter::Linear ? 2U : 0U)
        + (wrap == TexWrap::Repeat ? 1U : 0U);
    return g.samplers[index];
}

bool CreateSamplers()
{
    for (int filter = 0; filter < 2; ++filter)
    {
        for (int wrap = 0; wrap < 2; ++wrap)
        {
            SDL_GPUSamplerCreateInfo info{};
            info.min_filter = filter ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
            info.mag_filter = filter ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
            info.mipmap_mode = filter ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR
                                      : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            info.address_mode_u = wrap ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT
                                       : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            info.address_mode_v = info.address_mode_u;
            info.address_mode_w = info.address_mode_u;
            g.samplers[static_cast<std::size_t>(filter * 2 + wrap)] =
                SDL_CreateGPUSampler(g.device, &info);
            if (!g.samplers[static_cast<std::size_t>(filter * 2 + wrap)])
            {
                LogFailure("sampler creation");
                return false;
            }
        }
    }
    g.fallbackSampler = g.samplers[0];
    return true;
}

bool SubmitBufferUpload(SDL_GPUBuffer* destination, Uint32 destinationOffset,
    const void* data, Uint32 size, bool cycle)
{
    if (!data || size == 0) return true;

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = size;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(g.device, &transferInfo);
    if (!transfer)
    {
        LogFailure("buffer transfer creation");
        return false;
    }

    void* mapped = SDL_MapGPUTransferBuffer(g.device, transfer, false);
    if (!mapped)
    {
        LogFailure("buffer transfer mapping");
        SDL_ReleaseGPUTransferBuffer(g.device, transfer);
        return false;
    }
    std::memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(g.device, transfer);

    // Dynamic geometry is appended while the frame's render command buffer is
    // being recorded. Keep all of those copies in one upload command buffer,
    // submit it once before the render buffer at EndFrame, and retain the
    // transfer resources until that submission. The old path submitted one
    // command buffer per UI batch (20-30 queue submissions per frame).
    if (g.commandBuffer)
    {
        if (!g.uploadCommandBuffer)
            g.uploadCommandBuffer = SDL_AcquireGPUCommandBuffer(g.device);
        if (g.uploadCommandBuffer && !g.uploadCopyPass)
            g.uploadCopyPass = SDL_BeginGPUCopyPass(g.uploadCommandBuffer);
        if (!g.uploadCopyPass)
        {
            LogFailure("batched buffer upload copy pass");
            if (g.uploadCommandBuffer)
            {
                SDL_CancelGPUCommandBuffer(g.uploadCommandBuffer);
                g.uploadCommandBuffer = nullptr;
            }
            SDL_ReleaseGPUTransferBuffer(g.device, transfer);
            return false;
        }

        const SDL_GPUTransferBufferLocation source{transfer, 0};
        const SDL_GPUBufferRegion target{destination, destinationOffset, size};
        SDL_UploadToGPUBuffer(g.uploadCopyPass, &source, &target, cycle);
        g.pendingBufferTransfers.push_back(transfer);
        return true;
    }

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(g.device);
    SDL_GPUCopyPass* copyPass = command ? SDL_BeginGPUCopyPass(command) : nullptr;
    if (!copyPass)
    {
        LogFailure("buffer upload copy pass");
        if (command) SDL_CancelGPUCommandBuffer(command);
        SDL_ReleaseGPUTransferBuffer(g.device, transfer);
        return false;
    }

    const SDL_GPUTransferBufferLocation source{transfer, 0};
    const SDL_GPUBufferRegion target{destination, destinationOffset, size};
    SDL_UploadToGPUBuffer(copyPass, &source, &target, cycle);
    SDL_EndGPUCopyPass(copyPass);
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(g.device, transfer);
    if (!submitted) LogFailure("buffer upload submission");
    return submitted;
}

bool SubmitTextureUpload(SDL_GPUTexture* destination, int x, int y, int w, int h,
    const void* pixels, bool cycle)
{
    if (!pixels || w <= 0 || h <= 0) return true;
    const std::size_t byteCount = static_cast<std::size_t>(w)
        * static_cast<std::size_t>(h) * 4U;
    if (!FitsUint32(byteCount, "texture upload")) return false;

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(byteCount);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(g.device, &transferInfo);
    if (!transfer)
    {
        LogFailure("texture transfer creation");
        return false;
    }
    void* mapped = SDL_MapGPUTransferBuffer(g.device, transfer, false);
    if (!mapped)
    {
        LogFailure("texture transfer mapping");
        SDL_ReleaseGPUTransferBuffer(g.device, transfer);
        return false;
    }
    std::memcpy(mapped, pixels, byteCount);
    SDL_UnmapGPUTransferBuffer(g.device, transfer);

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(g.device);
    SDL_GPUCopyPass* copyPass = command ? SDL_BeginGPUCopyPass(command) : nullptr;
    if (!copyPass)
    {
        LogFailure("texture upload copy pass");
        if (command) SDL_CancelGPUCommandBuffer(command);
        SDL_ReleaseGPUTransferBuffer(g.device, transfer);
        return false;
    }
    const SDL_GPUTextureTransferInfo source{transfer, 0,
        static_cast<Uint32>(w), static_cast<Uint32>(h)};
    const SDL_GPUTextureRegion target{destination, 0, 0,
        static_cast<Uint32>(x), static_cast<Uint32>(y), 0,
        static_cast<Uint32>(w), static_cast<Uint32>(h), 1};
    SDL_UploadToGPUTexture(copyPass, &source, &target, cycle);
    SDL_EndGPUCopyPass(copyPass);
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(g.device, transfer);
    if (!submitted) LogFailure("texture upload submission");
    return submitted;
}

bool CreateDepthTarget()
{
    if (g.depthTarget)
    {
        SDL_ReleaseGPUTexture(g.device, g.depthTarget);
        g.depthTarget = nullptr;
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width = static_cast<Uint32>(std::max(g.width, 1));
    info.height = static_cast<Uint32>(std::max(g.height, 1));
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    g.depthTarget = SDL_CreateGPUTexture(g.device, &info);
    if (!g.depthTarget) LogFailure("depth-target creation");
    return g.depthTarget != nullptr;
}

bool CreateFallbackTexture()
{
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = 1;
    info.height = 1;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    g.fallbackTexture = SDL_CreateGPUTexture(g.device, &info);
    const std::uint8_t white[4] = {255, 255, 255, 255};
    if (!g.fallbackTexture
        || !SubmitTextureUpload(g.fallbackTexture, 0, 0, 1, 1, white, false))
    {
        LogFailure("fallback texture creation");
        return false;
    }
    return true;
}

SDL_GPUVertexElementFormat ToSdlFormat(VertexElementFormat format)
{
    switch (format)
    {
    case VertexElementFormat::Float:  return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    case VertexElementFormat::Float2: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    case VertexElementFormat::Float3: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    case VertexElementFormat::Float4: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    case VertexElementFormat::Int:    return SDL_GPU_VERTEXELEMENTFORMAT_INT;
    }
    return SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
}

SDL_GPUBlendFactor ToSdlBlendFactor(BlendFactor factor)
{
    switch (factor)
    {
    case BlendFactor::Zero:             return SDL_GPU_BLENDFACTOR_ZERO;
    case BlendFactor::One:              return SDL_GPU_BLENDFACTOR_ONE;
    case BlendFactor::SrcColor:         return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SrcAlpha:         return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    }
    return SDL_GPU_BLENDFACTOR_ONE;
}

SDL_GPUPrimitiveType ToSdlTopology(Topology topology)
{
    switch (topology)
    {
    case Topology::TriangleList: return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    case Topology::LineList:     return SDL_GPU_PRIMITIVETYPE_LINELIST;
    case Topology::LineStrip:    return SDL_GPU_PRIMITIVETYPE_LINESTRIP;
    }
    return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
}

SDL_GPUShader* VertexShaderFor(VertexLayout layout)
{
    switch (layout)
    {
    case VertexLayout::Terrain: return g.vertexShaders[1];
    case VertexLayout::PosOnly: return g.vertexShaders[2];
    case VertexLayout::PosUvColor:
    case VertexLayout::BMDMesh: return g.vertexShaders[0];
    }
    return nullptr;
}

SDL_GPUGraphicsPipeline* CreatePipeline(const PipelineKey& key)
{
    const VertexLayoutDesc layout = DescribeVertexLayout(key.vertexLayout);
    if (layout.stride == 0 || layout.attributeCount == 0) return nullptr;

    SDL_GPUVertexBufferDescription bufferDescription{};
    bufferDescription.slot = 0;
    bufferDescription.pitch = layout.stride;
    bufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    std::array<SDL_GPUVertexAttribute, 5> attributes{};
    for (std::size_t i = 0; i < layout.attributeCount; ++i)
    {
        attributes[i].location = layout.attributes[i].location;
        attributes[i].buffer_slot = 0;
        attributes[i].format = ToSdlFormat(layout.attributes[i].format);
        attributes[i].offset = layout.attributes[i].offset;
    }

    const BlendStateDesc blend = DescribeBlendMode(key.blendMode);
    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = SDL_GetGPUSwapchainTextureFormat(g.device, g.window);
    colorTarget.blend_state.enable_blend = blend.enabled;
    colorTarget.blend_state.src_color_blendfactor = ToSdlBlendFactor(blend.source);
    colorTarget.blend_state.dst_color_blendfactor = ToSdlBlendFactor(blend.destination);
    colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    colorTarget.blend_state.src_alpha_blendfactor = ToSdlBlendFactor(blend.source);
    colorTarget.blend_state.dst_alpha_blendfactor = ToSdlBlendFactor(blend.destination);
    colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = VertexShaderFor(key.vertexLayout);
    info.fragment_shader = g.fragmentShader;
    info.vertex_input_state.vertex_buffer_descriptions = &bufferDescription;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes.data();
    info.vertex_input_state.num_vertex_attributes = layout.attributeCount;
    info.primitive_type = ToSdlTopology(key.topology);
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = key.cullEnabled
        ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip = true;
    info.rasterizer_state.enable_depth_bias = key.polygonOffsetEnabled;
    info.rasterizer_state.depth_bias_constant_factor = g.polygonOffsetUnits;
    info.rasterizer_state.depth_bias_slope_factor = g.polygonOffsetFactor;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.depth_stencil_state.enable_depth_test = key.depthTestEnabled;
    info.depth_stencil_state.enable_depth_write =
        key.depthTestEnabled && key.depthWriteEnabled;
    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1;
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    info.target_info.has_depth_stencil_target = true;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(g.device, &info);
    if (!pipeline) LogFailure("graphics-pipeline creation");
    return pipeline;
}

SDL_GPUGraphicsPipeline* ResolvePipeline(Topology topology)
{
    const PipelineKey key{
        g.shader,
        g.vertexLayout,
        topology,
        g.blendMode,
        g.depthTestEnabled,
        g.cullEnabled,
        g.depthWriteEnabled,
        g.polygonOffsetEnabled,
    };
    const auto found = g.pipelines.find(key);
    if (found != g.pipelines.end()) return found->second;

    SDL_GPUGraphicsPipeline* pipeline = CreatePipeline(key);
    if (pipeline) g.pipelines.emplace(key, pipeline);
    return pipeline;
}

void ApplyDynamicState()
{
    if (!g.renderPass) return;
    SDL_SetGPUViewport(g.renderPass, &g.viewport);
    const SDL_Rect full{0, 0, g.width, g.height};
    SDL_SetGPUScissor(g.renderPass, g.scissorEnabled ? &g.scissor : &full);
}

bool StartRenderPass()
{
    if (g.renderPass) return true;
    if (!g.commandBuffer || !g.swapchainTexture) return false;

    SDL_GPUColorTargetInfo color{};
    color.texture = g.swapchainTexture;
    color.clear_color = g.clearValue;
    color.load_op = (g.clearColor || !g.colorHasContents)
        ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = g.depthTarget;
    depth.clear_depth = 1.0f;
    depth.load_op = (g.clearDepth || !g.depthHasContents)
        ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

    g.renderPass = SDL_BeginGPURenderPass(g.commandBuffer, &color, 1, &depth);
    if (!g.renderPass)
    {
        LogFailure("render-pass creation");
        return false;
    }
    g.clearColor = false;
    g.clearDepth = false;
    g.colorHasContents = true;
    g.depthHasContents = true;
    ApplyDynamicState();
    return true;
}

void EndActiveRenderPass()
{
    if (!g.renderPass) return;
    SDL_EndGPURenderPass(g.renderPass);
    g.renderPass = nullptr;
}

void PushUniforms()
{
    // Logical slots preserve the existing ownership contract:
    //   0 GlobalMatrices -> vertex uniform 0
    //   1 SceneData      -> fragment uniform 0 (consumed by later shader ports)
    //   2 BoneMatrices   -> vertex uniform 1 (consumed by BMD/shadow ports)
    const std::uint32_t globalHandle = g.uniformSlots[0];
    const auto global = g.uniforms.find(globalHandle);
    if (global != g.uniforms.end() && !global->second.bytes.empty())
    {
        SDL_PushGPUVertexUniformData(g.commandBuffer, 0,
            global->second.bytes.data(), static_cast<Uint32>(global->second.bytes.size()));
    }
    else
    {
        // A valid std140 GlobalMatrices default: four identity matrices plus time.
        static const std::array<float, 68> identityGlobal = [] {
            std::array<float, 68> value{};
            for (int matrix = 0; matrix < 4; ++matrix)
                for (int axis = 0; axis < 4; ++axis)
                    value[static_cast<std::size_t>(matrix * 16 + axis * 4 + axis)] = 1.0f;
            return value;
        }();
        SDL_PushGPUVertexUniformData(g.commandBuffer, 0,
            identityGlobal.data(), static_cast<Uint32>(sizeof(identityGlobal)));
    }

    SDL_PushGPUFragmentUniformData(g.commandBuffer, 0, &g.passthroughState,
        static_cast<Uint32>(sizeof(g.passthroughState)));
}

bool BindDrawState(Topology topology)
{
    const auto vertex = g.buffers.find(g.boundVertex.id);
    if (vertex == g.buffers.end() || !StartRenderPass()) return false;

    SDL_GPUGraphicsPipeline* pipeline = ResolvePipeline(topology);
    if (!pipeline) return false;

    PushUniforms();
    SDL_BindGPUGraphicsPipeline(g.renderPass, pipeline);
    const SDL_GPUBufferBinding vertexBinding{vertex->second.gpu, 0};
    SDL_BindGPUVertexBuffers(g.renderPass, 0, &vertexBinding, 1);

    SDL_GPUTexture* texture = g.fallbackTexture;
    SDL_GPUSampler* sampler = g.fallbackSampler;
    const auto bound = g.textures.find(g.boundTextures[0].id);
    if (bound != g.textures.end())
    {
        texture = bound->second.gpu;
        sampler = bound->second.sampler;
    }
    const SDL_GPUTextureSamplerBinding textureBinding{texture, sampler};
    SDL_BindGPUFragmentSamplers(g.renderPass, 0, &textureBinding, 1);
    return true;
}

BufferHandle CreateBuffer(const void* initialData, std::size_t sizeBytes,
    BufferUsage updateUsage, SDL_GPUBufferUsageFlags usage)
{
    if (!g.device || sizeBytes == 0 || !FitsUint32(sizeBytes, "buffer creation")) return {};
    SDL_GPUBufferCreateInfo info{};
    info.usage = usage;
    info.size = static_cast<Uint32>(sizeBytes);
    SDL_GPUBuffer* gpu = SDL_CreateGPUBuffer(g.device, &info);
    if (!gpu)
    {
        LogFailure("buffer creation");
        return {};
    }
    if (initialData && !SubmitBufferUpload(gpu, 0, initialData, info.size, false))
    {
        SDL_ReleaseGPUBuffer(g.device, gpu);
        return {};
    }
    const BufferHandle handle{AllocateHandle()};
    g.buffers.emplace(handle.id, BufferRecord{gpu, info.size, 0, usage, updateUsage});
    return handle;
}

} // namespace

const Caps& GetCaps()
{
    return g_Caps;
}

bool Init(void* nativeWindowHandle, int width, int height)
{
    if (g.initialized) return true;
    g.window = static_cast<SDL_Window*>(nativeWindowHandle);
    g.width = std::max(width, 1);
    g.height = std::max(height, 1);
    if (!g.window)
    {
        SDL_SetError("SDL_GPU requires a valid SDL_Window");
        LogFailure("initialization");
        return false;
    }

    const char* debugValue = SDL_getenv("MU_GPU_DEBUG");
    const bool debugMode = debugValue && debugValue[0] == '1';
    g.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debugMode, "vulkan");
    if (!g.device)
    {
        LogFailure("device creation (requires Vulkan + SPIR-V)");
        return false;
    }
    if (!SDL_ClaimWindowForGPUDevice(g.device, g.window))
    {
        LogFailure("window claim");
        Shutdown();
        return false;
    }
    g.windowClaimed = true;

    const SDL_PropertiesID properties = SDL_GetGPUDeviceProperties(g.device);
    const char* driver = SDL_GetGPUDeviceDriver(g.device);
    const char* deviceName = SDL_GetStringProperty(properties,
        SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown");
    g_ErrorReport.Write(L"> SDL_GPU driver = %hs; device = %hs; debug = %d.\r\n",
        driver ? driver : "unknown", deviceName, debugMode ? 1 : 0);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "MuMainGPU",
        "backend=SDL_GPU driver=%s device=%s debug=%d",
        driver ? driver : "unknown", deviceName, debugMode ? 1 : 0);
#endif

    g.vertexShaders[0] = CreateShader(RHI_SDL_GPU_Proof_Shaders::Vertex,
        sizeof(RHI_SDL_GPU_Proof_Shaders::Vertex), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    g.vertexShaders[1] = CreateShader(RHI_SDL_GPU_Proof_Shaders::TerrainVertex,
        sizeof(RHI_SDL_GPU_Proof_Shaders::TerrainVertex), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    g.vertexShaders[2] = CreateShader(RHI_SDL_GPU_Proof_Shaders::PosOnlyVertex,
        sizeof(RHI_SDL_GPU_Proof_Shaders::PosOnlyVertex), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    g.fragmentShader = CreateShader(RHI_SDL_GPU_Proof_Shaders::Fragment,
        sizeof(RHI_SDL_GPU_Proof_Shaders::Fragment), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!g.vertexShaders[0] || !g.vertexShaders[1] || !g.vertexShaders[2]
        || !g.fragmentShader || !CreateSamplers() || !CreateDepthTarget()
        || !CreateFallbackTexture())
    {
        LogFailure("core resource creation");
        Shutdown();
        return false;
    }

    g.buffers.reserve(128);
    g.textures.reserve(512);
    g.uniforms.reserve(16);
    g.pipelines.reserve(128);
    g.viewport = {0.0f, 0.0f, static_cast<float>(g.width),
        static_cast<float>(g.height), 0.0f, 1.0f};
    g.scissor = {0, 0, g.width, g.height};
    g.initialized = true;
    LogInfo("resource/frame contract initialized");
    return true;
}

void Shutdown()
{
    if (!g.device) return;
    EndActiveRenderPass();
    if (g.uploadCopyPass)
    {
        SDL_EndGPUCopyPass(g.uploadCopyPass);
        g.uploadCopyPass = nullptr;
    }
    if (g.uploadCommandBuffer)
    {
        SDL_CancelGPUCommandBuffer(g.uploadCommandBuffer);
        g.uploadCommandBuffer = nullptr;
    }
    for (SDL_GPUTransferBuffer* transfer : g.pendingBufferTransfers)
        SDL_ReleaseGPUTransferBuffer(g.device, transfer);
    g.pendingBufferTransfers.clear();
    if (g.commandBuffer)
    {
        SDL_CancelGPUCommandBuffer(g.commandBuffer);
        g.commandBuffer = nullptr;
    }
    SDL_WaitForGPUIdle(g.device);

    for (auto& [key, pipeline] : g.pipelines)
        if (pipeline) SDL_ReleaseGPUGraphicsPipeline(g.device, pipeline);
    for (auto& [id, buffer] : g.buffers)
        if (buffer.gpu) SDL_ReleaseGPUBuffer(g.device, buffer.gpu);
    for (auto& [id, texture] : g.textures)
        if (texture.gpu) SDL_ReleaseGPUTexture(g.device, texture.gpu);
    if (g.fallbackTexture) SDL_ReleaseGPUTexture(g.device, g.fallbackTexture);
    if (g.depthTarget) SDL_ReleaseGPUTexture(g.device, g.depthTarget);
    for (SDL_GPUShader* shader : g.vertexShaders)
        if (shader) SDL_ReleaseGPUShader(g.device, shader);
    if (g.fragmentShader) SDL_ReleaseGPUShader(g.device, g.fragmentShader);
    for (SDL_GPUSampler* sampler : g.samplers)
        if (sampler) SDL_ReleaseGPUSampler(g.device, sampler);
    if (g.windowClaimed) SDL_ReleaseWindowFromGPUDevice(g.device, g.window);
    SDL_DestroyGPUDevice(g.device);
    g = DeviceState{};
}

void BeginFrame()
{
    if (!g.initialized || g.commandBuffer) return;
    g.commandBuffer = SDL_AcquireGPUCommandBuffer(g.device);
    if (!g.commandBuffer)
    {
        LogFailure("command-buffer acquisition");
        return;
    }
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(g.commandBuffer, g.window,
            &g.swapchainTexture, nullptr, nullptr))
    {
        LogFailure("swapchain acquisition");
        SDL_CancelGPUCommandBuffer(g.commandBuffer);
        g.commandBuffer = nullptr;
        return;
    }
    g.colorHasContents = false;
    g.depthHasContents = false;
    g.clearColor = true;
    g.clearDepth = true;
}

void EndFrame()
{
    if (!g.commandBuffer) return;
    if (g.swapchainTexture && !g.renderPass) StartRenderPass();
    EndActiveRenderPass();

    if (g.uploadCopyPass)
    {
        SDL_EndGPUCopyPass(g.uploadCopyPass);
        g.uploadCopyPass = nullptr;
    }
    if (g.uploadCommandBuffer)
    {
        if (!SDL_SubmitGPUCommandBuffer(g.uploadCommandBuffer))
            LogFailure("batched buffer upload submission");
        g.uploadCommandBuffer = nullptr;
    }
    for (SDL_GPUTransferBuffer* transfer : g.pendingBufferTransfers)
        SDL_ReleaseGPUTransferBuffer(g.device, transfer);
    g.pendingBufferTransfers.clear();

    if (!SDL_SubmitGPUCommandBuffer(g.commandBuffer))
        LogFailure("command-buffer submission");
    g.commandBuffer = nullptr;
    g.swapchainTexture = nullptr;
}

void SetViewport(int x, int y, int w, int h)
{
    g.viewport = {static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    ApplyDynamicState();
}

void SetScissorEnabled(bool enabled)
{
    g.scissorEnabled = enabled;
    ApplyDynamicState();
}

void SetScissorRect(int x, int y, int w, int h)
{
    g.scissor = {x, y, w, h};
    ApplyDynamicState();
}

void OnResize(int width, int height)
{
    g.width = std::max(width, 1);
    g.height = std::max(height, 1);
    g.viewport.w = static_cast<float>(g.width);
    g.viewport.h = static_cast<float>(g.height);
    g.scissor = {0, 0, g.width, g.height};
    if (!g.commandBuffer) CreateDepthTarget();
}

void Clear(bool color, bool depth, float r, float green, float b, float a)
{
    if (g.renderPass) EndActiveRenderPass();
    if (color)
    {
        g.clearColor = true;
        g.clearValue = {r, green, b, a};
    }
    if (depth) g.clearDepth = true;
}

BufferHandle CreateVertexBuffer(const void* data, size_t size, BufferUsage usage)
{
    return CreateBuffer(data, size, usage, SDL_GPU_BUFFERUSAGE_VERTEX);
}

BufferHandle CreateIndexBuffer(const void* data, size_t size, BufferUsage usage)
{
    return CreateBuffer(data, size, usage, SDL_GPU_BUFFERUSAGE_INDEX);
}

void UpdateBuffer(BufferHandle handle, const void* data, size_t size)
{
    auto found = g.buffers.find(handle.id);
    if (found == g.buffers.end() || !data || !FitsUint32(size, "buffer update")) return;
    BufferRecord& record = found->second;
    if (size > record.capacity)
    {
        SDL_GPUBufferCreateInfo info{record.usage, static_cast<Uint32>(size), 0};
        SDL_GPUBuffer* replacement = SDL_CreateGPUBuffer(g.device, &info);
        if (!replacement)
        {
            LogFailure("buffer growth");
            return;
        }
        SDL_ReleaseGPUBuffer(g.device, record.gpu);
        record.gpu = replacement;
        record.capacity = info.size;
    }
    record.appendOffset = 0;
    SubmitBufferUpload(record.gpu, 0, data, static_cast<Uint32>(size), true);
}

size_t AppendBuffer(BufferHandle handle, const void* data, size_t size)
{
    auto found = g.buffers.find(handle.id);
    if (found == g.buffers.end() || !data || !FitsUint32(size, "buffer append")) return 0;
    BufferRecord& record = found->second;
    const Uint32 requested = static_cast<Uint32>(size);
    if (requested > record.capacity)
    {
        Uint32 capacity = std::max(record.capacity, 1U);
        while (capacity < requested && capacity <= std::numeric_limits<Uint32>::max() / 2U)
            capacity *= 2U;
        capacity = std::max(capacity, requested);
        SDL_GPUBufferCreateInfo info{record.usage, capacity, 0};
        SDL_GPUBuffer* replacement = SDL_CreateGPUBuffer(g.device, &info);
        if (!replacement)
        {
            LogFailure("append-buffer growth");
            return 0;
        }
        SDL_ReleaseGPUBuffer(g.device, record.gpu);
        record.gpu = replacement;
        record.capacity = capacity;
        record.appendOffset = 0;
    }
    if (record.appendOffset > record.capacity - requested) record.appendOffset = 0;
    const Uint32 offset = record.appendOffset;
    if (!SubmitBufferUpload(record.gpu, offset, data, requested, true)) return 0;
    record.appendOffset += requested;
    return offset;
}

void DestroyBuffer(BufferHandle handle)
{
    const auto found = g.buffers.find(handle.id);
    if (found == g.buffers.end()) return;
    SDL_ReleaseGPUBuffer(g.device, found->second.gpu);
    g.buffers.erase(found);
    if (g.boundVertex == handle) g.boundVertex = {};
    if (g.boundIndex == handle) g.boundIndex = {};
}

BufferHandle CreateUniformBlock(size_t size, int bindingSlot)
{
    if (size == 0 || bindingSlot < 0
        || bindingSlot >= static_cast<int>(kUniformSlotCount)) return {};
    const BufferHandle handle{AllocateHandle()};
    UniformRecord record;
    record.bindingSlot = bindingSlot;
    record.bytes.resize(size);
    g.uniforms.emplace(handle.id, std::move(record));
    g.uniformSlots[static_cast<std::size_t>(bindingSlot)] = handle.id;
    return handle;
}

void UpdateUniformBlock(BufferHandle handle, const void* data, size_t size)
{
    auto found = g.uniforms.find(handle.id);
    if (found == g.uniforms.end() || !data || !FitsUint32(size, "uniform update")) return;
    found->second.bytes.resize(size);
    std::memcpy(found->second.bytes.data(), data, size);
}

void DestroyUniformBlock(BufferHandle handle)
{
    const auto found = g.uniforms.find(handle.id);
    if (found == g.uniforms.end()) return;
    const int slot = found->second.bindingSlot;
    if (slot >= 0 && slot < static_cast<int>(kUniformSlotCount)
        && g.uniformSlots[static_cast<std::size_t>(slot)] == handle.id)
        g.uniformSlots[static_cast<std::size_t>(slot)] = 0;
    g.uniforms.erase(found);
}

TextureHandle CreateTexture(const TextureDesc& desc, const void* pixels)
{
    if (!g.device || desc.width <= 0 || desc.height <= 0) return {};
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(desc.width);
    info.height = static_cast<Uint32>(desc.height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* gpu = SDL_CreateGPUTexture(g.device, &info);
    if (!gpu)
    {
        LogFailure("texture creation");
        return {};
    }
    if (pixels && !SubmitTextureUpload(gpu, 0, 0, desc.width, desc.height, pixels, false))
    {
        SDL_ReleaseGPUTexture(g.device, gpu);
        return {};
    }
    const TextureHandle handle{AllocateHandle()};
    g.textures.emplace(handle.id, TextureRecord{gpu, desc,
        SamplerFor(desc.filter, desc.wrap)});
    return handle;
}

void UpdateTexture(TextureHandle handle, int x, int y, int w, int h, const void* pixels)
{
    const auto found = g.textures.find(handle.id);
    if (found == g.textures.end()) return;
    SubmitTextureUpload(found->second.gpu, x, y, w, h, pixels, true);
}

void DestroyTexture(TextureHandle handle)
{
    const auto found = g.textures.find(handle.id);
    if (found == g.textures.end()) return;
    SDL_ReleaseGPUTexture(g.device, found->second.gpu);
    g.textures.erase(found);
    for (TextureHandle& bound : g.boundTextures)
        if (bound == handle) bound = {};
}

void BindTexture(TextureHandle handle, int slot)
{
    if (slot < 0 || slot >= static_cast<int>(kTextureSlotCount)) return;
    g.boundTextures[static_cast<std::size_t>(slot)] = handle;
}

void SetBlendMode(BlendMode mode)
{
    g.blendMode = mode;
    const BlendStateDesc state = DescribeBlendMode(mode);
    g.cullEnabled = state.cullEnabled;
    g.depthWriteEnabled = state.depthWriteEnabled;
    g.fogEnabled = state.fogEnabled;
}

void SetDepthTestEnabled(bool enabled) { g.depthTestEnabled = enabled; }
void SetCullEnabled(bool enabled) { g.cullEnabled = enabled; }
void SetDepthWriteEnabled(bool enabled) { g.depthWriteEnabled = enabled; }
void SetFogEnabled(bool enabled) { g.fogEnabled = enabled; }

void SetPolygonOffset(bool enabled, float factor, float units)
{
    g.polygonOffsetEnabled = enabled;
    g.polygonOffsetFactor = factor;
    g.polygonOffsetUnits = units;
}

void SetShaderProgram(ShaderProgram shader) { g.shader = shader; }
void SetPassthroughState(const PassthroughState& state) { g.passthroughState = state; }

void BindVertexBuffer(BufferHandle handle, VertexLayout layout)
{
    g.boundVertex = handle;
    g.vertexLayout = layout;
}

void BindIndexBuffer(BufferHandle handle) { g.boundIndex = handle; }

void Draw(Topology topology, uint32_t vertexCount, uint32_t firstVertex)
{
    if (vertexCount == 0 || !BindDrawState(topology)) return;
    SDL_DrawGPUPrimitives(g.renderPass, vertexCount, 1, firstVertex, 0);
    FrameProfiler::CountSkip(FrameProfiler::Counter::DrawCalls);
}

void DrawIndexed(Topology topology, uint32_t indexCount, uint32_t firstIndex)
{
    const auto index = g.buffers.find(g.boundIndex.id);
    if (indexCount == 0 || index == g.buffers.end() || !BindDrawState(topology)) return;
    const SDL_GPUBufferBinding binding{index->second.gpu, 0};
    SDL_BindGPUIndexBuffer(g.renderPass, &binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(g.renderPass, indexCount, 1, firstIndex, 0, 0);
    FrameProfiler::CountSkip(FrameProfiler::Counter::DrawCalls);
}

bool ReadDepthPixel(int, int, float*)
{
    static bool logged = false;
    if (!logged)
    {
        LogInfo("depth readback is deferred to the scene-port stage");
        logged = true;
    }
    return false;
}

bool ReadColorFramebuffer(int, int, int, int, void*)
{
    static bool logged = false;
    if (!logged)
    {
        LogInfo("color readback is deferred to the scene-port stage");
        logged = true;
    }
    return false;
}

} // namespace RHI_SDL_GPU_Impl

#endif

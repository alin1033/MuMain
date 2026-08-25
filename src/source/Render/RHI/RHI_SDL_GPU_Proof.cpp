#include "stdafx.h"
#include "Render/RHI/RHI.h"

#if defined(MU_RENDER_BACKEND_SDL_GPU)

#include "Render/SdlGpuProofShaders.h"
#include "Core/Utilities/Log/ErrorReport.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace {

struct ProofVertex {
    float position[3];
    float uv[2];
    std::uint8_t color[4];
};

constexpr std::array<ProofVertex, 6> kVertices = {{
    {{-0.75f, -0.75f, 0.0f}, {0.0f, 1.0f}, {255, 255, 255, 255}},
    {{ 0.75f, -0.75f, 0.0f}, {1.0f, 1.0f}, {255, 255, 255, 255}},
    {{ 0.75f,  0.75f, 0.0f}, {1.0f, 0.0f}, {255, 255, 255, 255}},
    {{-0.75f, -0.75f, 0.0f}, {0.0f, 1.0f}, {255, 255, 255, 255}},
    {{ 0.75f,  0.75f, 0.0f}, {1.0f, 0.0f}, {255, 255, 255, 255}},
    {{-0.75f,  0.75f, 0.0f}, {0.0f, 0.0f}, {255, 255, 255, 255}},
}};

constexpr std::array<std::uint8_t, 16> kTexturePixels = {
    255,  48,  48, 255,    48, 255,  96, 255,
     48,  96, 255, 255,   255, 224,  48, 255,
};

void LogGpuInfo(const char* driver, const char* device)
{
    g_ErrorReport.Write(L"> SDL_GPU driver = %hs; device = %hs.\r\n", driver, device);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "MuMainGPU",
        "backend=SDL_GPU driver=%s device=%s", driver, device);
#endif
}

void LogGpuFailure(const char* stage)
{
    const char* error = SDL_GetError();
    g_ErrorReport.Write(L"> SDL_GPU %hs failed: %hs.\r\n", stage, error);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_ERROR, "MuMainGPU",
        "%s failed: %s", stage, error);
#endif
}

void LogProofReady()
{
    g_ErrorReport.Write(L"> SDL_GPU textured PosUvColor proof pipeline ready.\r\n");
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "MuMainGPU",
        "textured PosUvColor proof pipeline ready");
#endif
}

class ProofRenderer {
public:
    bool Initialize(SDL_Window* window)
    {
        window_ = window;
        device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
        if (!device_)
        {
            LogGpuFailure("device creation (requires Vulkan + SPIR-V)");
            return false;
        }
        if (!SDL_ClaimWindowForGPUDevice(device_, window_))
        {
            LogGpuFailure("window claim");
            return false;
        }
        windowClaimed_ = true;

        const SDL_PropertiesID properties = SDL_GetGPUDeviceProperties(device_);
        const char* driver = SDL_GetGPUDeviceDriver(device_);
        const char* deviceName = SDL_GetStringProperty(properties,
            SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown");
        LogGpuInfo(driver ? driver : "unknown", deviceName);

        return CreateShaders()
            && CreateResources()
            && UploadResources()
            && CreatePipeline();
    }

    bool RenderFrame()
    {
        SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device_);
        if (!commandBuffer)
        {
            LogGpuFailure("command-buffer acquisition");
            return false;
        }

        SDL_GPUTexture* swapchainTexture = nullptr;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window_,
                &swapchainTexture, nullptr, nullptr))
        {
            LogGpuFailure("swapchain acquisition");
            SDL_CancelGPUCommandBuffer(commandBuffer);
            return false;
        }
        if (!swapchainTexture)
        {
            return SDL_SubmitGPUCommandBuffer(commandBuffer);
        }

        SDL_GPUColorTargetInfo colorTarget{};
        colorTarget.texture = swapchainTexture;
        colorTarget.clear_color = {0.025f, 0.04f, 0.09f, 1.0f};
        colorTarget.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTarget.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(
            commandBuffer, &colorTarget, 1, nullptr);
        if (!renderPass)
        {
            LogGpuFailure("render-pass creation");
            SDL_SubmitGPUCommandBuffer(commandBuffer);
            return false;
        }

        const SDL_GPUBufferBinding vertexBinding{vertexBuffer_, 0};
        const SDL_GPUTextureSamplerBinding textureBinding{texture_, sampler_};
        SDL_BindGPUGraphicsPipeline(renderPass, pipeline_);
        SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
        SDL_BindGPUFragmentSamplers(renderPass, 0, &textureBinding, 1);
        SDL_DrawGPUPrimitives(renderPass, static_cast<Uint32>(kVertices.size()), 1, 0, 0);
        SDL_EndGPURenderPass(renderPass);

        if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
        {
            LogGpuFailure("command-buffer submission");
            return false;
        }
        return true;
    }

    ~ProofRenderer()
    {
        if (!device_)
        {
            return;
        }

        SDL_WaitForGPUIdle(device_);
        if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
        if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
        if (texture_) SDL_ReleaseGPUTexture(device_, texture_);
        if (vertexBuffer_) SDL_ReleaseGPUBuffer(device_, vertexBuffer_);
        if (fragmentShader_) SDL_ReleaseGPUShader(device_, fragmentShader_);
        if (vertexShader_) SDL_ReleaseGPUShader(device_, vertexShader_);
        if (windowClaimed_) SDL_ReleaseWindowFromGPUDevice(device_, window_);
        SDL_DestroyGPUDevice(device_);
    }

private:
    SDL_GPUShader* CreateShader(const unsigned char* code, std::size_t codeSize,
        SDL_GPUShaderStage stage, Uint32 samplerCount)
    {
        SDL_GPUShaderCreateInfo info{};
        info.code = code;
        info.code_size = codeSize;
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.stage = stage;
        info.num_samplers = samplerCount;
        return SDL_CreateGPUShader(device_, &info);
    }

    bool CreateShaders()
    {
        vertexShader_ = CreateShader(RHI_SDL_GPU_Proof_Shaders::Vertex,
            sizeof(RHI_SDL_GPU_Proof_Shaders::Vertex), SDL_GPU_SHADERSTAGE_VERTEX, 0);
        if (!vertexShader_)
        {
            LogGpuFailure("vertex-shader creation");
            return false;
        }

        fragmentShader_ = CreateShader(RHI_SDL_GPU_Proof_Shaders::Fragment,
            sizeof(RHI_SDL_GPU_Proof_Shaders::Fragment), SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
        if (!fragmentShader_)
        {
            LogGpuFailure("fragment-shader creation");
            return false;
        }
        return true;
    }

    bool CreateResources()
    {
        SDL_GPUBufferCreateInfo bufferInfo{};
        bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bufferInfo.size = sizeof(kVertices);
        vertexBuffer_ = SDL_CreateGPUBuffer(device_, &bufferInfo);

        SDL_GPUTextureCreateInfo textureInfo{};
        textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
        textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        textureInfo.width = 2;
        textureInfo.height = 2;
        textureInfo.layer_count_or_depth = 1;
        textureInfo.num_levels = 1;
        textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        texture_ = SDL_CreateGPUTexture(device_, &textureInfo);

        SDL_GPUSamplerCreateInfo samplerInfo{};
        samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
        samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
        samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_ = SDL_CreateGPUSampler(device_, &samplerInfo);

        if (!vertexBuffer_ || !texture_ || !sampler_)
        {
            LogGpuFailure("proof resource creation");
            return false;
        }
        return true;
    }

    bool UploadResources()
    {
        constexpr Uint32 vertexBytes = static_cast<Uint32>(sizeof(kVertices));
        constexpr Uint32 textureBytes = static_cast<Uint32>(sizeof(kTexturePixels));

        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = vertexBytes + textureBytes;
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
        if (!transfer)
        {
            LogGpuFailure("transfer-buffer creation");
            return false;
        }

        void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
        if (!mapped)
        {
            LogGpuFailure("transfer-buffer mapping");
            SDL_ReleaseGPUTransferBuffer(device_, transfer);
            return false;
        }
        std::memcpy(mapped, kVertices.data(), vertexBytes);
        std::memcpy(static_cast<std::uint8_t*>(mapped) + vertexBytes,
            kTexturePixels.data(), textureBytes);
        SDL_UnmapGPUTransferBuffer(device_, transfer);

        SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass* copyPass = commandBuffer
            ? SDL_BeginGPUCopyPass(commandBuffer)
            : nullptr;
        if (!copyPass)
        {
            LogGpuFailure("upload copy-pass creation");
            if (commandBuffer) SDL_CancelGPUCommandBuffer(commandBuffer);
            SDL_ReleaseGPUTransferBuffer(device_, transfer);
            return false;
        }

        const SDL_GPUTransferBufferLocation bufferSource{transfer, 0};
        const SDL_GPUBufferRegion bufferDestination{vertexBuffer_, 0, vertexBytes};
        SDL_UploadToGPUBuffer(copyPass, &bufferSource, &bufferDestination, false);

        const SDL_GPUTextureTransferInfo textureSource{transfer, vertexBytes, 2, 2};
        const SDL_GPUTextureRegion textureDestination{
            texture_, 0, 0, 0, 0, 0, 2, 2, 1};
        SDL_UploadToGPUTexture(copyPass, &textureSource, &textureDestination, false);
        SDL_EndGPUCopyPass(copyPass);

        const bool submitted = SDL_SubmitGPUCommandBuffer(commandBuffer);
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        if (!submitted)
        {
            LogGpuFailure("resource upload submission");
        }
        return submitted;
    }

    bool CreatePipeline()
    {
        const SDL_GPUVertexBufferDescription bufferDescription{
            0, sizeof(ProofVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        const std::array<SDL_GPUVertexAttribute, 3> attributes = {{
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(ProofVertex, position)},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(ProofVertex, uv)},
            {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(ProofVertex, color)},
        }};

        const SDL_GPUColorTargetDescription colorTarget{
            SDL_GetGPUSwapchainTextureFormat(device_, window_), {}};
        SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.vertex_shader = vertexShader_;
        pipelineInfo.fragment_shader = fragmentShader_;
        pipelineInfo.vertex_input_state = {
            &bufferDescription, 1, attributes.data(), static_cast<Uint32>(attributes.size())};
        pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipelineInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipelineInfo.rasterizer_state.enable_depth_clip = true;
        pipelineInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pipelineInfo.target_info.color_target_descriptions = &colorTarget;
        pipelineInfo.target_info.num_color_targets = 1;
        pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipelineInfo);
        if (!pipeline_)
        {
            LogGpuFailure("graphics-pipeline creation");
            return false;
        }
        LogProofReady();
        return true;
    }

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUShader* vertexShader_ = nullptr;
    SDL_GPUShader* fragmentShader_ = nullptr;
    SDL_GPUBuffer* vertexBuffer_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    bool windowClaimed_ = false;
};

} // namespace

namespace RHI {

bool RunSdlGpuProof(void* nativeWindowHandle)
{
    auto* window = static_cast<SDL_Window*>(nativeWindowHandle);
    ProofRenderer renderer;
    if (!window || !renderer.Initialize(window))
    {
        return false;
    }

    bool running = true;
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT
                || (event.type == SDL_EVENT_KEY_DOWN
                    && (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_AC_BACK)))
            {
                running = false;
            }
        }

        if (running && !renderer.RenderFrame())
        {
            return false;
        }
    }
    return true;
}

} // namespace RHI

#else

namespace RHI {

bool RunSdlGpuProof(void*)
{
    return false;
}

} // namespace RHI

#endif

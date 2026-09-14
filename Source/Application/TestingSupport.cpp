// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace Ggui
{

#ifdef IMGUI_BUILD_TESTING
bool Application::UsesSdrSwapchainForTest() const
{
    if (_gpu_device == nullptr || _window == nullptr)
        return false;
    const SDL_GPUTextureFormat format =
        SDL_GetGPUSwapchainTextureFormat(_gpu_device, _window);
    return format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ||
           format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
}

bool Application::CaptureFramebufferForTest(
    int x, int y, int width, int height, unsigned int* pixels)
{
    if (_gpu_device == nullptr || _capture_texture == nullptr || pixels == nullptr ||
        x < 0 || y < 0 || width <= 0 || height <= 0 ||
        static_cast<unsigned int>(x) > _capture_width ||
        static_cast<unsigned int>(y) > _capture_height ||
        static_cast<unsigned int>(width) > _capture_width - static_cast<unsigned int>(x) ||
        static_cast<unsigned int>(height) > _capture_height - static_cast<unsigned int>(y))
        return false;
    const std::size_t byte_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    if (byte_count > std::numeric_limits<Uint32>::max())
        return false;

    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transfer_info.size = static_cast<Uint32>(byte_count);
    SDL_GPUTransferBuffer* transfer_buffer =
        SDL_CreateGPUTransferBuffer(_gpu_device, &transfer_info);
    SDL_GPUCommandBuffer* command_buffer =
        SDL_AcquireGPUCommandBuffer(_gpu_device);
    if (transfer_buffer == nullptr || command_buffer == nullptr)
    {
        if (command_buffer != nullptr)
            SDL_CancelGPUCommandBuffer(command_buffer);
        if (transfer_buffer != nullptr)
            SDL_ReleaseGPUTransferBuffer(_gpu_device, transfer_buffer);
        return false;
    }

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    if (copy_pass == nullptr)
    {
        SDL_CancelGPUCommandBuffer(command_buffer);
        SDL_ReleaseGPUTransferBuffer(_gpu_device, transfer_buffer);
        return false;
    }
    SDL_GPUTextureRegion source{};
    source.texture = _capture_texture;
    source.x = static_cast<Uint32>(x);
    source.y = static_cast<Uint32>(y);
    source.w = static_cast<Uint32>(width);
    source.h = static_cast<Uint32>(height);
    source.d = 1;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = transfer_buffer;
    destination.pixels_per_row = static_cast<Uint32>(width);
    destination.rows_per_layer = static_cast<Uint32>(height);
    SDL_DownloadFromGPUTexture(copy_pass, &source, &destination);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_GPUFence* fence =
        SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
    const bool completed = fence != nullptr &&
        SDL_WaitForGPUFences(_gpu_device, true, &fence, 1);
    if (fence != nullptr)
        SDL_ReleaseGPUFence(_gpu_device, fence);

    void* mapped = completed
        ? SDL_MapGPUTransferBuffer(_gpu_device, transfer_buffer, false)
        : nullptr;
    if (mapped != nullptr)
    {
        std::memcpy(pixels, mapped, byte_count);
        if (SDL_GetGPUSwapchainTextureFormat(_gpu_device, _window) ==
            SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)
        {
            auto* bytes = reinterpret_cast<unsigned char*>(pixels);
            for (std::size_t offset = 0; offset < byte_count; offset += 4)
                std::swap(bytes[offset], bytes[offset + 2]);
        }
        SDL_UnmapGPUTransferBuffer(_gpu_device, transfer_buffer);
    }
    SDL_ReleaseGPUTransferBuffer(_gpu_device, transfer_buffer);
    return mapped != nullptr;
}

namespace ApplicationInternal
{

Application* test_application = nullptr;

bool CaptureFramebuffer(
    ImGuiID viewport_id, int x, int y, int width, int height, unsigned int* pixels, void* user_data)
{
    (void)viewport_id;
    (void)user_data;
    return test_application != nullptr &&
           test_application->CaptureFramebufferForTest(
               x, y, width, height, pixels);
}

} // namespace ApplicationInternal
#endif

} // namespace Ggui

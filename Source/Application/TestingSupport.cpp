// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <SDL3/SDL_opengl.h>

#include <algorithm>

namespace Ggui::ApplicationInternal
{

#ifdef IMGUI_BUILD_TESTING
Application* test_application = nullptr;

bool CaptureFramebuffer(
    ImGuiID viewport_id, int x, int y, int width, int height, unsigned int* pixels, void* user_data)
{
    (void)viewport_id;
    (void)user_data;
    const int framebuffer_height = static_cast<int>(ImGui::GetIO().DisplaySize.y);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, framebuffer_height - (y + height), width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (int row = 0; row < height / 2; ++row)
    {
        unsigned int* top = pixels + row * width;
        unsigned int* bottom = pixels + (height - row - 1) * width;
        std::swap_ranges(top, top + width, bottom);
    }
    return true;
}
#endif

} // namespace Ggui::ApplicationInternal

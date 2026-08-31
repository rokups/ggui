// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

namespace Ggui
{
using namespace ApplicationInternal;

void Application::ApplyTheme()
{
    const float display_scale = _window == nullptr ? 1.0f : SDL_GetWindowDisplayScale(_window);
    const float dpi_scale = display_scale > 0.0f ? display_scale : 1.0f;
    ImGui::GetStyle() = ImGuiStyle{};
    ImGui::GetStyle().FontScaleDpi = dpi_scale;
    if (_dark_theme)
    {
        ImGui::StyleColorsDark();
        ImVec4* colors = ImGui::GetStyle().Colors;
        colors[ImGuiCol_Text] = ImVec4(0.902f, 0.925f, 0.953f, 1.0f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.490f, 0.521f, 0.564f, 1.0f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.047f, 0.067f, 0.094f, 1.0f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.082f, 0.106f, 0.145f, 1.0f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.082f, 0.106f, 0.145f, 0.98f);
        colors[ImGuiCol_Border] = ImVec4(0.204f, 0.251f, 0.314f, 1.0f);
        colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.098f, 0.129f, 0.176f, 1.0f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.125f, 0.161f, 0.220f, 1.0f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.145f, 0.188f, 0.255f, 1.0f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.059f, 0.078f, 0.110f, 1.0f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.082f, 0.106f, 0.145f, 1.0f);
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.059f, 0.078f, 0.110f, 1.0f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.059f, 0.078f, 0.110f, 1.0f);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.059f, 0.078f, 0.110f, 1.0f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.176f, 0.216f, 0.278f, 1.0f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.235f, 0.286f, 0.365f, 1.0f);
        colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.278f, 0.341f, 0.435f, 1.0f);
        colors[ImGuiCol_CheckMark] = ImVec4(0.180f, 0.706f, 0.333f, 1.0f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.133f, 0.478f, 0.863f, 1.0f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.180f, 0.561f, 0.973f, 1.0f);
        colors[ImGuiCol_Button] = ImVec4(0.114f, 0.424f, 0.765f, 0.92f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.153f, 0.510f, 0.910f, 1.0f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.110f, 0.357f, 0.639f, 1.0f);
        colors[ImGuiCol_Header] = ImVec4(0.114f, 0.278f, 0.459f, 0.95f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.153f, 0.349f, 0.565f, 1.0f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.173f, 0.392f, 0.624f, 1.0f);
        colors[ImGuiCol_Separator] = ImVec4(0.204f, 0.251f, 0.314f, 1.0f);
        colors[ImGuiCol_SeparatorHovered] = ImVec4(0.267f, 0.337f, 0.427f, 1.0f);
        colors[ImGuiCol_SeparatorActive] = ImVec4(0.114f, 0.424f, 0.765f, 1.0f);
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.114f, 0.424f, 0.765f, 0.25f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.153f, 0.510f, 0.910f, 0.67f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(0.153f, 0.510f, 0.910f, 0.95f);
        colors[ImGuiCol_Tab] = ImVec4(0.082f, 0.106f, 0.145f, 1.0f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.125f, 0.161f, 0.220f, 1.0f);
        colors[ImGuiCol_TabUnfocused] = ImVec4(0.074f, 0.094f, 0.129f, 1.0f);
        colors[ImGuiCol_DockingPreview] = ImVec4(0.153f, 0.510f, 0.910f, 0.35f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.047f, 0.067f, 0.094f, 1.0f);
        colors[ImGuiCol_TableHeaderBg] = ImVec4(0.082f, 0.106f, 0.145f, 1.0f);
        colors[ImGuiCol_TableBorderStrong] = ImVec4(0.204f, 0.251f, 0.314f, 1.0f);
        colors[ImGuiCol_TableBorderLight] = ImVec4(0.157f, 0.196f, 0.251f, 1.0f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.071f, 0.090f, 0.122f, 0.55f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(0.153f, 0.510f, 0.910f, 0.35f);
        colors[ImGuiCol_DragDropTarget] = ImVec4(0.180f, 0.706f, 0.333f, 1.0f);
        colors[ImGuiCol_NavHighlight] = ImVec4(0.153f, 0.510f, 0.910f, 0.85f);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.031f, 0.043f, 0.063f, 0.78f);
    }
    else
        ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_TabActive] = style.Colors[ImGuiCol_WindowBg];
    style.Colors[ImGuiCol_TabUnfocusedActive] = style.Colors[ImGuiCol_WindowBg];
    style.WindowRounding = 8.0f;
    style.ChildRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.FrameRounding = 7.0f;
    style.GrabRounding = 7.0f;
    style.TabRounding = 8.0f;
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.CellPadding = ImVec2(10.0f, 8.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 13.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    const float user_scale = static_cast<float>(_ui_scale_percent) / 100.0f;
    style.ScaleAllSizes(user_scale * dpi_scale);
    style.FontSizeBase = 16.0f * user_scale;
    style._NextFrameFontSizeBase = style.FontSizeBase;
    style.FontScaleMain = 1.0f;
}

} // namespace Ggui

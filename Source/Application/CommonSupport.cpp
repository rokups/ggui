// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ggui::ApplicationInternal
{
namespace
{
ImFont* diff_font = nullptr;
}

float FontPx(float value)
{
    return value * ImGui::GetFontSize() / 16.0f;
}

std::string IconLabel(std::string_view icon, std::string_view label)
{
    return std::string(icon) + std::string(label) + "###" + std::string(label);
}

bool ActionMenuItem(std::string_view icon, std::string_view label, const char* shortcut,
    bool enabled)
{
    const std::string decorated = IconLabel(icon, label);
    return ImGui::MenuItem(decorated.c_str(), shortcut, false, enabled);
}

bool ActionButton(std::string_view icon, std::string_view label, const ImVec2& size)
{
    const std::string decorated = IconLabel(icon, label);
    return ImGui::Button(decorated.c_str(), size);
}

void IdCopyMenuItems(std::string_view name, std::string_view id, std::size_t unique_length)
{
    const std::size_t short_length = std::min(id.size(), std::max<std::size_t>(8, unique_length));
    const std::string short_label = "Short " + std::string(name);
    if (ActionMenuItem(ICON_MS_CONTENT_COPY, short_label, nullptr, !id.empty()))
        ImGui::SetClipboardText(std::string(id.substr(0, short_length)).c_str());
    const std::string full_label = "Full " + std::string(name);
    if (ActionMenuItem(ICON_MS_COPY_ALL, full_label, nullptr, !id.empty()))
        ImGui::SetClipboardText(std::string(id).c_str());
}

bool DangerButton(const char* label, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.17f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f, 0.08f, 0.08f, 1.0f));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

std::string LimitedLines(std::string_view text, std::size_t maximum)
{
    if (maximum == 0)
        return text.empty() ? "" : "...";
    std::size_t start = 0;
    for (std::size_t line = 0; line < maximum; ++line)
    {
        const std::size_t next = text.find('\n', start);
        if (next == std::string_view::npos)
            return std::string(text);
        if (line + 1 == maximum)
            return next + 1 == text.size() ? std::string(text) : std::string(text.substr(0, next)) + "...";
        start = next + 1;
    }
    return std::string(text);
}

std::string FormatTimestamp(std::int64_t timestamp)
{
    if (timestamp <= 0)
        return "Unknown date";
    const std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &value) != 0)
        return "Unknown date";
#else
    if (localtime_r(&value, &local) == nullptr)
        return "Unknown date";
#endif
    std::array<char, 17> result{};
    return std::strftime(result.data(), result.size(), "%Y-%m-%d %H:%M", &local) == 0
        ? "Unknown date" : result.data();
}

void LoadUiFont()
{
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.FontDataOwnedByAtlas = false;
    ImStrncpy(config.Name, "NotoSansMono.ttf", IM_ARRAYSIZE(config.Name));
    if (ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(ggui_ui_font_data),
            static_cast<int>(ggui_ui_font_data_len), 16.0f, &config) == nullptr)
        ImGui::GetIO().Fonts->AddFontDefault();
    static constexpr ImWchar ranges[]{0xe000, 0xf8ff, 0};
    ImFontConfig icons;
    icons.MergeMode = true;
    icons.PixelSnapH = true;
    icons.GlyphMinAdvanceX = 16.0f;
    icons.GlyphOffset = ImVec2(0.0f, 3.0f);
    icons.FontDataOwnedByAtlas = false;
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(ggui_icon_font_data),
        static_cast<int>(ggui_icon_font_data_len), 17.0f, &icons, ranges);

    ImFontConfig diff;
    diff.OversampleH = 2;
    diff.OversampleV = 2;
    diff.FontDataOwnedByAtlas = false;
    ImStrncpy(diff.Name, "JetBrainsMono.ttf", IM_ARRAYSIZE(diff.Name));
    diff_font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(ggui_diff_font_data),
        static_cast<int>(ggui_diff_font_data_len), 16.0f, &diff);
    if (diff_font == nullptr)
        diff_font = ImGui::GetIO().Fonts->Fonts.front();
}

ImFont* DiffFont()
{
    return diff_font;
}

std::vector<std::string> SplitLines(std::string_view text)
{
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin < text.size())
    {
        const std::size_t end = text.find_first_of("\n,", begin);
        std::string value(text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin));
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) { return !std::isspace(c); }).base(),
            value.end());
        if (!value.empty())
            result.push_back(std::move(value));
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return result;
}

bool HasText(std::string_view value)
{
    return std::ranges::any_of(value, [](unsigned char character) { return !std::isspace(character); });
}

const char* DeltaName(git_delta_t status)
{
    switch (status)
    {
    case GIT_DELTA_ADDED: return "A";
    case GIT_DELTA_DELETED: return "D";
    case GIT_DELTA_MODIFIED: return "M";
    case GIT_DELTA_RENAMED: return "R";
    case GIT_DELTA_COPIED: return "C";
    case GIT_DELTA_TYPECHANGE: return "T";
    case GIT_DELTA_UNTRACKED: return "?";
    case GIT_DELTA_IGNORED: return "I";
    case GIT_DELTA_CONFLICTED: return "C";
    default: return " ";
    }
}

bool ContainsInsensitive(std::string_view haystack, std::string_view needle)
{
    return std::ranges::search(haystack, needle, [](char left, char right) {
        return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
    }).begin()
        != haystack.end();
}

std::string FileUrl(const std::string& path)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::string result = "file://";
#ifdef _WIN32
    result += '/';
#endif
    for (const unsigned char character : normalized)
    {
        if (std::isalnum(character) != 0 || std::string_view("-._~/:@").find(character) != std::string_view::npos)
            result += static_cast<char>(character);
        else
        {
            result += '%';
            result += hex[character >> 4];
            result += hex[character & 0x0f];
        }
    }
    return result;
}

std::string RepositoryName(const std::string& root)
{
    const std::filesystem::path path(root);
    const std::filesystem::path name = path.filename().empty() ? path.parent_path().filename() : path.filename();
    return name.empty() ? root : name.string();
}

std::string LimitedFragment(std::string_view text, std::size_t maximum)
{
    return text.size() <= maximum ? std::string(text)
                                  : std::string(text.substr(0, maximum - 3)) + "...";
}

} // namespace Ggui::ApplicationInternal

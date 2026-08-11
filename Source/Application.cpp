// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Application.hpp"

#include <SDL3/SDL_opengl.h>
#include <TextDiff.h>
#include <TextEditor.h>
#include <IconsMaterialSymbols.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#ifdef IMGUI_BUILD_TESTING
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#endif
#include <nfd.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Ggui
{

namespace
{

constexpr float kRowHeight = 34.0f;
constexpr float kLaneWidth = 20.0f;
constexpr float kDotRadius = 5.0f;
constexpr float kGraphPadding = 12.0f;

constexpr ImU32 kTextMuted = IM_COL32(125, 133, 144, 255);
constexpr ImU32 kBadgeTextMuted = IM_COL32(255, 255, 255, 145);
constexpr ImU32 kRowBackground = IM_COL32(22, 27, 34, 255);
constexpr ImU32 kRowHover = IM_COL32(28, 34, 43, 255);
constexpr ImU32 kRowSelected = IM_COL32(33, 52, 74, 255);
constexpr ImU32 kRowBorder = IM_COL32(48, 54, 61, 180);
constexpr ImU32 kGraphBackground = IM_COL32(18, 22, 29, 255);
constexpr ImU32 kBadgeBookmark = IM_COL32(9, 105, 218, 235);
constexpr ImU32 kBadgeBookmarkSynced = IM_COL32(31, 136, 61, 235);
constexpr ImU32 kBadgeBookmarkDiverged = IM_COL32(219, 109, 40, 235);
constexpr ImU32 kBadgeTag = IM_COL32(88, 70, 155, 235);
constexpr ImU32 kBadgeRemote = IM_COL32(66, 68, 90, 235);
constexpr ImU32 kBadgeWorkingCopy = IM_COL32(31, 136, 61, 235);
constexpr ImU32 kStatusAdded = IM_COL32(46, 160, 67, 255);
constexpr ImU32 kStatusModified = IM_COL32(210, 153, 34, 255);
constexpr ImU32 kStatusDeleted = IM_COL32(248, 81, 73, 255);
constexpr ImU32 kStatusRenamed = IM_COL32(47, 129, 247, 255);
constexpr ImU32 kStatusSpecial = IM_COL32(166, 91, 216, 255);
constexpr ImU32 kStatusConflict = IM_COL32(255, 123, 114, 255);
constexpr ImU32 kStatusPushed = IM_COL32(246, 248, 250, 255);
constexpr ImU32 kStatusUnpushed = IM_COL32(219, 109, 40, 255);
constexpr ImU32 kChangeId = IM_COL32(166, 91, 216, 255);
constexpr ImU32 kWorkingChangeId = IM_COL32(225, 113, 247, 255);
constexpr ImU32 kCommitId = IM_COL32(47, 129, 247, 255);
constexpr ImU32 kWorkingCommitId = IM_COL32(100, 181, 246, 255);

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

constexpr std::array<ImU32, 8> kLaneColors{
    IM_COL32(47, 129, 247, 255), IM_COL32(166, 91, 216, 255), IM_COL32(46, 160, 67, 255),
    IM_COL32(210, 153, 34, 255), IM_COL32(248, 81, 73, 255), IM_COL32(57, 197, 187, 255),
    IM_COL32(219, 109, 40, 255), IM_COL32(163, 113, 247, 255)};

float FontPx(float value)
{
    return value * ImGui::GetFontSize() / 16.0f;
}

std::string IconLabel(std::string_view icon, std::string_view label)
{
    return std::string(icon) + "  " + std::string(label) + "###" + std::string(label);
}

bool ActionMenuItem(std::string_view icon, std::string_view label, const char* shortcut = nullptr,
    bool enabled = true)
{
    const std::string decorated = IconLabel(icon, label);
    return ImGui::MenuItem(decorated.c_str(), shortcut, false, enabled);
}

bool ActionButton(std::string_view icon, std::string_view label, const ImVec2& size = {})
{
    const std::string decorated = IconLabel(icon, label);
    return ImGui::Button(decorated.c_str(), size);
}

bool ActionSmallButton(std::string_view icon, std::string_view label)
{
    const std::string decorated = IconLabel(icon, label);
    return ImGui::SmallButton(decorated.c_str());
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

bool DangerButton(const char* label, const ImVec2& size = {})
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
#ifdef _WIN32
    constexpr std::array paths{"C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf"};
#elif defined(__APPLE__)
    constexpr std::array paths{"/System/Library/Fonts/Monaco.ttf", "/System/Library/Fonts/Menlo.ttc"};
#else
    constexpr std::array paths{"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf"};
#endif
    bool loaded = false;
    for (const char* path : paths)
    {
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 2;
        if (std::filesystem::exists(path)
            && ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 16.0f, &config) != nullptr)
        {
            loaded = true;
            break;
        }
    }
    if (!loaded)
        ImGui::GetIO().Fonts->AddFontDefault();
    static constexpr ImWchar ranges[]{0xe000, 0xf8ff, 0};
    ImFontConfig icons;
    icons.MergeMode = true;
    icons.PixelSnapH = true;
    icons.GlyphMinAdvanceX = 16.0f;
    icons.GlyphOffset = ImVec2(0.0f, 3.0f);
    std::vector<std::filesystem::path> icon_paths{GGUI_ICON_FONT_PATH};
    if (const char* base = SDL_GetBasePath(); base != nullptr)
        icon_paths.emplace_back(std::filesystem::path(base) / ".." / "share" / "ggui" / "Fonts"
            / "MaterialSymbolsOutlined.ttf");
    for (const std::filesystem::path& path : icon_paths)
        if (std::filesystem::exists(path)
            && ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), 17.0f, &icons, ranges) != nullptr)
            break;
}

ImU32 StatusColor(git_delta_t status)
{
    switch (status)
    {
    case GIT_DELTA_ADDED: return kStatusAdded;
    case GIT_DELTA_UNTRACKED: return kStatusAdded;
    case GIT_DELTA_DELETED: return kStatusDeleted;
    case GIT_DELTA_MODIFIED: return kStatusModified;
    case GIT_DELTA_RENAMED: return kStatusRenamed;
    case GIT_DELTA_COPIED:
    case GIT_DELTA_TYPECHANGE: return kStatusSpecial;
    case GIT_DELTA_CONFLICTED: return kStatusConflict;
    default: return kTextMuted;
    }
}

const TextEditor::Language* DiffLanguage(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (extension == ".c") return TextEditor::Language::C();
    if (extension == ".cc" || extension == ".cpp" || extension == ".cxx" || extension == ".h"
        || extension == ".hh" || extension == ".hpp" || extension == ".hxx")
        return TextEditor::Language::Cpp();
    if (extension == ".cs") return TextEditor::Language::Cs();
    if (extension == ".as") return TextEditor::Language::AngelScript();
    if (extension == ".lua") return TextEditor::Language::Lua();
    if (extension == ".py") return TextEditor::Language::Python();
    if (extension == ".glsl" || extension == ".vert" || extension == ".frag")
        return TextEditor::Language::Glsl();
    if (extension == ".hlsl") return TextEditor::Language::Hlsl();
    if (extension == ".json") return TextEditor::Language::Json();
    if (extension == ".md") return TextEditor::Language::Markdown();
    if (extension == ".sql") return TextEditor::Language::Sql();
    return nullptr;
}

constexpr std::array kDiffContextChoices{0, 1, 3, 5, 10, 25, -1};

int WhitespaceModeIndex(DiffWhitespaceMode mode)
{
    switch (mode)
    {
    case DiffWhitespaceMode::IgnoreWhitespace: return 1;
    case DiffWhitespaceMode::IgnoreAllWhitespace: return 2;
    default: return 0;
    }
}

DiffWhitespaceMode WhitespaceModeFromIndex(int index)
{
    switch (index)
    {
    case 1: return DiffWhitespaceMode::IgnoreWhitespace;
    case 2: return DiffWhitespaceMode::IgnoreAllWhitespace;
    default: return DiffWhitespaceMode::Normal;
    }
}

int ContextLineChoiceIndex(int context_lines)
{
    const auto choice = std::ranges::find(kDiffContextChoices, context_lines);
    return choice == kDiffContextChoices.end() ? 2 : static_cast<int>(choice - kDiffContextChoices.begin());
}

const char* ContextLineChoiceLabel(int index)
{
    switch (kDiffContextChoices[static_cast<std::size_t>(index)])
    {
    case 0: return "0 lines";
    case 1: return "1 line";
    case 3: return "3 lines";
    case 5: return "5 lines";
    case 10: return "10 lines";
    case 25: return "25 lines";
    default: return "Full";
    }
}

bool IsImagePath(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::ranges::transform(
        extension, extension.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    constexpr std::array<std::string_view, 9> extensions{
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".tga", ".svg", ".ico"};
    return std::ranges::find(extensions, extension) != extensions.end();
}

bool IsSymlinkMode(unsigned int mode)
{
    return (mode & 0170000U) == 0120000U;
}

bool IsSubmoduleMode(unsigned int mode)
{
    return (mode & 0170000U) == 0160000U;
}

std::string FormatFileMode(unsigned int mode)
{
    if (mode == 0)
        return "none";
    std::array<char, 16> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%06o", mode);
    return buffer.data();
}

const char* ModeKind(unsigned int mode)
{
    if (IsSubmoduleMode(mode))
        return "submodule";
    if (IsSymlinkMode(mode))
        return "symlink";
    if ((mode & 0111U) != 0)
        return "executable file";
    return mode == 0 ? "none" : "file";
}

ImU32 BookmarkBadgeColor(std::string_view name, const std::vector<NamedRef>& refs)
{
    const auto local = std::ranges::find_if(refs, [&](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == name;
    });
    const bool remote = std::ranges::any_of(refs, [&](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.name == name;
    });
    if (local == refs.end())
        return kBadgeRemote;
    if (!remote)
        return kBadgeBookmark;
    const bool synchronized = std::ranges::all_of(refs, [&](const NamedRef& ref) {
        return ref.kind != GG_NAMED_REF_REMOTE_BOOKMARK || ref.name != name || ref.target == local->target;
    });
    return synchronized ? kBadgeBookmarkSynced : kBadgeBookmarkDiverged;
}

ImU32 RefBadgeColor(const NamedRef& ref, const std::vector<NamedRef>& refs)
{
    if (ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK || ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK)
        return BookmarkBadgeColor(ref.name, refs);
    if (ref.kind == GG_NAMED_REF_LOCAL_TAG)
        return kBadgeTag;
    return kBadgeRemote;
}

std::string ReferenceLabel(const NamedRef& ref)
{
    return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && !ref.remote.empty()
        ? ref.remote + "/" + ref.name : ref.name;
}

std::pair<std::string, std::size_t> ReferenceBadgeLabel(const NamedRef& ref, const std::vector<NamedRef>& refs)
{
    if (ref.kind != GG_NAMED_REF_LOCAL_BOOKMARK && ref.kind != GG_NAMED_REF_REMOTE_BOOKMARK)
        return {ReferenceLabel(ref), 0};
    const auto local = std::ranges::find_if(refs, [&](const NamedRef& candidate) {
        return candidate.kind == GG_NAMED_REF_LOCAL_BOOKMARK && candidate.name == ref.name
            && candidate.target == ref.target;
    });
    const auto remote = std::ranges::find_if(refs, [&](const NamedRef& candidate) {
        return candidate.kind == GG_NAMED_REF_REMOTE_BOOKMARK && candidate.name == ref.name
            && candidate.target == ref.target && !candidate.remote.empty();
    });
    if (local == refs.end() || remote == refs.end())
        return {ReferenceLabel(ref), 0};
    if (ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK)
        return {};
    return {ReferenceLabel(*remote), remote->remote.size() + 1};
}

const Remote* DefaultRemote(const RepoSnapshot& snapshot)
{
    const auto origin = std::ranges::find(snapshot.remotes, "origin", &Remote::name);
    return origin != snapshot.remotes.end() ? &*origin
                                            : snapshot.remotes.empty() ? nullptr : &snapshot.remotes.front();
}

const NamedRef* BookmarkAt(const RepoSnapshot& snapshot, const std::string& revision)
{
    const auto tracked = std::ranges::find_if(snapshot.refs, [&](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.target == revision && ref.tracked;
    });
    if (tracked != snapshot.refs.end())
        return &*tracked;
    const auto local = std::ranges::find_if(snapshot.refs, [&](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.target == revision;
    });
    return local == snapshot.refs.end() ? nullptr : &*local;
}

std::string RemoteForBookmark(const RepoSnapshot& snapshot, std::string_view bookmark)
{
    const auto tracked = std::ranges::find_if(snapshot.refs, [&](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.name == bookmark
            && std::ranges::find(snapshot.remotes, ref.remote, &Remote::name) != snapshot.remotes.end();
    });
    if (tracked != snapshot.refs.end())
        return tracked->remote;
    const Remote* remote = DefaultRemote(snapshot);
    return remote == nullptr ? "" : remote->name;
}

std::string RefRemotes(const std::vector<NamedRef>& refs, std::string_view name, gg_named_ref_kind kind)
{
    std::string result;
    for (const NamedRef& ref : refs)
    {
        if (ref.kind != kind || ref.name != name || ref.remote.empty())
            continue;
        if (!result.empty()) result += ", ";
        result += ref.remote;
    }
    return result;
}

void DrawBadge(ImDrawList* draw, ImVec2& cursor, float center_y, std::string_view label, ImU32 color,
    std::size_t dimmed_prefix = 0)
{
    const ImVec2 text_size = ImGui::CalcTextSize(label.data(), label.data() + label.size());
    const float pad_x = FontPx(7.0f);
    const float pad_top = FontPx(3.0f);
    const float pad_bottom = 0.0f;
    const ImVec2 minimum(cursor.x, center_y - text_size.y * 0.5f - pad_top);
    const ImVec2 maximum(cursor.x + text_size.x + pad_x * 2.0f, center_y + text_size.y * 0.5f + pad_bottom);
    draw->AddRectFilled(minimum, maximum, color, FontPx(6.0f));
    ImVec2 text(minimum.x + pad_x, minimum.y + pad_top);
    if (dimmed_prefix != 0)
    {
        draw->AddText(text, kBadgeTextMuted, label.data(), label.data() + dimmed_prefix);
        text.x += ImGui::CalcTextSize(label.data(), label.data() + dimmed_prefix).x;
    }
    draw->AddText(text, IM_COL32_WHITE, label.data() + dimmed_prefix, label.data() + label.size());
    cursor.x = maximum.x + FontPx(6.0f);
}

void DrawElidedText(
    ImDrawList* draw, ImVec2 position, float maximum_x, std::string_view text, ImU32 color)
{
    if (maximum_x <= position.x)
        return;
    const ImVec2 size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::RenderTextEllipsis(draw, position,
        ImVec2(maximum_x, position.y + ImGui::GetTextLineHeight()), maximum_x,
        text.data(), text.data() + text.size(), &size);
    ImGui::PopStyleColor();
}

bool DrawTextWithin(ImDrawList* draw, ImVec2 position, float maximum_x, std::string_view text, ImU32 color)
{
    const float width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
    if (position.x + width <= maximum_x)
    {
        draw->AddText(position, color, text.data(), text.data() + text.size());
        return false;
    }
    DrawElidedText(draw, position, maximum_x, text, color);
    return true;
}

void DrawElidedBadge(
    ImDrawList* draw, ImVec2 cursor, float center_y, float maximum_x, std::string_view label, ImU32 color,
    std::size_t dimmed_prefix = 0)
{
    if (maximum_x <= cursor.x)
        return;
    const float pad_x = FontPx(7.0f);
    const float pad_top = FontPx(3.0f);
    const float pad_bottom = 0.0f;
    const float text_y = center_y - ImGui::GetTextLineHeight() * 0.5f;
    const ImVec2 minimum(cursor.x, text_y - pad_top);
    const ImVec2 maximum(maximum_x, text_y + ImGui::GetTextLineHeight() + pad_bottom);
    draw->AddRectFilled(minimum, maximum, color, FontPx(6.0f));
    ImVec2 text(minimum.x + pad_x, text_y);
    if (dimmed_prefix != 0)
    {
        if (DrawTextWithin(draw, text, maximum.x - pad_x, label.substr(0, dimmed_prefix), kBadgeTextMuted))
            return;
        text.x += ImGui::CalcTextSize(label.data(), label.data() + dimmed_prefix).x;
    }
    DrawElidedText(draw, text, maximum.x - pad_x, label.substr(dimmed_prefix), IM_COL32_WHITE);
}

bool BadgedSelectable(std::string_view label, bool selected, float height, ImU32 color,
    std::string_view suffix = {}, bool* out_elided = nullptr)
{
    const std::string id = "###" + std::string(label);
    const bool clicked = ImGui::Selectable(id.c_str(), selected, 0, ImVec2(0.0f, height));
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(
        minimum, ImVec2(minimum.x + 4.0f, maximum.y), color, 4.0f, ImDrawFlags_RoundCornersLeft);
    ImVec2 text(minimum.x + 12.0f, minimum.y + 3.0f);
    bool elided = DrawTextWithin(draw, text, maximum.x - 8.0f, label, ImGui::GetColorU32(ImGuiCol_Text));
    if (!elided && !suffix.empty())
    {
        text.x += ImGui::CalcTextSize(label.data(), label.data() + label.size()).x + 8.0f;
        elided = DrawTextWithin(draw, text, maximum.x - 8.0f, suffix, kTextMuted);
    }
    if (out_elided != nullptr)
        *out_elided = elided;
    return clicked;
}

float DrawHighlightedId(
    ImDrawList* draw, ImVec2 position, std::string_view id, std::size_t unique_length, ImU32 prefix_color)
{
    const std::size_t shown = std::min(id.size(), std::max<std::size_t>(8, unique_length));
    const std::size_t unique = std::min(shown, unique_length);
    draw->AddText(position, prefix_color, id.data(), id.data() + unique);
    position.x += ImGui::CalcTextSize(id.data(), id.data() + unique).x;
    draw->AddText(position, kTextMuted, id.data() + unique, id.data() + shown);
    return position.x + ImGui::CalcTextSize(id.data() + unique, id.data() + shown).x;
}

bool DrawHighlightedIdWithin(ImDrawList* draw, ImVec2 position, float maximum_x, std::string_view id,
    std::size_t unique_length, ImU32 prefix_color)
{
    const std::size_t shown = std::min(id.size(), std::max<std::size_t>(8, unique_length));
    if (position.x + ImGui::CalcTextSize(id.data(), id.data() + shown).x <= maximum_x)
    {
        DrawHighlightedId(draw, position, id, unique_length, prefix_color);
        return false;
    }
    DrawElidedText(draw, position, maximum_x, std::string_view(id.data(), shown), prefix_color);
    return true;
}

void TextHighlightedId(std::string_view id, std::size_t unique_length, ImU32 prefix_color)
{
    const std::size_t shown = std::min(id.size(), std::max<std::size_t>(8, unique_length));
    const std::size_t unique = std::min(shown, unique_length);
    ImGui::PushStyleColor(ImGuiCol_Text, prefix_color);
    ImGui::TextUnformatted(id.data(), id.data() + unique);
    ImGui::PopStyleColor();
    if (unique != shown)
    {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled("%.*s", static_cast<int>(shown - unique), id.data() + unique);
    }
}

void TextLabelledId(std::string_view label, std::string_view id, std::size_t unique_length, ImU32 prefix_color)
{
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    ImGui::SameLine(0.0f, 0.0f);
    TextHighlightedId(id, unique_length, prefix_color);
}

void DialogInput(const char* label, const char* hint, std::string* value, bool focus = false,
    ImGuiInputTextFlags flags = 0)
{
    ImGui::TextUnformatted(label);
    if (focus)
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    const std::string id = std::string("###") + label;
    ImGui::InputTextWithHint(id.c_str(), hint, value, flags);
}

void DialogMultiline(const char* label, std::string* value, float height, bool focus = false)
{
    ImGui::TextUnformatted(label);
    if (focus)
        ImGui::SetKeyboardFocusHere();
    const std::string id = std::string("###") + label;
    ImGui::InputTextMultiline(id.c_str(), value, ImVec2(-1.0f, height));
}

ImU32 ChangeIdColor(bool working)
{
    return working ? kWorkingChangeId : kChangeId;
}

ImU32 CommitIdColor(bool working)
{
    return working ? kWorkingCommitId : kCommitId;
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
    case GIT_DELTA_CONFLICTED: return "!";
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

} // namespace

#ifdef IMGUI_BUILD_TESTING
void RegisterUiTests(ImGuiTestEngine* engine);
#endif

int Application::Run(int argc, char** argv)
{
#ifdef IMGUI_BUILD_TESTING
    test_application = this;
    std::string repository_path;
    std::string test_filter = "all";
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "--test")
            _test_mode = true;
        else if (argument == "--smoke")
            _smoke_mode = true;
        else if (argument.starts_with("--test="))
        {
            _test_mode = true;
            test_filter = argument.substr(7);
        }
        else
            repository_path = argument;
    }
#endif
    if (!Initialize())
        return 1;
#ifdef IMGUI_BUILD_TESTING
    if (_test_mode)
        InitializeTestEngine(test_filter);
    else if (!repository_path.empty())
        _engine.Enqueue(OpenRepository{repository_path});
#else
    if (argc > 1)
        _engine.Enqueue(OpenRepository{argv[1]});
#endif
    else if (!_recent_repositories.empty() && std::filesystem::exists(_recent_repositories.front()))
        _engine.Enqueue(OpenRepository{_recent_repositories.front()});

    while (_running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            ProcessEvent(event);
        }

        PollEngine();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        RenderFrame();
        ImGui::Render();

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(_window, &width, &height);
        glViewport(0, 0, width, height);
        if (_dark_theme)
            glClearColor(0.047f, 0.067f, 0.094f, 1.0f);
        else
            glClearColor(0.94f, 0.95f, 0.97f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(_window);
#ifdef IMGUI_BUILD_TESTING
        if (_smoke_mode)
            _running = false;
        if (_test_engine != nullptr)
        {
            ImGuiTestEngine_PostSwap(_test_engine);
            if (_test_mode && ImGuiTestEngine_IsTestQueueEmpty(_test_engine))
            {
                ImGuiTestEngineResultSummary summary;
                ImGuiTestEngine_GetResultSummary(_test_engine, &summary);
                _test_result = summary.CountTested > 0 && summary.CountTested == summary.CountSuccess ? 0 : 1;
                _running = false;
            }
        }
#endif
    }
    Shutdown();
#ifdef IMGUI_BUILD_TESTING
    test_application = nullptr;
    if (_test_mode)
        return _test_result;
#endif
    return 0;
}

void Application::ProcessEvent(SDL_Event& event)
{
    if (event.type == SDL_EVENT_QUIT
        || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(_window)))
        _running = false;
    if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr && _active_operation.empty())
        _engine.Enqueue(OpenRepository{event.drop.data});
    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST
        && event.window.windowID == SDL_GetWindowID(_window))
    {
        const SDL_WindowFlags flags = SDL_GetWindowFlags(_window);
        _window_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
        const bool normal = (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN)) == 0;
        if (normal && event.type == SDL_EVENT_WINDOW_MOVED)
        {
            _window_x = event.window.data1;
            _window_y = event.window.data2;
            _window_has_position = true;
        }
        if (normal && event.type == SDL_EVENT_WINDOW_RESIZED)
        {
            _window_width = event.window.data1;
            _window_height = event.window.data2;
            _window_has_size = true;
        }
        if (event.type == SDL_EVENT_WINDOW_MOVED || event.type == SDL_EVENT_WINDOW_RESIZED
            || event.type == SDL_EVENT_WINDOW_MAXIMIZED || event.type == SDL_EVENT_WINDOW_RESTORED)
            ImGui::MarkIniSettingsDirty();
    }
}

bool Application::Initialize()
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        spdlog::error("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    _window = SDL_CreateWindow(
        "ggui", 1440, 900, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (_window == nullptr)
    {
        spdlog::error("SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return false;
    }
    _window_has_position = SDL_GetWindowPosition(_window, &_window_x, &_window_y);
    _window_has_size = SDL_GetWindowSize(_window, &_window_width, &_window_height);
    _gl_context = SDL_GL_CreateContext(_window);
    if (_gl_context == nullptr)
    {
        spdlog::error("SDL_GL_CreateContext failed: {}", SDL_GetError());
        SDL_DestroyWindow(_window);
        _window = nullptr;
        SDL_Quit();
        return false;
    }
    SDL_GL_SetSwapInterval(1);

    char* preferences = SDL_GetPrefPath("gg", "ggui");
    if (preferences != nullptr)
    {
        _settings_path = std::filesystem::path(preferences) / "settings.json";
        _imgui_ini_path = (std::filesystem::path(preferences) / "imgui.ini").string();
        SDL_free(preferences);
    }
    LoadSettings();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = _imgui_ini_path.empty() ? nullptr : _imgui_ini_path.c_str();
    RegisterWindowSettings();
    if (io.IniFilename != nullptr && std::filesystem::exists(io.IniFilename))
        ImGui::LoadIniSettingsFromDisk(io.IniFilename);
    LoadUiFont();
    ApplyTheme();
    ImGui_ImplSDL3_InitForOpenGL(_window, _gl_context);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    return true;
}

void Application::RegisterWindowSettings()
{
    ImGuiSettingsHandler handler;
    handler.TypeName = "Ggui";
    handler.TypeHash = ImHashStr(handler.TypeName);
    handler.ReadOpenFn = WindowSettingsReadOpen;
    handler.ReadLineFn = WindowSettingsReadLine;
    handler.ApplyAllFn = WindowSettingsApplyAll;
    handler.WriteAllFn = WindowSettingsWriteAll;
    handler.UserData = this;
    ImGui::AddSettingsHandler(&handler);
}

void* Application::WindowSettingsReadOpen(
    ImGuiContext*, ImGuiSettingsHandler* handler, const char* name)
{
    if (std::strcmp(name, "MainWindow") != 0)
        return nullptr;
    auto* application = static_cast<Application*>(handler->UserData);
    application->_window_has_position = false;
    application->_window_has_size = false;
    application->_window_maximized = false;
    return application;
}

void Application::WindowSettingsReadLine(
    ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
{
    auto* application = static_cast<Application*>(entry);
    int first = 0;
    int second = 0;
    if (std::sscanf(line, "Pos=%d,%d", &first, &second) == 2)
    {
        application->_window_x = first;
        application->_window_y = second;
        application->_window_has_position = true;
    }
    else if (std::sscanf(line, "Size=%d,%d", &first, &second) == 2 && first >= 320 && second >= 240)
    {
        application->_window_width = first;
        application->_window_height = second;
        application->_window_has_size = true;
    }
    else if (std::sscanf(line, "Maximized=%d", &first) == 1)
        application->_window_maximized = first != 0;
}

void Application::WindowSettingsApplyAll(ImGuiContext*, ImGuiSettingsHandler* handler)
{
    auto* application = static_cast<Application*>(handler->UserData);
    if (application->_window == nullptr)
        return; // GCOV_EXCL_LINE: handler is registered only while the SDL window exists
    const SDL_WindowFlags flags = SDL_GetWindowFlags(application->_window);
    if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) != 0)
        SDL_RestoreWindow(application->_window); // GCOV_EXCL_LINE: headless Xvfb has no window manager state to restore
    if (application->_window_has_size)
        SDL_SetWindowSize(application->_window, application->_window_width, application->_window_height);
    if (application->_window_has_position)
        SDL_SetWindowPosition(application->_window, application->_window_x, application->_window_y);
    if (application->_window_maximized)
        SDL_MaximizeWindow(application->_window);
}

void Application::WindowSettingsWriteAll(
    ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* output)
{
    auto* application = static_cast<Application*>(handler->UserData);
    if (application->_window != nullptr)
    {
        const SDL_WindowFlags flags = SDL_GetWindowFlags(application->_window);
        application->_window_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
        if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN)) == 0)
        {
            application->_window_has_position =
                SDL_GetWindowPosition(application->_window, &application->_window_x, &application->_window_y);
            application->_window_has_size =
                SDL_GetWindowSize(application->_window, &application->_window_width, &application->_window_height);
        }
    }
    output->appendf("[%s][MainWindow]\n", handler->TypeName);
    output->appendf("Pos=%d,%d\n", application->_window_x, application->_window_y);
    output->appendf("Size=%d,%d\n", application->_window_width, application->_window_height);
    output->appendf("Maximized=%d\n\n", application->_window_maximized ? 1 : 0);
}

void Application::Shutdown()
{
    SaveSettings();
#ifdef IMGUI_BUILD_TESTING
    if (_test_engine != nullptr)
        ImGuiTestEngine_Stop(_test_engine);
#endif
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
#ifdef IMGUI_BUILD_TESTING
    if (_test_engine != nullptr)
    {
        ImGuiTestEngine_DestroyContext(_test_engine);
        _test_engine = nullptr;
    }
#endif
    if (_gl_context != nullptr)
        SDL_GL_DestroyContext(_gl_context);
    if (_window != nullptr)
        SDL_DestroyWindow(_window);
    _gl_context = nullptr;
    _window = nullptr;
    SDL_Quit();
}

void Application::LoadSettings()
{
    if (_settings_path.empty() || !std::filesystem::exists(_settings_path))
        return;
    try
    {
        std::ifstream input(_settings_path);
        nlohmann::json json;
        input >> json;
        _recent_repositories = json.value("recentRepositories", std::vector<std::string>{});
        _default_layout = json.value("defaultLayout", true);
        _diff_side_by_side = json.value("diffSideBySide", false);
        const int whitespace = json.value("diffWhitespaceMode", 0);
        _diff_whitespace_mode = WhitespaceModeFromIndex(whitespace);
        const int context_lines = json.value("diffContextLines", 3);
        _diff_context_lines =
            std::ranges::find(kDiffContextChoices, context_lines) == kDiffContextChoices.end() ? 3 : context_lines;
    }
    catch (const std::exception& error)
    {
        spdlog::warn("Could not load settings: {}", error.what());
    }
}

void Application::SaveSettings()
{
    if (_settings_path.empty())
        return; // GCOV_EXCL_LINE: SDL supplied no preferences directory
    try
    {
        std::filesystem::create_directories(_settings_path.parent_path());
        const nlohmann::json json{{"recentRepositories", _recent_repositories}, {"defaultLayout", _default_layout},
            {"diffSideBySide", _diff_side_by_side}, {"diffWhitespaceMode", WhitespaceModeIndex(_diff_whitespace_mode)},
            {"diffContextLines", _diff_context_lines}};
        const std::filesystem::path temporary = _settings_path.string() + ".tmp";
        std::ofstream(temporary) << json.dump(2) << '\n';
        std::error_code error;
        std::filesystem::rename(temporary, _settings_path, error);
        if (error)
        {
            std::filesystem::remove(_settings_path, error);
            std::filesystem::rename(temporary, _settings_path, error);
        }
    }
    catch (const std::exception& error) // GCOV_EXCL_START: filesystem/allocator failure while persisting settings
    {
        spdlog::warn("Could not save settings: {}", error.what());
    }
    // GCOV_EXCL_STOP
}

void Application::RememberRepository(const std::string& path)
{
    _recent_repositories.erase(
        std::remove(_recent_repositories.begin(), _recent_repositories.end(), path), _recent_repositories.end());
    _recent_repositories.insert(_recent_repositories.begin(), path);
    if (_recent_repositories.size() > 10)
        _recent_repositories.resize(10);
}

void Application::PollEngine()
{
#ifdef IMGUI_BUILD_TESTING
    if (_test_snapshot_mode)
    {
        _engine.PollEvents();
        return;
    }
#endif
    for (Event& event : _engine.PollEvents())
        ApplyEvent(std::move(event));
}

void Application::ApplyEvent(Event event)
{
    std::visit(
            [this](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, SnapshotReady>)
                {
                    const bool had_snapshot = _snapshot != nullptr;
                    const std::string old_root = _snapshot == nullptr ? "" : _snapshot->root;
                    const std::string old_working = _snapshot == nullptr ? "" : _snapshot->working_copy;
                    const std::string old_selection = _selected_revision;
                    const std::string old_compare_to = _compare_to;
                    const bool had_selection = !_selected_revisions.empty();
                    std::unordered_map<std::string, std::string> selected_changes;
                    if (_snapshot != nullptr)
                    {
                        for (const Revision& revision : _snapshot->revisions)
                            if (!revision.change_id.empty()
                                && std::ranges::find(_selected_revisions, revision.oid) != _selected_revisions.end())
                                selected_changes.emplace(revision.oid, revision.change_id);
                    }
                    _snapshot = std::move(value.snapshot);
                    SDL_SetWindowTitle(_window, (RepositoryName(_snapshot->root) + " - ggui").c_str());
                    RebuildIdPrefixes();
                    RememberRepository(_snapshot->root);
                    _graph_generation = 0;
                    const bool repository_changed = old_root != _snapshot->root;
                    if (repository_changed)
                    {
                        _preferred_file.clear();
                        _compare_to.clear();
                        _file_comparison = false;
                    }
                    if (repository_changed)
                    {
                        _selected_revision = !_snapshot->working_copy.empty() ? _snapshot->working_copy
                            : _snapshot->revisions.empty()                    ? ""
                                                                             : _snapshot->revisions.front().oid;
                        _selected_revisions = _selected_revision.empty() ? std::vector<std::string>{}
                                                                        : std::vector{_selected_revision};
                    }
                    else
                    {
                        if (old_working.empty() && !_snapshot->working_copy.empty())
                        {
                            _selected_revision = _snapshot->working_copy;
                            _selected_revisions = {_selected_revision};
                        }
                        else if (old_working != _snapshot->working_copy)
                        {
                            const auto old = std::ranges::find(_selected_revisions, old_working);
                            if (old != _selected_revisions.end())
                            {
                                const auto replacement = std::ranges::find(_selected_revisions, _snapshot->working_copy);
                                if (_snapshot->working_copy.empty() || replacement != _selected_revisions.end())
                                    _selected_revisions.erase(old);
                                else
                                    *old = _snapshot->working_copy;
                                if (_selected_revision == old_working)
                                    _selected_revision = _snapshot->working_copy;
                            }
                        }
                        for (std::string& oid : _selected_revisions)
                        {
                            if (std::ranges::any_of(
                                    _snapshot->revisions, [&](const Revision& revision) { return revision.oid == oid; }))
                                continue;
                            const auto change = selected_changes.find(oid);
                            const auto replacement = change == selected_changes.end()
                                ? _snapshot->revisions.end()
                                : std::ranges::find(_snapshot->revisions, change->second, &Revision::change_id);
                            if (replacement != _snapshot->revisions.end())
                            {
                                if (_selected_revision == oid)
                                    _selected_revision = replacement->oid;
                                oid = replacement->oid;
                            }
                        }
                        std::vector<std::string> unique_selection;
                        for (const std::string& oid : _selected_revisions)
                            if (std::ranges::find(unique_selection, oid) == unique_selection.end())
                                unique_selection.push_back(oid);
                        _selected_revisions = std::move(unique_selection);
                        std::erase_if(_selected_revisions, [this](const std::string& oid) {
                            return std::ranges::none_of(
                                _snapshot->revisions, [&](const Revision& revision) { return revision.oid == oid; });
                        });
                        if (std::ranges::find(_selected_revisions, _selected_revision) == _selected_revisions.end())
                        {
                            if (!_selected_revisions.empty())
                                _selected_revision = _selected_revisions.back();
                            else
                            {
                                _selected_revision = had_selection && !_snapshot->working_copy.empty()
                                    ? _snapshot->working_copy
                                    : had_selection && !_snapshot->revisions.empty()
                                    ? _snapshot->revisions.front().oid
                                    : "";
                                if (!_selected_revision.empty())
                                    _selected_revisions.push_back(_selected_revision);
                            }
                        }
                    }
                    if (!_compare_to.empty())
                    {
                        if (_snapshot->working_copy.empty() || _selected_revision == _snapshot->working_copy)
                        {
                            _compare_to.clear();
                            _file_comparison = false;
                        }
                        else
                            _compare_to = _snapshot->working_copy;
                    }
                    const bool selection_changed = !had_snapshot || old_selection != _selected_revision;
                    if (repository_changed || selection_changed || old_compare_to != _compare_to)
                        RequestDiff(true);
                }
                else if constexpr (std::is_same_v<T, DiffReady>)
                {
                    if (value.diff.revision != _selected_revision || value.diff.compare_to != _compare_to
                        || value.diff.file_comparison != _file_comparison
                        || value.diff.options.whitespace_mode != _diff_whitespace_mode
                        || value.diff.options.context_lines != _diff_context_lines)
                        return;
                    if (value.diff.revision == _pending_revision || _selected_file.empty())
                    {
                        _selected_file = value.diff.path;
                        if (_preferred_file.empty())
                            _preferred_file = _selected_file;
                        _pending_revision.clear();
                        _diff = std::move(value.diff);
                        _diff_loading = false;
                    }
                    else if (value.diff.path == _selected_file)
                    {
                        _diff = std::move(value.diff);
                        _diff_loading = false;
                    }
                }
                else if constexpr (std::is_same_v<T, OperationStarted>)
                {
                    _active_operation = value.name;
                    _error_message.clear();
                    _status_message.clear();
                    _progress_phase.clear();
                    _progress_completed = 0;
                    _progress_total = 0;
                    if (value.name == "open" || value.name == "init" || value.name == "clone")
                    {
                        _snapshot.reset();
                        SDL_SetWindowTitle(_window, "Opening repository - ggui");
                    }
                }
                else if constexpr (std::is_same_v<T, OperationProgress>)
                {
                    _progress_phase = value.phase;
                    _progress_completed = value.completed;
                    _progress_total = value.total;
                }
                else if constexpr (std::is_same_v<T, OperationFinished>)
                {
                    if (value.name == "close")
                        ResetRepositoryState();
                    else
                        _status_message = value.name + " completed";
                    _active_operation.clear();
                    _progress_phase.clear();
                }
                else if constexpr (std::is_same_v<T, ErrorEvent>)
                {
                    _error_message = value.message;
                    _active_operation.clear();
                    _progress_phase.clear();
                    if (value.operation == "diff")
                        _diff_loading = false;
                }
                else if constexpr (std::is_same_v<T, CredentialRequest>)
                {
                    _credential_request = std::move(value);
                    OpenDialog(Dialog::Credentials);
                }
            },
            event);
}

void Application::RenderFrame()
{
    RenderMenuBar();
    ImGuiIO& io = ImGui::GetIO();
    if (_dialog == Dialog::None)
    {
        if (_active_operation.empty() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) PickAndOpen(false);
        if (_snapshot != nullptr && _active_operation.empty() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_W))
            _engine.Enqueue(CloseRepository{});
        if (CanCreateChange() && _active_operation.empty() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N))
            CreateChange();
    }
    if (!io.WantTextInput)
    {
        if (_snapshot != nullptr && _snapshot->can_undo && _active_operation.empty() && io.KeyCtrl
            && ImGui::IsKeyPressed(ImGuiKey_Z))
            _engine.Enqueue(Undo{});
        if (_snapshot != nullptr && _snapshot->can_redo && _active_operation.empty() && io.KeyCtrl
            && ImGui::IsKeyPressed(ImGuiKey_Y))
            _engine.Enqueue(Redo{});
        if (_snapshot != nullptr && _active_operation.empty() && ImGui::IsKeyPressed(ImGuiKey_F5))
            _engine.Enqueue(Refresh{});
        if (_snapshot != nullptr && _dialog == Dialog::None && ImGui::IsKeyPressed(ImGuiKey_F6))
            NavigateChangedFile(io.KeyShift ? -1 : 1);
        const bool plain_key = !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper;
        if (_snapshot != nullptr && _dialog == Dialog::None && _active_operation.empty() && plain_key
            && !_selected_revision.empty())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_E))
                _engine.Enqueue(Edit{_selected_revision});
            if (CanCreateChange() && ImGui::IsKeyPressed(ImGuiKey_N))
                CreateChange();
            if (ImGui::IsKeyPressed(ImGuiKey_A))
                RequestAbandon(_selected_revision);
            if (ImGui::IsKeyPressed(ImGuiKey_S))
                OpenDialog(Dialog::Split);
        }
    }
    if (_snapshot == nullptr)
        RenderWelcome();
    else
    {
        SetupDockspace();
        if (_show_bookmarks) RenderBookmarks();
        if (_show_tags) RenderTags();
        if (_show_workspaces) RenderWorkspaces();
        if (_show_remotes) RenderRemotes();
        if (_show_history) RenderHistory();
        if (_show_changes) RenderChanges();
        if (_show_change_info) RenderChangeInformation();
        if (_show_diff) RenderDiff();
        if (_show_operations) RenderOperations();
    }
    RenderDialogs();
}

void Application::SetupDockspace()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("ggui dockspace", nullptr, flags);
    ImGui::PopStyleVar(3);
    RenderToolbar();
    const ImGuiID dockspace = ImGui::GetID("ggui main dockspace");
    ImGui::DockSpace(dockspace, {}, ImGuiDockNodeFlags_PassthruCentralNode);
    if (_default_layout && _snapshot != nullptr)
    {
        ImGui::DockBuilderRemoveNode(dockspace);
        ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);
        ImGuiID references = 0;
        ImGuiID content = 0;
        ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.23f, &references, &content);
        ImGuiID reference_top = 0;
        ImGuiID reference_bottom = 0;
        ImGui::DockBuilderSplitNode(references, ImGuiDir_Up, 0.5f, &reference_top, &reference_bottom);
        ImGuiID details = 0;
        ImGuiID history = 0;
        ImGui::DockBuilderSplitNode(content, ImGuiDir_Right, 0.36f, &details, &history);
        ImGuiID changes = 0;
        ImGuiID diff = 0;
        ImGui::DockBuilderSplitNode(details, ImGuiDir_Up, 0.48f, &changes, &diff);
        ImGuiID change_list = 0;
        ImGuiID change_information = 0;
        ImGui::DockBuilderSplitNode(changes, ImGuiDir_Down, 0.40f, &change_information, &change_list);
        ImGui::DockBuilderDockWindow("Bookmarks", reference_top);
        ImGui::DockBuilderDockWindow("Tags", reference_top);
        ImGui::DockBuilderDockWindow("Workspaces", reference_bottom);
        ImGui::DockBuilderDockWindow("Remotes", reference_bottom);
        ImGui::DockBuilderDockWindow("History", history);
        ImGui::DockBuilderDockWindow("Changes", change_list);
        ImGui::DockBuilderDockWindow("Change information", change_information);
        ImGui::DockBuilderDockWindow("Diff", diff);
        ImGui::DockBuilderDockWindow("Operations", diff);
        ImGui::DockBuilderFinish(dockspace);
        _default_layout = false;
    }
    ImGui::End();
}

void Application::RenderMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
        return; // GCOV_EXCL_LINE: defensive ImGui frame rejection
    const bool actions_locked = !_active_operation.empty();
    if (ImGui::BeginMenu("Repository"))
    {
        ImGui::BeginDisabled(actions_locked);
        if (ActionMenuItem(ICON_MS_FOLDER, "Open...", "Ctrl+O"))
            PickAndOpen(false); // GCOV_EXCL_LINE: native folder picker integration
        if (ActionMenuItem(ICON_MS_CREATE_NEW_FOLDER, "Initialize..."))
            PickAndOpen(true); // GCOV_EXCL_LINE: native folder picker integration
        if (ActionMenuItem(ICON_MS_CLOUD_DOWNLOAD, "Clone..."))
            OpenDialog(Dialog::Clone);
        if (!_recent_repositories.empty() && ImGui::BeginMenu("Recent"))
        {
            for (const std::string& path : _recent_repositories)
                if (ActionMenuItem(ICON_MS_FOLDER, path))
                    _engine.Enqueue(OpenRepository{path});
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (_snapshot != nullptr)
        {
            if (ActionMenuItem(ICON_MS_FOLDER_OPEN, "Open working directory"))
                OpenExternalPath(_snapshot->root, "Working directory");
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy path"))
                ImGui::SetClipboardText(_snapshot->root.c_str());
            if (ActionMenuItem(ICON_MS_CLOSE, "Close repository", "Ctrl+W"))
                _engine.Enqueue(CloseRepository{});
            ImGui::Separator();
        }
        if (ActionMenuItem(ICON_MS_REFRESH, "Refresh", "F5", _snapshot != nullptr))
            _engine.Enqueue(Refresh{});
        ImGui::EndDisabled();
        if (ActionMenuItem(ICON_MS_CLOSE, "Quit"))
            _running = false; // GCOV_EXCL_LINE: terminating the host aborts an in-process test queue
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Change", _snapshot != nullptr && !actions_locked))
    {
        if (ActionMenuItem(ICON_MS_ADD, "New change", "Ctrl+N", CanCreateChange() && _active_operation.empty()))
            CreateChange();
        if (ActionMenuItem(ICON_MS_COMMIT, "Commit...", nullptr, _compare_to.empty())) OpenDialog(Dialog::Commit);
        if (ActionMenuItem(ICON_MS_INFO, "Metaedit...", nullptr, !_selected_revision.empty())) OpenDialog(Dialog::Metaedit);
        if (ActionMenuItem(ICON_MS_EDIT, "Edit", nullptr, !_selected_revision.empty()))
            _engine.Enqueue(Edit{_selected_revision});
        if (ActionMenuItem(ICON_MS_ARROW_DOWNWARD, "Move working copy to previous")) _engine.Enqueue(MoveChange{GG_MOVE_PREVIOUS});
        if (ActionMenuItem(ICON_MS_ARROW_UPWARD, "Move working copy to next")) _engine.Enqueue(MoveChange{GG_MOVE_NEXT});
        if (ActionMenuItem(ICON_MS_REBASE, "Rebase...", nullptr, !_selected_revision.empty())) OpenDialog(Dialog::Rebase);
        if (ActionMenuItem(ICON_MS_MERGE, "Squash...", nullptr, !_selected_revision.empty())) OpenDialog(Dialog::Squash);
        if (ActionMenuItem(ICON_MS_DIFFERENCE, "Split...", nullptr, !_selected_revision.empty())) OpenDialog(Dialog::Split);
        if (ActionMenuItem(ICON_MS_RESTORE, "Restore...", nullptr,
                !_selected_revision.empty() && _compare_to.empty()))
            OpenDialog(Dialog::Restore);
        if (ActionMenuItem(ICON_MS_DELETE, "Abandon...", nullptr, !_selected_revision.empty())) RequestAbandon(_selected_revision);
        if (ActionMenuItem(ICON_MS_FORMAT_LIST_BULLETED, "Simplify parents", nullptr, !_selected_revision.empty()))
            QueueCommands({SimplifyParents{{_selected_revision}}}, {_selected_revision},
                "Simplifying the parents will rewrite a locked commit.");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit", _snapshot != nullptr && !actions_locked))
    {
        if (ActionMenuItem(ICON_MS_UNDO, "Undo", "Ctrl+Z", _snapshot->can_undo && _active_operation.empty()))
            _engine.Enqueue(Undo{});
        if (ActionMenuItem(ICON_MS_REDO, "Redo", "Ctrl+Y", _snapshot->can_redo && _active_operation.empty()))
            _engine.Enqueue(Redo{});
        ImGui::Separator();
        if (ActionMenuItem(ICON_MS_UPLOAD_FILE, "Apply patch...", nullptr, _compare_to.empty()))
            _open_apply_patch = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        if (_snapshot != nullptr)
        {
            ImGui::MenuItem("Bookmarks", nullptr, &_show_bookmarks);
            ImGui::MenuItem("Tags", nullptr, &_show_tags);
            ImGui::MenuItem("Workspaces", nullptr, &_show_workspaces);
            ImGui::MenuItem("Remotes", nullptr, &_show_remotes);
            ImGui::Separator();
            ImGui::MenuItem("History", nullptr, &_show_history);
            ImGui::MenuItem("Changes", nullptr, &_show_changes);
            ImGui::MenuItem("Change information", nullptr, &_show_change_info);
            ImGui::MenuItem("Diff", nullptr, &_show_diff);
            ImGui::MenuItem("Operations", nullptr, &_show_operations);
            ImGui::Separator();
            if (ActionMenuItem(ICON_MS_ARROW_UPWARD, "Previous changed file", "Shift+F6",
                    CanNavigateChangedFile(-1)))
                NavigateChangedFile(-1);
            if (ActionMenuItem(ICON_MS_ARROW_DOWNWARD, "Next changed file", "F6",
                    CanNavigateChangedFile(1)))
                NavigateChangedFile(1);
            ImGui::Separator();
        }
        if (ActionMenuItem(ICON_MS_HOME, "Reset layout"))
        {
            _default_layout = true;
            _show_bookmarks = _show_tags = _show_workspaces = _show_remotes = true;
            _show_history = _show_changes = _show_change_info = _show_diff = true;
            _show_operations = false;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Application::RenderToolbar()
{
    ImGui::SetCursorPos(ImVec2(10.0f, 8.0f));
    ImGui::BeginDisabled(!_active_operation.empty());
    ImGui::BeginDisabled(!CanCreateChange());
    if (ActionButton(ICON_MS_ADD, "New")) CreateChange();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(
            "Create and edit an empty change on the selected parent(s). An already-empty current change is refreshed.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!_compare_to.empty());
    if (ActionButton(ICON_MS_COMMIT, "Commit")) OpenDialog(Dialog::Commit);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.122f, 0.161f, 0.216f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.176f, 0.235f, 0.314f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.208f, 0.278f, 0.369f, 1.0f));
    if (ActionButton(ICON_MS_ARROW_DOWNWARD, "Prev")) _engine.Enqueue(MoveChange{GG_MOVE_PREVIOUS});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Move the working copy to its parent. This modifies the repository and can be undone.");
    ImGui::SameLine();
    if (ActionButton(ICON_MS_ARROW_UPWARD, "Next")) _engine.Enqueue(MoveChange{GG_MOVE_NEXT});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Move the working copy to its child. This modifies the repository and can be undone.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!_snapshot->can_undo);
    if (ActionButton(ICON_MS_UNDO, "Undo")) _engine.Enqueue(Undo{});
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!_snapshot->can_redo);
    if (ActionButton(ICON_MS_REDO, "Redo")) _engine.Enqueue(Redo{});
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ActionButton(ICON_MS_REFRESH, "Refresh")) _engine.Enqueue(Refresh{});
    const Remote* remote = DefaultRemote(*_snapshot);
    const NamedRef* bookmark = BookmarkAt(*_snapshot, _selected_revision);
    const std::string push_remote = bookmark == nullptr ? "" : RemoteForBookmark(*_snapshot, bookmark->name);
    ImGui::SameLine();
    ImGui::BeginDisabled(remote == nullptr);
    if (ActionButton(ICON_MS_CLOUD_DOWNLOAD, "Pull")) _engine.Enqueue(Fetch{remote->name, true});
    ImGui::SameLine();
    if (ActionButton(ICON_MS_SYNC, "Fetch")) _engine.Enqueue(Fetch{remote->name, false});
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(bookmark == nullptr || push_remote.empty());
    if (ActionButton(ICON_MS_CLOUD_UPLOAD, "Push")) _engine.Enqueue(Push{bookmark->name, push_remote});
    ImGui::SameLine();
    if (ActionButton(ICON_MS_PUBLISH, "Push to..."))
    {
        OpenDialog(Dialog::PushTo);
        _input_primary = push_remote;
        _input_secondary = bookmark->name;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("REPOSITORY");
    ImGui::SameLine();
    const std::string repository_name = RepositoryName(_snapshot->root);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        _dark_theme ? ImVec4(0.18f, 0.24f, 0.32f, 1.0f) : ImVec4(0.80f, 0.86f, 0.94f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
        _dark_theme ? ImVec4(0.21f, 0.28f, 0.37f, 1.0f) : ImVec4(0.74f, 0.82f, 0.92f, 1.0f));
    if (ActionButton(ICON_MS_FOLDER, repository_name))
        OpenExternalPath(_snapshot->root, "Repository directory"); // GCOV_EXCL_LINE: external application handoff
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nClick to open the repository directory.", _snapshot->root.c_str());
    if (!_snapshot->working_copy.empty())
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.78f, 0.42f, 1.0f));
        ImGui::BeginGroup();
        TextLabelledId("@ ", _snapshot->working_copy, RevisionPrefix(_snapshot->working_copy), CommitIdColor(true));
        ImGui::EndGroup();
        ImGui::PopStyleColor();
        if (ImGui::BeginPopupContextItem("working copy ID context"))
        {
            IdCopyMenuItems("commit ID", _snapshot->working_copy, RevisionPrefix(_snapshot->working_copy));
            ImGui::EndPopup();
        }
    }
    if (!_active_operation.empty())
    {
        ImGui::SameLine();
        if (_progress_phase.empty())
            ImGui::Text("Working: %s", _active_operation.c_str());
        else if (_progress_total == 0)
            ImGui::Text("Working: %s (%s)", _active_operation.c_str(), _progress_phase.c_str());
        else
            ImGui::Text("Working: %s (%s %zu/%zu)", _active_operation.c_str(), _progress_phase.c_str(),
                _progress_completed, _progress_total);
        ImGui::SameLine();
        if (ActionButton(ICON_MS_CLOSE, "Cancel")) _engine.Cancel();
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    if (!_error_message.empty())
    {
        ImGui::SetCursorPosX(10.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
            _dark_theme ? ImVec4(0.22f, 0.07f, 0.08f, 1.0f) : ImVec4(1.0f, 0.88f, 0.88f, 1.0f));
        ImGui::BeginChild("error banner", ImVec2(0.0f, 48.0f), ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar);
        if (ImGui::BeginTable("error banner contents", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("message", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("Error: %s", _error_message.c_str());
            ImGui::TableNextColumn();
            if (ActionSmallButton(ICON_MS_CLOSE, "Dismiss error")) _error_message.clear();
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

void Application::RenderWelcome()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("Welcome", nullptr, flags);
    ImGui::PopStyleVar(2);
    const float width = 460.0f;
    ImGui::SetCursorPosX(std::max(20.0f, (ImGui::GetContentRegionAvail().x - width) * 0.5f));
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0.0f, 70.0f));
    ImGui::SetWindowFontScale(2.0f);
    ImGui::TextColored(ImVec4(0.32f, 0.64f, 1.0f, 1.0f), "ggui");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("A graph-first workspace for the gg workflow");
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::BeginDisabled(!_active_operation.empty());
    if (ImGui::Button("Open repository...", ImVec2(width, 42.0f))) PickAndOpen(false);
    if (ImGui::Button("Initialize repository...", ImVec2(width, 42.0f))) PickAndOpen(true);
    if (ImGui::Button("Clone repository...", ImVec2(width, 42.0f))) OpenDialog(Dialog::Clone);
    ImGui::EndDisabled();
    if (!_active_operation.empty())
    {
        const bool opening = _active_operation == "open" || _active_operation == "init"
            || _active_operation == "clone";
        if (opening)
            ImGui::TextDisabled("Opening repository and importing Git history...");
        else
            ImGui::TextDisabled("Working: %s", _active_operation.c_str());
        if (!_progress_phase.empty())
            ImGui::TextDisabled("%s", _progress_phase.c_str());
        if (_progress_total != 0)
            ImGui::ProgressBar(static_cast<float>(_progress_completed) / _progress_total, ImVec2(width, 0.0f));
        if (ActionButton(ICON_MS_CLOSE, "Cancel", ImVec2(width, 0.0f))) _engine.Cancel();
    }
    if (!_error_message.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.35f, 1.0f), "%s", _error_message.c_str());
    if (!_recent_repositories.empty())
    {
        ImGui::SeparatorText("Recent repositories");
        ImGui::BeginDisabled(!_active_operation.empty());
        for (const std::string& path : _recent_repositories)
            if (ImGui::Selectable(path.c_str(), false, 0, ImVec2(width, 34.0f)))
                _engine.Enqueue(OpenRepository{path});
        ImGui::EndDisabled();
    }
    ImGui::EndGroup();
    ImGui::End();
}

void Application::RenderBookmarks()
{
    if (!ImGui::Begin("Bookmarks", &_show_bookmarks))
    {
        ImGui::End();
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    const bool actions_locked = !_active_operation.empty();
    ImGui::BeginDisabled(actions_locked);
    if (ImGui::Button("Create bookmark", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::Bookmark);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##bookmark filter", "Filter bookmarks", &_bookmark_filter);
    std::vector<std::string> names;
    for (const NamedRef& ref : _snapshot->refs)
    {
        if ((ref.kind != GG_NAMED_REF_LOCAL_BOOKMARK && ref.kind != GG_NAMED_REF_REMOTE_BOOKMARK)
            || std::ranges::find(names, ref.name) != names.end())
            continue;
        names.push_back(ref.name);
    }
    for (const std::string& name : names)
    {
        if (!ContainsInsensitive(name, _bookmark_filter))
            continue;
        const auto local = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == name;
        });
        const auto remote_ref = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.name == name;
        });
        const NamedRef& ref = local != _snapshot->refs.end() ? *local : *remote_ref;
        const std::string remotes = RefRemotes(_snapshot->refs, name, GG_NAMED_REF_REMOTE_BOOKMARK);
        ImGui::PushID(name.c_str());
        bool elided = false;
        if (BadgedSelectable(name, ref.target == _selected_revision, 36.0f,
                BookmarkBadgeColor(name, _snapshot->refs), {}, &elided))
            SelectRevision(ref.target);
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        elided |= DrawTextWithin(ImGui::GetWindowDrawList(), ImVec2(minimum.x + 12.0f, minimum.y + 21.0f),
            maximum.x - 8.0f, remotes, kTextMuted);
        if (ImGui::BeginPopupContextItem("bookmark context"))
        {
            if (ActionMenuItem(ICON_MS_VISIBILITY, "Reveal commit")) RevealRevision(ref.target);
            ImGui::Separator();
            ImGui::BeginDisabled(actions_locked);
            const auto tracked = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& candidate) {
                return candidate.kind == GG_NAMED_REF_REMOTE_BOOKMARK && candidate.name == name;
            });
            const std::string remote = tracked != _snapshot->refs.end() ? tracked->remote
                : std::ranges::any_of(_snapshot->remotes, [](const Remote& candidate) { return candidate.name == "origin"; })
                ? "origin"
                : _snapshot->remotes.empty() ? "" : _snapshot->remotes.front().name;
            const bool has_local = local != _snapshot->refs.end();
            if (ActionMenuItem(ICON_MS_CLOUD_UPLOAD, "Push", nullptr, has_local && !remote.empty()))
                _engine.Enqueue(Push{name, remote});
            if (ActionMenuItem(ICON_MS_PUBLISH, "Push to...", nullptr,
                    has_local && !_snapshot->remotes.empty()))
            {
                OpenDialog(Dialog::PushTo);
                _input_primary = remote;
                _input_secondary = name;
            }
            ImGui::Separator();
            if (ActionMenuItem(ICON_MS_EDIT, "Rename...", nullptr, has_local))
            {
                OpenDialog(Dialog::BookmarkRename);
                _input_primary = name;
                _input_secondary = name;
            }
            const std::string delete_label = IconLabel(ICON_MS_DELETE, "Delete");
            if (ImGui::BeginMenu(delete_label.c_str()))
            {
                if (ActionMenuItem(ICON_MS_BOOKMARK, "Local", nullptr, has_local))
                    _engine.Enqueue(Bookmark{GG_BOOKMARK_DELETE, {name}, {}, {}});
                for (const NamedRef& candidate : _snapshot->refs)
                    if (candidate.kind == GG_NAMED_REF_REMOTE_BOOKMARK && candidate.name == name
                        && !candidate.remote.empty()
                        && ActionMenuItem(ICON_MS_CLOUD, candidate.remote))
                        _engine.Enqueue(RemoteBookmarkDelete{name, candidate.remote});
                ImGui::EndMenu();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        if (hovered && elided)
        {
            ImGui::BeginTooltip();
            ImGui::Text("Bookmark: %s", name.c_str());
            ImGui::Text("Remotes: %s", remotes.empty() ? "(local only)" : remotes.c_str());
            ImGui::Text("Commit: %s", ref.target.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::End();
}

void Application::RenderTags()
{
    if (!ImGui::Begin("Tags", &_show_tags))
    {
        ImGui::End();
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    const bool actions_locked = !_active_operation.empty();
    ImGui::BeginDisabled(actions_locked);
    if (ImGui::Button("Create tag", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::Tag);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##tag filter", "Filter tags", &_tag_filter);
    std::vector<std::string> names;
    for (const NamedRef& ref : _snapshot->refs)
    {
        if ((ref.kind != GG_NAMED_REF_LOCAL_TAG && ref.kind != GG_NAMED_REF_REMOTE_TAG)
            || std::ranges::find(names, ref.name) != names.end())
            continue;
        names.push_back(ref.name);
    }
    for (const std::string& name : names)
    {
        if (!ContainsInsensitive(name, _tag_filter))
            continue;
        const auto local = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_TAG && ref.name == name;
        });
        const auto remote_ref = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_REMOTE_TAG && ref.name == name;
        });
        const NamedRef& ref = local != _snapshot->refs.end() ? *local : *remote_ref;
        const std::string remotes = RefRemotes(_snapshot->refs, name, GG_NAMED_REF_REMOTE_TAG);
        ImGui::PushID(name.c_str());
        bool elided = false;
        if (BadgedSelectable(name, ref.target == _selected_revision, 36.0f,
                RefBadgeColor(ref, _snapshot->refs), {}, &elided))
            SelectRevision(ref.target);
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        if (!elided)
        {
            const float id_x = minimum.x + 20.0f + ImGui::CalcTextSize(name.c_str()).x;
            elided = DrawHighlightedIdWithin(ImGui::GetWindowDrawList(), ImVec2(id_x, minimum.y + 3.0f),
                maximum.x - 8.0f, ref.target, RevisionPrefix(ref.target),
                CommitIdColor(ref.target == _snapshot->working_copy));
        }
        elided |= DrawTextWithin(ImGui::GetWindowDrawList(), ImVec2(minimum.x + 12.0f, minimum.y + 21.0f),
            maximum.x - 8.0f, remotes, kTextMuted);
        if (ImGui::BeginPopupContextItem("tag context"))
        {
            if (ActionMenuItem(ICON_MS_VISIBILITY, "Reveal commit")) RevealRevision(ref.target);
            const std::string copy_label = IconLabel(ICON_MS_CONTENT_COPY, "Copy");
            if (ImGui::BeginMenu(copy_label.c_str()))
            {
                IdCopyMenuItems("commit ID", ref.target, RevisionPrefix(ref.target));
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::BeginDisabled(actions_locked);
            if (ActionMenuItem(ICON_MS_DELETE, "Delete", nullptr, local != _snapshot->refs.end()))
                _engine.Enqueue(Tag{GG_TAG_DELETE, {name}, {}, false});
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        if (hovered && elided)
        {
            ImGui::BeginTooltip();
            ImGui::Text("Tag: %s", name.c_str());
            ImGui::Text("Remotes: %s", remotes.empty() ? "(local only)" : remotes.c_str());
            ImGui::Text("Commit: %s", ref.target.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::End();
}

void Application::RenderWorkspaces()
{
    if (!ImGui::Begin("Workspaces", &_show_workspaces))
    {
        ImGui::End();
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    const bool actions_locked = !_active_operation.empty();
    ImGui::BeginDisabled(actions_locked);
    if (ImGui::Button("Add workspace", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::WorkspaceAdd);
    ImGui::EndDisabled();
    for (const Workspace& workspace : _snapshot->workspaces)
    {
        ImGui::PushID(&workspace);
        const ImU32 accent = workspace.stale ? kStatusDeleted : kBadgeWorkingCopy;
        bool elided = false;
        if (BadgedSelectable(workspace.name, workspace.working_copy == _selected_revision, 40.0f,
                accent, {}, &elided))
            SelectRevision(workspace.working_copy);
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        const std::string_view location = workspace.stale ? std::string_view("Unavailable")
                                                          : std::string_view(workspace.root);
        elided |= DrawTextWithin(ImGui::GetWindowDrawList(), ImVec2(minimum.x + 12.0f, minimum.y + 23.0f),
            maximum.x - 8.0f, location, kTextMuted);
        if (ImGui::BeginPopupContextItem("workspace context"))
        {
            if (ActionMenuItem(ICON_MS_FOLDER, "Open directory", nullptr, !workspace.stale))
                OpenExternalPath(workspace.root, "Workspace directory"); // GCOV_EXCL_LINE: external application handoff
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy path"))
                ImGui::SetClipboardText(workspace.root.c_str());
            ImGui::BeginDisabled(actions_locked);
            if (ActionMenuItem(ICON_MS_DELETE, "Forget")) _engine.Enqueue(WorkspaceForget{{workspace.name}});
            if (ActionMenuItem(ICON_MS_EDIT, "Rename current...")) OpenDialog(Dialog::WorkspaceRename);
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        if (hovered && elided)
        {
            ImGui::BeginTooltip();
            ImGui::Text("Workspace: %s", workspace.name.c_str());
            ImGui::Text("Directory: %.*s", static_cast<int>(location.size()), location.data());
            ImGui::Text("Working copy: %s", workspace.working_copy.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::End();
}

void Application::RenderRemotes()
{
    if (!ImGui::Begin("Remotes", &_show_remotes))
    {
        ImGui::End();
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    const bool actions_locked = !_active_operation.empty();
    ImGui::BeginDisabled(actions_locked);
    if (ActionButton(ICON_MS_ADD, "Add remote", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::RemoteAdd);
    ImGui::EndDisabled();
    for (const Remote& remote : _snapshot->remotes)
    {
        ImGui::PushID(&remote);
        const bool separate_push = !remote.push_url.empty() && remote.push_url != remote.fetch_url;
        bool elided = false;
        BadgedSelectable(remote.name, false, separate_push ? 56.0f : 40.0f, kBadgeRemote, {}, &elided);
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        elided |= DrawTextWithin(draw, ImVec2(minimum.x + 12.0f, minimum.y + 23.0f), maximum.x - 8.0f,
            remote.fetch_url, kTextMuted);
        const std::string push_url = "Push: " + remote.push_url;
        if (separate_push)
            elided |= DrawTextWithin(draw, ImVec2(minimum.x + 12.0f, minimum.y + 39.0f), maximum.x - 8.0f,
                push_url, kTextMuted);
        if (ImGui::BeginPopupContextItem("remote context"))
        {
            ImGui::BeginDisabled(actions_locked);
            if (ActionMenuItem(ICON_MS_CLOUD_DOWNLOAD, "Pull")) _engine.Enqueue(Fetch{remote.name, true});
            if (ActionMenuItem(ICON_MS_SYNC, "Fetch")) _engine.Enqueue(Fetch{remote.name, false});
            ImGui::Separator();
            if (ActionMenuItem(ICON_MS_DELETE, "Delete remote")) _engine.Enqueue(DeleteRemote{remote.name});
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        if (hovered && elided)
        {
            ImGui::BeginTooltip();
            ImGui::Text("Remote: %s", remote.name.c_str());
            ImGui::Text("Fetch: %s", remote.fetch_url.c_str());
            ImGui::Text("Push: %s", remote.push_url.empty() ? remote.fetch_url.c_str() : remote.push_url.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::End();
}

void Application::RebuildGraph()
{
    _visible_revisions.clear();
    std::vector<GraphNode> nodes;
    for (int index = 0; index < static_cast<int>(_snapshot->revisions.size()); ++index)
    {
        const Revision& revision = _snapshot->revisions[index];
        bool matches = _graph_filter.empty() || ContainsInsensitive(revision.description, _graph_filter)
            || ContainsInsensitive(revision.change_id, _graph_filter) || ContainsInsensitive(revision.oid, _graph_filter);
        if (!matches)
        {
            matches = std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
                return ref.target == revision.oid && ContainsInsensitive(ReferenceLabel(ref), _graph_filter);
            });
        }
        if (matches)
        {
            _visible_revisions.push_back(index);
            nodes.push_back({revision.oid, revision.parents});
        }
    }
    _graph_rows = BuildGraphLayout(nodes);
    _graph_generation = _snapshot->generation;
    _built_filter = _graph_filter;
}

void Application::RebuildIdPrefixes()
{
    const auto build = [](const std::vector<std::string>& values, auto& destination) {
        destination.clear();
        const std::vector<std::size_t> lengths = UniquePrefixLengths(values);
        for (std::size_t index = 0; index < values.size(); ++index)
            destination.emplace(values[index], lengths[index]);
    };
    std::vector<std::string> revision_ids;
    std::vector<std::string> change_ids;
    revision_ids.reserve(_snapshot->revisions.size());
    change_ids.reserve(_snapshot->revisions.size());
    for (const Revision& revision : _snapshot->revisions)
    {
        revision_ids.push_back(revision.oid);
        change_ids.push_back(revision.change_id);
    }
    std::vector<std::string> operation_ids;
    operation_ids.reserve(_snapshot->operations.size());
    for (const Operation& operation : _snapshot->operations)
        operation_ids.push_back(operation.oid);
    build(revision_ids, _revision_prefixes);
    build(change_ids, _change_prefixes);
    build(operation_ids, _operation_prefixes);
}

std::size_t Application::RevisionPrefix(const std::string& oid) const
{
    const auto found = _revision_prefixes.find(oid);
    return found == _revision_prefixes.end() ? std::min<std::size_t>(1, oid.size()) : found->second;
}

std::size_t Application::ChangePrefix(const std::string& id) const
{
    const auto found = _change_prefixes.find(id);
    return found == _change_prefixes.end() ? std::min<std::size_t>(1, id.size()) : found->second;
}

std::size_t Application::OperationPrefix(const std::string& oid) const
{
    const auto found = _operation_prefixes.find(oid);
    return found == _operation_prefixes.end() ? std::min<std::size_t>(1, oid.size()) : found->second;
}

void Application::RenderHistory()
{
    if (!_reveal_revision.empty()) ImGui::SetNextWindowFocus();
    if (!ImGui::Begin("History", &_show_history))
    {
        ImGui::End();
        return;
    }
    const bool actions_locked = !_active_operation.empty();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##graph filter", "Filter changes, IDs, bookmarks, tags", &_graph_filter);
    if (_graph_generation != _snapshot->generation || _built_filter != _graph_filter)
        RebuildGraph();
    ImGui::BeginChild("graph scroll", {}, ImGuiChildFlags_Borders);
    if (!_reveal_revision.empty())
    {
        const auto target = std::ranges::find_if(_visible_revisions, [&](int index) {
            return _snapshot->revisions[index].oid == _reveal_revision;
        });
        if (target != _visible_revisions.end())
        {
            const float row = static_cast<float>(target - _visible_revisions.begin()) * kRowHeight;
            const float viewport = ImGui::GetContentRegionAvail().y;
            ImGui::SetScrollY(std::max(0.0f, row - (viewport - kRowHeight) * 0.5f));
        }
        _reveal_revision.clear();
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(_visible_revisions.size()), kRowHeight);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    while (clipper.Step())
    {
        for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible)
        {
            const Revision& revision = _snapshot->revisions[_visible_revisions[visible]];
            const GraphRow& row = _graph_rows[visible];
            ImGui::PushID(revision.oid.c_str());
            const int row_column_count = GraphColumnCount(row);
            const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            ImGui::InvisibleButton("row", ImVec2(width, kRowHeight),
                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const float center = (minimum.y + maximum.y) * 0.5f;
            const bool selected = std::ranges::find(_selected_revisions, revision.oid) != _selected_revisions.end();
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) SelectRevision(revision.oid, ImGui::GetIO().KeyCtrl);
            if (!actions_locked && ImGui::BeginDragDropSource(
                    ImGuiDragDropFlags_SourceAllowNullID | ImGuiDragDropFlags_SourceNoPreviewTooltip))
            {
                const bool choose_action = ImGui::GetCurrentContext()->ActiveIdMouseButton == ImGuiMouseButton_Right;
                ImGui::SetDragDropPayload(
                    choose_action ? "GGUI_CHANGE_ACTION" : "GGUI_CHANGE", revision.oid.c_str(), revision.oid.size() + 1);
                ImGui::EndDragDropSource();
            }
            std::optional<DropAction> hovered_drop;
            ImVec2 drop_zone_minimum{};
            ImVec2 drop_zone_maximum{};
            ImU32 drop_outline_color = 0;
            bool hovered_action_drop = false;
            if (!actions_locked && ImGui::BeginDragDropTarget())
            {
                const float ratio = (ImGui::GetMousePos().y - minimum.y) / kRowHeight;
                const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
                if (dragging != nullptr && dragging->IsDataType("GGUI_CHANGE"))
                {
                    hovered_drop = ratio < 0.2f ? DropAction::ReorderBefore
                        : ratio < 0.8f                  ? DropAction::Squash
                                                      : DropAction::Rebase;
                    const std::string tooltip = DropTooltip(*hovered_drop, revision.change_id);
                    ImGui::SetTooltip("%s", tooltip.c_str());
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE"))
                {
                    _pending_drop = {static_cast<const char*>(payload->Data), revision.oid, *hovered_drop};
                    if (_pending_drop.source != _pending_drop.target) OpenDialog(Dialog::ConfirmDrop);
                }
                hovered_action_drop = dragging != nullptr && dragging->IsDataType("GGUI_CHANGE_ACTION");
                if (hovered_action_drop)
                    ImGui::SetTooltip("Choose an action for %s", revision.change_id.c_str());
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE_ACTION"))
                {
                    _pending_drop = {static_cast<const char*>(payload->Data), revision.oid, DropAction::ReorderBefore};
                    _open_drop_actions = _pending_drop.source != _pending_drop.target;
                }
                if (_compare_to.empty())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_FILE"))
                    {
                        const char* source = static_cast<const char*>(payload->Data);
                        const char* path = source + std::char_traits<char>::length(source) + 1;
                        if (source != revision.oid && path < source + payload->DataSize && *path != '\0')
                            QueueCommands({MoveFiles{source, revision.oid, {path}}}, {source, revision.oid},
                                "Moving this file will rewrite a locked source or destination commit.");
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (!hovered_action_drop && ImGui::BeginPopupContextItem("change context"))
            {
                const std::string copy_label = IconLabel(ICON_MS_CONTENT_COPY, "Copy");
                if (ImGui::BeginMenu(copy_label.c_str()))
                {
                    IdCopyMenuItems("change ID", revision.change_id, ChangePrefix(revision.change_id));
                    IdCopyMenuItems("commit ID", revision.oid, RevisionPrefix(revision.oid));
                    if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Full description", nullptr,
                            !revision.description.empty()))
                        ImGui::SetClipboardText(revision.description.c_str());
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                ImGui::BeginDisabled(actions_locked);
                const NamedRef* bookmark = BookmarkAt(*_snapshot, revision.oid);
                const std::string remote = bookmark == nullptr ? "" : RemoteForBookmark(*_snapshot, bookmark->name);
                if (ActionMenuItem(ICON_MS_CLOUD_UPLOAD, "Push", nullptr,
                        bookmark != nullptr && !remote.empty()))
                    _engine.Enqueue(Push{bookmark->name, remote});
                if (ActionMenuItem(ICON_MS_PUBLISH, "Push to...", nullptr,
                        bookmark != nullptr && !_snapshot->remotes.empty()))
                {
                    OpenDialog(Dialog::PushTo);
                    _input_primary = remote;
                    _input_secondary = bookmark->name;
                }
                ImGui::Separator();
                if (ActionMenuItem(ICON_MS_BOOKMARK_ADD, "Create bookmark..."))
                {
                    SelectRevision(revision.oid);
                    OpenDialog(Dialog::Bookmark);
                }
                const bool can_move_bookmark = std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
                    return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.target != revision.oid;
                });
                const std::string move_bookmark = IconLabel(ICON_MS_MOVE_ITEM, "Move bookmark here");
                if (ImGui::BeginMenu(move_bookmark.c_str(), can_move_bookmark))
                {
                    for (const NamedRef& ref : _snapshot->refs)
                        if (ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.target != revision.oid
                            && ActionMenuItem(ICON_MS_BOOKMARK, ref.name))
                            _engine.Enqueue(Bookmark{GG_BOOKMARK_MOVE, {ref.name}, revision.oid, {}});
                    ImGui::EndMenu();
                }
                const std::string delete_bookmark = IconLabel(ICON_MS_DELETE, "Delete bookmark");
                if (ImGui::BeginMenu(delete_bookmark.c_str(), bookmark != nullptr))
                {
                    for (const NamedRef& ref : _snapshot->refs)
                        if (ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.target == revision.oid
                            && ActionMenuItem(ICON_MS_DELETE, ref.name))
                            _engine.Enqueue(Bookmark{GG_BOOKMARK_DELETE, {ref.name}, {}, {}});
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ActionMenuItem(ICON_MS_EDIT, "Edit", "E")) _engine.Enqueue(Edit{revision.oid});
                if (ActionMenuItem(ICON_MS_DIFFERENCE, "Split...", "S"))
                {
                    SelectRevision(revision.oid);
                    OpenDialog(Dialog::Split);
                }
                if (ActionMenuItem(ICON_MS_DELETE, "Abandon...", "A")) RequestAbandon(revision.oid);
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            const ImU32 row_fill = _dark_theme
                ? selected ? kRowSelected : hovered ? kRowHover : kRowBackground
                : ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive
                                             : hovered ? ImGuiCol_HeaderHovered : ImGuiCol_WindowBg);
            draw->AddRectFilled(minimum, maximum, row_fill, 6.0f);
            draw->AddRect(minimum, maximum, _dark_theme ? kRowBorder : ImGui::GetColorU32(ImGuiCol_Border), 6.0f);
            if (selected)
                draw->AddRectFilled(minimum, ImVec2(minimum.x + 4.0f, maximum.y), kBadgeBookmark, 6.0f,
                    ImDrawFlags_RoundCornersLeft);
            if (hovered_drop.has_value())
            {
                const ImU32 zone_color = *hovered_drop == DropAction::ReorderBefore ? IM_COL32(90, 150, 255, 80)
                    : *hovered_drop == DropAction::Squash                            ? IM_COL32(220, 170, 70, 80)
                                                                                     : IM_COL32(110, 210, 145, 80);
                drop_outline_color = *hovered_drop == DropAction::ReorderBefore ? IM_COL32(90, 150, 255, 230)
                    : *hovered_drop == DropAction::Squash                        ? IM_COL32(220, 170, 70, 230)
                                                                                 : IM_COL32(110, 210, 145, 230);
                const float zone_top = *hovered_drop == DropAction::ReorderBefore ? minimum.y
                    : *hovered_drop == DropAction::Squash                          ? minimum.y + kRowHeight * 0.2f
                                                                                  : minimum.y + kRowHeight * 0.8f;
                const float zone_bottom = *hovered_drop == DropAction::ReorderBefore ? minimum.y + kRowHeight * 0.2f
                    : *hovered_drop == DropAction::Squash                             ? minimum.y + kRowHeight * 0.8f
                                                                                     : maximum.y;
                drop_zone_minimum = ImVec2(minimum.x, zone_top);
                drop_zone_maximum = ImVec2(maximum.x, zone_bottom);
                draw->AddRectFilled(drop_zone_minimum, drop_zone_maximum, zone_color);
            }
            const float graph_width = row_column_count * kLaneWidth + kGraphPadding * 2.0f;
            draw->AddRectFilled(minimum, ImVec2(minimum.x + graph_width, maximum.y),
                _dark_theme ? kGraphBackground : IM_COL32(229, 233, 239, 255), 6.0f, ImDrawFlags_RoundCornersLeft);
            if (hovered_action_drop)
                draw->AddRectFilled(minimum, maximum, IM_COL32(90, 150, 255, 80), 6.0f);
            const float graph_left = minimum.x + kGraphPadding;
            auto lane_x = [&](int column) { return graph_left + column * kLaneWidth + kLaneWidth * 0.5f; };
            auto color = [](int track) { return kLaneColors[static_cast<std::size_t>(track) % kLaneColors.size()]; };

            std::vector<int> shifted_before(row.tracks_before.size(), -1);
            std::vector<int> shifted_after(row.tracks_after.size(), -1);
            for (std::size_t before = 0; before < row.tracks_before.size(); ++before)
            {
                const auto after = std::find(row.tracks_after.begin(), row.tracks_after.end(), row.tracks_before[before]);
                if (after != row.tracks_after.end() && static_cast<std::size_t>(after - row.tracks_after.begin()) != before)
                {
                    shifted_before[before] = static_cast<int>(after - row.tracks_after.begin());
                    shifted_after[shifted_before[before]] = static_cast<int>(before);
                }
            }
            for (std::size_t column = 0; column < row.tracks_before.size(); ++column)
                draw->AddLine(ImVec2(lane_x(static_cast<int>(column)), minimum.y),
                    ImVec2(lane_x(static_cast<int>(column)), shifted_before[column] < 0 ? center : center - 7.0f),
                    color(row.tracks_before[column]), 2.0f);
            for (std::size_t column = 0; column < row.tracks_after.size(); ++column)
            {
                const bool new_non_current_lane = static_cast<int>(column) != row.column && shifted_after[column] < 0
                    && std::ranges::find(row.tracks_before, row.tracks_after[column]) == row.tracks_before.end();
                if (new_non_current_lane)
                    continue;
                draw->AddLine(ImVec2(lane_x(static_cast<int>(column)), shifted_after[column] < 0 ? center : center + 7.0f),
                    ImVec2(lane_x(static_cast<int>(column)), maximum.y), color(row.tracks_after[column]), 2.0f);
            }
            for (std::size_t before = 0; before < shifted_before.size(); ++before)
            {
                if (shifted_before[before] < 0) continue;
                draw->AddBezierCubic(ImVec2(lane_x(static_cast<int>(before)), center - 7.0f),
                    ImVec2(lane_x(static_cast<int>(before)), center), ImVec2(lane_x(shifted_before[before]), center),
                    ImVec2(lane_x(shifted_before[before]), center + 7.0f), color(row.tracks_before[before]), 2.0f);
            }
            const float dot_x = lane_x(row.column);
            for (int parent_column : row.parent_columns)
            {
                if (parent_column == row.column || parent_column >= static_cast<int>(row.tracks_after.size())) continue;
                const float parent_x = lane_x(parent_column);
                draw->AddBezierCubic(ImVec2(dot_x, center + kDotRadius), ImVec2(dot_x, center + 12.0f),
                    ImVec2(parent_x, maximum.y - 10.0f), ImVec2(parent_x, maximum.y),
                    color(row.tracks_after[parent_column]), 2.0f);
            }
            if (row.continues_beyond_layout)
                draw->AddLine(ImVec2(dot_x, center), ImVec2(dot_x, maximum.y), color(row.track), 2.0f);
            if (selected)
                draw->AddCircle(ImVec2(dot_x, center), kDotRadius + 3.0f, IM_COL32(47, 129, 247, 150), 0, 2.0f);
            draw->AddCircleFilled(ImVec2(dot_x, center), kDotRadius,
                revision.conflicted ? kStatusConflict
                    : revision.working_copy ? kStatusAdded
                    : revision.pushed       ? kStatusPushed
                                            : kStatusUnpushed);
            draw->AddCircle(ImVec2(dot_x, center), kDotRadius, IM_COL32(17, 24, 39, 255), 0, 1.25f);
            const float content_x = std::min(minimum.x + graph_width + 12.0f, maximum.x - 8.0f);
            const float content_right = maximum.x - 8.0f;
            ImGui::PushClipRect(ImVec2(content_x, minimum.y), ImVec2(content_right, maximum.y), true);
            const std::string description = FirstLine(revision.description);
            const std::string_view title = description.empty() ? "(no description)" : std::string_view(description);
            ImVec2 content_cursor(content_x, center - ImGui::GetTextLineHeight() * 0.5f);
            bool elided = false;
            const auto draw_text = [&](std::string_view text, ImU32 color, float gap) {
                content_cursor.x += gap;
                const float text_width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
                if (content_cursor.x + text_width > content_right)
                {
                    DrawElidedText(draw, content_cursor, content_right, text, color);
                    elided = true;
                    return false;
                }
                draw->AddText(content_cursor, color, text.data(), text.data() + text.size());
                content_cursor.x += text_width;
                return true;
            };
            draw_text(title, ImGui::GetColorU32(ImGuiCol_Text), 0.0f);
            const auto draw_id = [&](const std::string& id, std::size_t prefix, ImU32 color) {
                if (elided)
                    return;
                content_cursor.x += 16.0f;
                const std::size_t shown = std::min(id.size(), std::max<std::size_t>(8, prefix));
                const std::string_view visible(id.data(), shown);
                if (content_cursor.x + ImGui::CalcTextSize(visible.data(), visible.data() + visible.size()).x
                    > content_right)
                {
                    DrawElidedText(draw, content_cursor, content_right, visible, color);
                    elided = true;
                    return;
                }
                content_cursor.x = DrawHighlightedId(draw, content_cursor, id, prefix, color);
            };
            draw_id(revision.change_id, ChangePrefix(revision.change_id), ChangeIdColor(revision.working_copy));
            draw_id(revision.oid, RevisionPrefix(revision.oid), CommitIdColor(revision.working_copy));
            if (!elided)
                draw_text(revision.author, kTextMuted, 16.0f);
            ImVec2 badge_cursor(content_cursor.x + 12.0f, center);
            std::vector<std::string> drawn_refs;
            for (const NamedRef& ref : _snapshot->refs)
            {
                if (elided)
                    break;
                if (ref.target != revision.oid) continue;
                const bool bookmark = ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK
                    || ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK;
                const auto [label, dimmed_prefix] = ReferenceBadgeLabel(ref, _snapshot->refs);
                if (label.empty())
                    continue;
                const std::string key = (bookmark ? "bookmark:" : "tag:") + label;
                if (std::ranges::find(drawn_refs, key) != drawn_refs.end())
                    continue;
                drawn_refs.push_back(key);
                const float badge_width = ImGui::CalcTextSize(label.c_str()).x + FontPx(14.0f);
                if (badge_cursor.x + badge_width > content_right)
                {
                    DrawElidedBadge(draw, badge_cursor, center, content_right, label,
                        RefBadgeColor(ref, _snapshot->refs), dimmed_prefix);
                    elided = true;
                    break;
                }
                DrawBadge(draw, badge_cursor, center, label, RefBadgeColor(ref, _snapshot->refs), dimmed_prefix);
            }
            ImGui::PopClipRect();

            if (hovered && elided)
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520.0f);
                const std::string message = LimitedLines(revision.description, 16);
                ImGui::TextUnformatted(message.empty() ? "(no description)" : message.c_str());
                ImGui::Separator();
                ImGui::Text("Author: %s", revision.author.empty() ? "(unknown)" : revision.author.c_str());
                ImGui::Text("Change: %s", revision.change_id.c_str());
                ImGui::Text("Commit: %s", revision.oid.c_str());
                if (!revision.parents.empty())
                {
                    std::string parents;
                    for (const std::string& parent : revision.parents)
                    {
                        if (!parents.empty()) parents += ", ";
                        parents += parent;
                    }
                    ImGui::TextWrapped("Parents: %s", parents.c_str());
                }
                std::string refs;
                for (const NamedRef& ref : _snapshot->refs)
                {
                    if (ref.target != revision.oid) continue;
                    if (!refs.empty()) refs += ", ";
                    refs += ref.remote.empty() ? ref.name : ref.remote + "/" + ref.name;
                }
                if (!refs.empty()) ImGui::TextWrapped("Refs: %s", refs.c_str());
                ImGui::TextUnformatted(revision.pushed ? "Locked (pushed)" : "Not pushed");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            if (hovered_drop.has_value())
                draw->AddRect(drop_zone_minimum + ImVec2(1.0f, 1.0f), drop_zone_maximum - ImVec2(1.0f, 1.0f),
                    drop_outline_color, 3.0f, ImDrawFlags_None, 2.0f);
            else if (hovered_action_drop)
                draw->AddRect(ImVec2(minimum.x + 1.0f, minimum.y + 1.0f),
                    ImVec2(maximum.x - 1.0f, maximum.y - 1.0f), IM_COL32(90, 150, 255, 230), 5.0f,
                    ImDrawFlags_None, 2.0f);
            ImGui::SetCursorScreenPos(ImVec2(minimum.x, maximum.y));
            ImGui::PopID();
        }
    }
    ImGui::InvisibleButton("move to end", ImVec2(-1.0f, 22.0f));
    const ImVec2 end_minimum = ImGui::GetItemRectMin();
    const ImVec2 end_maximum = ImGui::GetItemRectMax();
    bool hovered_end_drop = false;
    if (!actions_locked && !_visible_revisions.empty() && ImGui::BeginDragDropTarget())
    {
        ImGui::TextUnformatted("Move after final change");
        const Revision& final = _snapshot->revisions[_visible_revisions.back()];
        const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
        hovered_end_drop = dragging != nullptr
            && (dragging->IsDataType("GGUI_CHANGE") || dragging->IsDataType("GGUI_CHANGE_ACTION"));
        if (dragging != nullptr && dragging->IsDataType("GGUI_CHANGE"))
        {
            const std::string tooltip = DropTooltip(DropAction::ReorderAfter, final.change_id);
            ImGui::SetTooltip("%s", tooltip.c_str());
        }
        else if (dragging != nullptr && dragging->IsDataType("GGUI_CHANGE_ACTION"))
            ImGui::SetTooltip("Choose an action after %s", final.change_id.c_str());
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE"))
        {
            _pending_drop = {static_cast<const char*>(payload->Data), final.oid, DropAction::ReorderAfter};
            if (_pending_drop.source != _pending_drop.target) OpenDialog(Dialog::ConfirmDrop);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE_ACTION"))
        {
            _pending_drop = {static_cast<const char*>(payload->Data), final.oid, DropAction::ReorderAfter};
            _open_drop_actions = _pending_drop.source != _pending_drop.target;
        }
        ImGui::EndDragDropTarget();
    }
    if (hovered_end_drop)
        draw->AddRect(ImVec2(end_minimum.x + 1.0f, end_minimum.y + 1.0f),
            ImVec2(end_maximum.x - 1.0f, end_maximum.y - 1.0f), IM_COL32(90, 150, 255, 230), 5.0f,
            ImDrawFlags_None, 2.0f);
    ImGui::EndChild();
    if (_open_drop_actions)
    {
        ImGui::OpenPopup("Drop action");
        _open_drop_actions = false;
    }
    if (ImGui::BeginPopup("Drop action"))
    {
        ImGui::BeginDisabled(actions_locked);
        std::optional<DropAction> action;
        if (ActionMenuItem(ICON_MS_ARROW_DOWNWARD, "Move before")) action = DropAction::ReorderBefore;
        if (ActionMenuItem(ICON_MS_ARROW_UPWARD, "Move after")) action = DropAction::ReorderAfter;
        if (ActionMenuItem(ICON_MS_MERGE, "Squash")) action = DropAction::Squash;
        if (ActionMenuItem(ICON_MS_REBASE, "Rebase")) action = DropAction::Rebase;
        if (action.has_value())
        {
            _pending_drop.action = *action;
            OpenDialog(Dialog::ConfirmDrop);
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::End();
}

void Application::RenderChanges()
{
    if (!ImGui::Begin("Changes", &_show_changes))
    {
        ImGui::End();
        return;
    }
    const bool actions_locked = !_active_operation.empty();
    const bool comparison_active = !_compare_to.empty();
    bool comparing = comparison_active && !_file_comparison;
    ImGui::BeginDisabled(_selected_revision.empty() || _snapshot->working_copy.empty()
        || (!comparison_active && _selected_revision == _snapshot->working_copy));
    if (ImGui::Checkbox("Compare with @", &comparing))
        ToggleComparison(false);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Compare the entire selected change with the working copy.");
    if (comparing)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s → @ %s", ShortId(_selected_revision).c_str(), ShortId(_compare_to).c_str());
    }
    ImGui::SameLine();
    const auto [parent, child] = AdjacentRevisions(_diff.revision);
    if (_diff_loading && !_pending_revision.empty())
        ImGui::TextDisabled("Loading selected change...");
    else if (_diff.files.empty())
        ImGui::TextDisabled(comparing ? "The comparison has no differences." : "Selected change is empty.");
    else
        ImGui::TextDisabled("%zu %s file%s", _diff.files.size(), comparing ? "differing" : "changed",
            _diff.files.size() == 1 ? "" : "s");
    const float navigation_width = FontPx(74.0f);
    ImGui::SetNextItemWidth(-navigation_width);
    ImGui::InputTextWithHint("##changes filter", "Filter changed files", &_changes_filter);
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanNavigateChangedFile(-1));
    if (ImGui::ArrowButton("Previous changed file", ImGuiDir_Up)) NavigateChangedFile(-1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Previous changed file (Shift+F6)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanNavigateChangedFile(1));
    if (ImGui::ArrowButton("Next changed file", ImGuiDir_Down)) NavigateChangedFile(1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Next changed file (F6)");
    ImGui::BeginChild("file list");
    const ImVec2 item_spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0f));
    for (const StatusEntry& file : _diff.files)
    {
        const std::string status = DeltaName(file.status);
        if (!FileMatchesFilter(file))
            continue;
        ImGui::PushID(&file);
        const std::string label = status + "  " + file.path;
        const std::string item_id = "###" + label;
        const ImU32 accent = StatusColor(file.conflicted ? GIT_DELTA_CONFLICTED : file.status);
        const bool selected = ImGui::Selectable(item_id.c_str(), file.path == _selected_file, 0, ImVec2(0.0f, 26.0f));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(minimum, ImVec2(minimum.x + 4.0f, maximum.y), accent, 4.0f, ImDrawFlags_RoundCornersLeft);
        ImVec2 text(minimum.x + 12.0f, minimum.y + (maximum.y - minimum.y - ImGui::GetTextLineHeight()) * 0.5f);
        draw->AddText(text, accent, status.c_str());
        text.x += ImGui::CalcTextSize(status.c_str()).x + ImGui::CalcTextSize("  ").x;
        const bool elided = DrawTextWithin(draw, text, maximum.x - 8.0f, file.path, ImGui::GetColorU32(ImGuiCol_Text));
        if (selected)
            SelectFile(file.path);
        if (!actions_locked && !comparison_active && ImGui::BeginDragDropSource())
        {
            std::string payload = _diff.revision;
            payload.push_back('\0');
            payload += file.path;
            payload.push_back('\0');
            ImGui::SetDragDropPayload("GGUI_FILE", payload.data(), payload.size());
            ImGui::Text("Move %s", file.path.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, item_spacing);
        if (ImGui::BeginPopupContextItem("file context"))
        {
            if (_selected_file != file.path)
                SelectFile(file.path);
            const std::optional<std::filesystem::path> absolute = WorkingCopyPath(_snapshot->root, file.path);
            const bool file_exists = absolute.has_value() && std::filesystem::exists(*absolute)
                && !std::filesystem::is_directory(*absolute);
            const bool folder_exists = absolute.has_value() && std::filesystem::is_directory(absolute->parent_path());
            ImGui::BeginDisabled(!file_exists);
            if (ActionMenuItem(ICON_MS_OPEN_IN_NEW, "Open working-copy file"))
                OpenExternalPath(*absolute, "File");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!folder_exists);
            if (ActionMenuItem(ICON_MS_FOLDER_OPEN, "Open containing folder"))
                OpenExternalPath(absolute->parent_path(), "Containing folder");
            ImGui::EndDisabled();
            const std::string copy_label = IconLabel(ICON_MS_CONTENT_COPY, "Copy");
            if (ImGui::BeginMenu(copy_label.c_str()))
            {
                if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Name"))
                    ImGui::SetClipboardText(std::filesystem::path(file.path).filename().string().c_str());
                if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Relative path"))
                    ImGui::SetClipboardText(file.path.c_str());
                ImGui::BeginDisabled(!absolute.has_value());
                if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Absolute path"))
                    ImGui::SetClipboardText(absolute->string().c_str());
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }
            ImGui::Separator();
            const bool patch_available = _diff.path == file.path && !_diff.patch.empty();
            ImGui::BeginDisabled(!patch_available);
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy patch"))
            {
                ImGui::SetClipboardText(_diff.patch.c_str());
                _status_message = "Patch copied to clipboard";
            }
            if (ActionMenuItem(ICON_MS_SAVE, "Save patch..."))
                _open_save_patch = true;
            ImGui::EndDisabled();
            const std::string external_diff_label = IconLabel(ICON_MS_OPEN_IN_NEW, "External diff");
            if (ImGui::BeginMenu(external_diff_label.c_str()))
            {
                ImGui::BeginDisabled(_diff.revision == _snapshot->working_copy || _snapshot->working_copy.empty());
                if (ActionMenuItem(ICON_MS_OPEN_IN_NEW, "vs @"))
                    OpenExternalDiff(file.path, _snapshot->working_copy);
                ImGui::EndDisabled();
                ImGui::BeginDisabled(parent.empty());
                if (ActionMenuItem(ICON_MS_OPEN_IN_NEW, "vs parent"))
                    OpenExternalDiff(file.path, {});
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::BeginDisabled(actions_locked || comparison_active || child.empty());
            if (ActionMenuItem(ICON_MS_ARROW_UPWARD, "Move to child"))
                QueueCommands({MoveFiles{_diff.revision, child, {file.path}}}, {_diff.revision, child},
                    "Moving this file will rewrite a locked source or destination commit.");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(actions_locked || comparison_active || parent.empty());
            if (ActionMenuItem(ICON_MS_ARROW_DOWNWARD, "Move to parent"))
                QueueCommands({MoveFiles{_diff.revision, parent, {file.path}}}, {_diff.revision, parent},
                    "Moving this file will rewrite a locked source or destination commit.");
            ImGui::EndDisabled();
            if (_selected_revision == _snapshot->working_copy)
            {
                ImGui::Separator();
                ImGui::BeginDisabled(actions_locked || comparison_active);
                if (ActionMenuItem(ICON_MS_COMMIT, "Commit only this file"))
                {
                    _selected_file = file.path;
                    OpenDialog(Dialog::Commit);
                    _input_filesets = file.path;
                }
                if (ActionMenuItem(ICON_MS_RESTORE, "Restore this file"))
                    QueueCommands({Restore{"@-", "@", {file.path}}}, {"@"},
                        "Restoring this file will rewrite the locked working-copy commit.");
                if (ActionMenuItem(ICON_MS_ADD, "Track"))
                    QueueCommands({TrackPaths{{file.path}}}, {"@"},
                        "Tracking this file will rewrite the locked working-copy commit.");
                if (ActionMenuItem(ICON_MS_DELETE, "Untrack"))
                    QueueCommands({UntrackPaths{{file.path}}}, {"@"},
                        "Untracking this file will rewrite the locked working-copy commit.");
                ImGui::EndDisabled();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        if (hovered && elided)
        {
            ImGui::BeginTooltip();
            ImGui::Text("Status: %s", status.c_str());
            ImGui::Text("Path: %s", file.path.c_str());
            if (!file.old_path.empty() && file.old_path != file.path)
                ImGui::Text("Previous path: %s", file.old_path.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    if (_selected_revision == _snapshot->working_copy && !_snapshot->conflicts.empty())
    {
        ImGui::SeparatorText("Conflicts");
        for (const Conflict& conflict : _snapshot->conflicts)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kStatusConflict);
            ImGui::TextUnformatted(conflict.path.c_str());
            ImGui::PopStyleColor();
            ImGui::TextDisabled("%zu base removals, %zu side additions", conflict.removes, conflict.adds);
            ImGui::PushID(&conflict);
            if (ImGui::SmallButton("Open externally"))
            {
                const std::string path = (std::filesystem::path(_snapshot->root) / conflict.path)
                                             .string(); // GCOV_EXCL_LINE: external application handoff
                OpenExternalPath(path, "Conflicted file"); // GCOV_EXCL_LINE: external application handoff
            }
            ImGui::PopID();
        }
        ImGui::TextWrapped("Save resolved files; ggui snapshots them automatically.");
    }
    ImGui::EndChild();
    ImGui::End();
}

void Application::RenderChangeInformation()
{
    if (!ImGui::Begin("Change information", &_show_change_info))
    {
        ImGui::End();
        return;
    }
    const auto revision = std::ranges::find(_snapshot->revisions, _selected_revision, &Revision::oid);
    if (revision == _snapshot->revisions.end())
    {
        ImGui::TextDisabled("Select a change to inspect it.");
        ImGui::End();
        return;
    }
    if (_change_info_revision != revision->oid)
    {
        _change_info_revision = revision->oid;
        _change_info_message = revision->description;
        _change_info_dirty = false;
    }

    ImGui::TextUnformatted(revision->author.empty() ? "Unknown author" : revision->author.c_str());
    ImGui::SameLine(0.0f, 12.0f);
    const std::string date = FormatTimestamp(revision->timestamp);
    ImGui::Text("%s%s", date.c_str(), revision->pushed ? "  locked" : "");
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::BeginGroup();
    TextLabelledId("Change ", revision->change_id, ChangePrefix(revision->change_id),
        ChangeIdColor(revision->working_copy));
    ImGui::EndGroup();
    if (ImGui::BeginPopupContextItem("change ID context"))
    {
        IdCopyMenuItems("change ID", revision->change_id, ChangePrefix(revision->change_id));
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::BeginGroup();
    TextLabelledId("Commit ", revision->oid, RevisionPrefix(revision->oid), CommitIdColor(revision->working_copy));
    ImGui::EndGroup();
    if (ImGui::BeginPopupContextItem("commit ID context"))
    {
        IdCopyMenuItems("commit ID", revision->oid, RevisionPrefix(revision->oid));
        ImGui::EndPopup();
    }
    const float button_height = ImGui::GetFrameHeight();
    const float message_height = std::max(46.0f, ImGui::GetContentRegionAvail().y - button_height - 12.0f);
    const ImVec2 message_size = ImGui::CalcTextSize(
        _change_info_message.data(), _change_info_message.data() + _change_info_message.size(), false);
    const ImVec2 available = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("commit message scroll", ImVec2(-1.0f, message_height), ImGuiChildFlags_None,
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar
            | ImGuiWindowFlags_AlwaysVerticalScrollbar);
    const ImVec2 editor_size(std::max(available.x - ImGui::GetStyle().ScrollbarSize,
                                 message_size.x + ImGui::GetStyle().FramePadding.x * 2.0f),
        std::max(message_height - ImGui::GetStyle().ScrollbarSize,
            message_size.y + ImGui::GetStyle().FramePadding.y * 2.0f));
    if (ImGui::InputTextMultiline("##commit message", &_change_info_message, editor_size))
        _change_info_dirty = true;
    ImGui::EndChild();
    ImGui::BeginDisabled(!_change_info_dirty || !_active_operation.empty());
    const bool save = revision->pushed ? DangerButton("Save message") : ImGui::Button("Save message");
    ImGui::EndDisabled();
    if (save)
    {
        if (revision->pushed)
        {
            _pending_change_info_save = true;
            QueueCommands({Describe{revision->oid, _change_info_message}}, {revision->oid},
                "Saving the message will rewrite this locked commit.");
        }
        else
        {
            _engine.Enqueue(Describe{revision->oid, _change_info_message});
            _change_info_dirty = false;
        }
    }
    ImGui::End();
}

void Application::RenderDiff()
{
    if (!ImGui::Begin("Diff", &_show_diff))
    {
        ImGui::End();
        return;
    }
    const auto render_comparison = [this]()
    {
        if (!_compare_to.empty())
        {
            ImGui::TextDisabled("%s → @ %s", ShortId(_selected_revision).c_str(), ShortId(_compare_to).c_str());
            ImGui::SameLine();
        }
        bool comparing = !_compare_to.empty() && _file_comparison;
        ImGui::BeginDisabled(_selected_revision.empty() || _snapshot->working_copy.empty()
            || (_compare_to.empty() && _selected_revision == _snapshot->working_copy));
        if (ImGui::Checkbox("Compare with @", &comparing))
            ToggleComparison(true);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Compare only the selected file with the working copy.");
        if (!_diff_loading && comparing && _diff.selected_status == GIT_DELTA_UNMODIFIED)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Files are identical");
        }
    };
    if (_diff_loading)
    {
        const std::array<const char*, 4> spinner{"◐", "◓", "◑", "◒"};
        const int frame = static_cast<int>(ImGui::GetTime() * 8.0) & 3;
        ImGui::Text("%s Loading...", spinner[static_cast<std::size_t>(frame)]);
        ImGui::SameLine();
        render_comparison();
        ImGui::End();
        return;
    }
    if (_diff.revision.empty())
    {
        ImGui::TextWrapped("Select a change or file to inspect its diff.");
        ImGui::SameLine();
        render_comparison();
        ImGui::End();
        return;
    }
    if (_diff.path.empty())
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Selected change is empty.");
        ImGui::SameLine();
        render_comparison();
        ImGui::End();
        return;
    }

    const git_delta_t status = _diff.selected_status;
    const bool plain = status == GIT_DELTA_ADDED || status == GIT_DELTA_UNTRACKED || status == GIT_DELTA_DELETED;

    const auto reload = [this](DiffWhitespaceMode whitespace, int context_lines)
    {
        _diff_whitespace_mode = whitespace;
        _diff_context_lines = context_lines;
        RequestDiff(false);
    };
    const auto combo_width = [](const char* longest_entry) {
        return ImGui::CalcTextSize(longest_entry).x + ImGui::GetStyle().FramePadding.x * 2.0f
            + ImGui::GetFrameHeight();
    };

    ImGui::SetNextItemWidth(combo_width("Side by Side"));
    int view_index = _diff_side_by_side ? 1 : 0;
    if (ImGui::Combo("View", &view_index, "Unified\0Side by Side\0"))
        _diff_side_by_side = view_index == 1;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(combo_width("Ignore All Whitespace"));
    int whitespace_index = WhitespaceModeIndex(_diff_whitespace_mode);
    if (ImGui::Combo("##whitespace mode", &whitespace_index, "Normal\0Ignore Whitespace\0Ignore All Whitespace\0"))
        reload(WhitespaceModeFromIndex(whitespace_index), _diff_context_lines);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Choose how whitespace-only changes are displayed.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(combo_width("25 lines"));
    int context_index = ContextLineChoiceIndex(_diff_context_lines);
    if (ImGui::BeginCombo("##context lines", ContextLineChoiceLabel(context_index)))
    {
        for (int index = 0; index < static_cast<int>(kDiffContextChoices.size()); ++index)
        {
            const bool selected = index == context_index;
            if (ImGui::Selectable(ContextLineChoiceLabel(index), selected))
                reload(_diff_whitespace_mode, kDiffContextChoices[static_cast<std::size_t>(index)]);
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Number of unchanged lines shown around each change.");

    ImGui::SameLine();
    ImGui::BeginDisabled(_diff.patch.empty());
    if (ActionButton(ICON_MS_CONTENT_COPY, "Copy Patch"))
    {
        ImGui::SetClipboardText(_diff.patch.c_str());
        _status_message = "Patch copied to clipboard";
    }
    ImGui::SameLine();
    if (ActionButton(ICON_MS_SAVE, "Save Patch..."))
        _open_save_patch = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(_snapshot == nullptr);
    if (ActionButton(ICON_MS_OPEN_IN_NEW, "External Diff"))
        OpenExternalDiff(_diff.path, _compare_to);
    ImGui::EndDisabled();
    ImGui::SameLine();
    render_comparison();
    ImGui::Separator();

    const bool mode_changed = _diff.old_mode != _diff.new_mode;
    const bool special_mode = IsSymlinkMode(_diff.old_mode) || IsSymlinkMode(_diff.new_mode)
        || IsSubmoduleMode(_diff.old_mode) || IsSubmoduleMode(_diff.new_mode);
    if (mode_changed || special_mode)
    {
        const std::string old_mode = FormatFileMode(_diff.old_mode);
        const std::string new_mode = FormatFileMode(_diff.new_mode);
        ImGui::TextDisabled("Mode: %s (%s) → %s (%s)", old_mode.c_str(), ModeKind(_diff.old_mode), new_mode.c_str(),
            ModeKind(_diff.new_mode));
    }

    if (IsSubmoduleMode(_diff.old_mode) || IsSubmoduleMode(_diff.new_mode))
    {
        ImGui::TextUnformatted("Submodule diff preview is not available.");
        ImGui::TextDisabled("Old commit: %s", _diff.old_oid.empty() ? "(none)" : _diff.old_oid.c_str());
        ImGui::TextDisabled("New commit: %s", _diff.new_oid.empty() ? "(none)" : _diff.new_oid.c_str());
        ImGui::End();
        return;
    }
    if (_diff.binary)
    {
        ImGui::TextUnformatted(
            IsImagePath(_diff.path) ? "Image diff preview is not available." : "Binary diff preview is not available.");
        ImGui::TextDisabled("Old blob: %s", _diff.old_oid.empty() ? "(none)" : _diff.old_oid.c_str());
        ImGui::TextDisabled("New blob: %s", _diff.new_oid.empty() ? "(none)" : _diff.new_oid.c_str());
        ImGui::TextWrapped("Use External Diff to open a configured image or binary diff tool.");
        ImGui::End();
        return;
    }

    static TextDiff diff;
    static TextEditor editor;
    static std::string loaded_before;
    static std::string loaded_after;
    static std::string loaded_path;
    static DiffWhitespaceMode loaded_whitespace = DiffWhitespaceMode::Normal;
    static int loaded_context_lines = 3;
    static bool loaded_plain = false;
    static bool dark_palette = !_dark_theme;
    if (loaded_before != _diff.before || loaded_after != _diff.after || loaded_path != _diff.path
        || loaded_whitespace != _diff_whitespace_mode || loaded_context_lines != _diff_context_lines
        || loaded_plain != plain)
    {
        loaded_before = _diff.before;
        loaded_after = _diff.after;
        loaded_path = _diff.path;
        loaded_whitespace = _diff_whitespace_mode;
        loaded_context_lines = _diff_context_lines;
        loaded_plain = plain;
        if (plain)
        {
            editor.SetLanguage(DiffLanguage(_diff.path));
            editor.SetText(status == GIT_DELTA_DELETED ? loaded_before : loaded_after);
            editor.SetReadOnlyEnabled(true);
        }
        else
        {
            diff.SetLanguage(DiffLanguage(_diff.path));
            diff.SetText(loaded_before, loaded_after);
        }
    }
    if (dark_palette != _dark_theme)
    {
        dark_palette = _dark_theme;
        const TextEditor::Palette& palette = _dark_theme ? TextEditor::GetDarkPalette() : TextEditor::GetLightPalette();
        diff.SetPalette(palette);
        editor.SetPalette(palette);
        diff.SetColors(_dark_theme ? IM_COL32(46, 160, 67, 55) : IM_COL32(46, 160, 67, 38),
            _dark_theme ? IM_COL32(248, 81, 73, 55) : IM_COL32(248, 81, 73, 38));
    }

    static int context_row = -1;
    static bool context_has_selection = false;
    static std::vector<DiffLine> context_line;
    static std::vector<DiffLine> context_region;
    static std::string context_revision;
    static std::string context_path;
    const auto render_move_context = [&](TextEditor& view, bool supports_selection) {
        ImGuiWindow* view_window = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
        IM_ASSERT(view_window->ChildId == ImGui::GetItemID());
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            const float line_height = std::max(view.GetLineHeight(), 1.0f);
            const float content_y = ImGui::GetItemRectMin().y + ImGui::GetStyle().WindowPadding.y;
            context_row = view.GetFirstVisibleLine()
                + static_cast<int>(std::max(ImGui::GetMousePos().y - content_y, 0.0f) / line_height);
            context_revision = _diff.revision;
            context_path = _diff.path;
            context_line.clear();
            context_region.clear();
            context_has_selection = false;
            if (context_row >= 0 && context_row < static_cast<int>(_diff.lines.size()))
            {
                const DiffLine& clicked = _diff.lines[static_cast<std::size_t>(context_row)];
                if (clicked.kind != DiffLineKind::Context)
                    context_line.push_back(clicked);

                int first = context_row;
                int last = context_row;
                if (supports_selection && view.AnyCursorHasSelection())
                {
                    const TextEditor::CursorSelection selection = view.GetMainCursorSelection();
                    first = selection.start.line;
                    last = selection.end.line;
                    if (last > first && selection.end.column == 0)
                        --last;
                    context_has_selection = context_row >= first && context_row <= last;
                }
                if (!context_has_selection)
                {
                    first = 0;
                    last = static_cast<int>(_diff.lines.size()) - 1;
                }
                const int hunk = clicked.hunk;
                for (int row = std::max(first, 0);
                     row <= last && row < static_cast<int>(_diff.lines.size()); ++row)
                {
                    const DiffLine& line = _diff.lines[static_cast<std::size_t>(row)];
                    if (line.kind != DiffLineKind::Context
                        && (context_has_selection || (hunk >= 0 && line.hunk == hunk)))
                        context_region.push_back(line);
                }
            }
            ImGui::OpenPopup("Diff line context");
        }

        if (!ImGui::BeginPopup("Diff line context"))
            return;
        const auto [parent, child] = AdjacentRevisions(_diff.revision);
        const auto source_revision = std::ranges::find(_snapshot->revisions, _diff.revision, &Revision::oid);
        const auto child_revision = std::ranges::find(_snapshot->revisions, child, &Revision::oid);
        const bool linear_source = source_revision != _snapshot->revisions.end()
            && source_revision->parents.size() == 1;
        const bool linear_child = child_revision != _snapshot->revisions.end()
            && child_revision->parents.size() == 1 && child_revision->parents.front() == _diff.revision;
        const bool conflicted = std::ranges::any_of(_diff.files, [&](const StatusEntry& file) {
            return file.path == _diff.path && file.conflicted;
        });
        const bool stale = context_revision != _diff.revision || context_path != _diff.path;
        const bool unsupported = stale || !linear_source || !_compare_to.empty() || !_active_operation.empty()
            || conflicted || _diff.selected_status == GIT_DELTA_RENAMED
            || _diff.selected_status == GIT_DELTA_COPIED || _diff.selected_status == GIT_DELTA_TYPECHANGE
            || IsSymlinkMode(_diff.old_mode)
            || IsSymlinkMode(_diff.new_mode) || IsSubmoduleMode(_diff.old_mode) || IsSubmoduleMode(_diff.new_mode)
            || (_diff.old_mode != 0 && _diff.new_mode != 0 && _diff.old_mode != _diff.new_mode);
        if (!unsupported && !context_line.empty() && (linear_child || !parent.empty()))
        {
            const float line_height = std::max(view.GetLineHeight(), 1.0f);
            const float line_y = view_window->DC.CursorStartPos.y + context_row * line_height;
            view_window->DrawList->PushClipRect(view_window->InnerClipRect.Min, view_window->InnerClipRect.Max, true);
            view_window->DrawList->AddRectFilled(ImVec2(view_window->InnerClipRect.Min.x, line_y),
                ImVec2(view_window->InnerClipRect.Max.x, line_y + line_height),
                ImGui::GetColorU32(ImGuiCol_NavHighlight, 0.28f));
            view_window->DrawList->PopClipRect();
        }
        const auto move = [&](std::string_view icon, const char* label, const std::string& destination,
                              const std::vector<DiffLine>& lines, bool target_valid) {
            ImGui::BeginDisabled(unsupported || !target_valid || lines.empty());
            if (ActionMenuItem(icon, label))
                QueueCommands({MoveDiffLines{_diff.revision, destination, _diff.path, lines}},
                    {_diff.revision, destination},
                    "Moving these lines will rewrite a locked source or destination commit.");
            ImGui::EndDisabled();
        };
        move(ICON_MS_ARROW_UPWARD, "Move line to child", child, context_line, linear_child);
        move(ICON_MS_ARROW_DOWNWARD, "Move line to parent", parent, context_line, !parent.empty());
        ImGui::Separator();
        move(ICON_MS_ARROW_UPWARD, context_has_selection ? "Move selection to child" : "Move hunk to child",
            child, context_region, linear_child);
        move(ICON_MS_ARROW_DOWNWARD, context_has_selection ? "Move selection to parent" : "Move hunk to parent",
            parent, context_region, !parent.empty());
        ImGui::EndPopup();
    };

    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (plain)
    {
        editor.Render("##file view", available, true);
        render_move_context(editor, true);
    }
    else
    {
        diff.SetSideBySideMode(_diff_side_by_side);
        diff.Render("##diff view", available, true);
        render_move_context(diff, !_diff_side_by_side);
    }
    ImGui::End();
}

void Application::RenderOperations()
{
    if (!ImGui::Begin("Operations", &_show_operations))
    {
        ImGui::End();
        return;
    }
    if (!_error_message.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.35f, 1.0f), "%s", _error_message.c_str());
        ImGui::Separator();
    }
    if (!_progress_phase.empty())
    {
        const float progress = _progress_total == 0 ? 0.0f
                                                    : static_cast<float>(_progress_completed) / _progress_total;
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), _progress_phase.c_str());
    }
    if (!_status_message.empty())
        ImGui::TextDisabled("%s", _status_message.c_str());
    if (ImGui::BeginTable("operation table", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Operation", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (const Operation& operation : _snapshot->operations)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(operation.description.c_str());
            ImGui::TableNextColumn();
            TextHighlightedId(operation.oid, OperationPrefix(operation.oid), CommitIdColor(false));
            ImGui::TableNextColumn();
            ImGui::PushID(&operation);
            ImGui::BeginDisabled(!_active_operation.empty());
            if (ImGui::SmallButton("Restore")) _engine.Enqueue(RestoreOperation{operation.oid});
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Application::OpenDialog(Dialog dialog)
{
    if (!_active_operation.empty() && dialog != Dialog::Credentials)
        return;
    _dialog = dialog;
    _input_primary.clear();
    _input_secondary.clear();
    _input_tertiary.clear();
    _input_filesets.clear();
    _input_flag = false;
    _input_flag_secondary = false;
    _input_flag_tertiary = false;
    _input_mode = 0;
    if (dialog == Dialog::Metaedit)
    {
        const auto selected = std::ranges::find_if(
            _snapshot->revisions, [this](const Revision& revision) { return revision.oid == _selected_revision; });
        if (selected != _snapshot->revisions.end())
            _input_primary = selected->description;
    }
    if (dialog == Dialog::Split || dialog == Dialog::Restore)
        _input_filesets = _selected_file;
    if (dialog == Dialog::Credentials)
        _input_primary = _credential_request.username;
}

void Application::RenderDialogs()
{
    if (_open_save_patch)
    {
        ImGui::OpenPopup("Save Patch");
        _open_save_patch = false;
    }
    static char patch_save_path[512] = "patch.diff";
    if (ImGui::BeginPopupModal("Save Patch", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Save patch to file");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(FontPx(420.0f));
        ImGui::InputText("Path", patch_save_path, IM_ARRAYSIZE(patch_save_path));
        if (ImGui::Button("Save"))
        {
            std::ofstream output(patch_save_path, std::ios::binary);
            output.write(_diff.patch.data(), static_cast<std::streamsize>(_diff.patch.size()));
            if (output)
            {
                _status_message = "Patch saved to " + std::string(patch_save_path);
                ImGui::CloseCurrentPopup();
            }
            else
                _error_message = "Could not save patch to " + std::string(patch_save_path);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (_open_apply_patch)
    {
        ImGui::OpenPopup("Apply Patch");
        _open_apply_patch = false;
    }
    static char patch_apply_path[512]{};
    if (ImGui::BeginPopupModal("Apply Patch", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("Apply a patch to the current working copy. Invalid or conflicting patches are rejected.");
        if (ImGui::Button("Apply from Clipboard"))
        {
            const char* clipboard = ImGui::GetClipboardText();
            _engine.Enqueue(ApplyPatch{clipboard == nullptr ? "" : clipboard, {}});
            ImGui::CloseCurrentPopup();
        }
        ImGui::Spacing();
        ImGui::SetNextItemWidth(FontPx(420.0f));
        ImGui::InputText("File", patch_apply_path, IM_ARRAYSIZE(patch_apply_path));
        if (ImGui::Button("Apply from File"))
        {
            _engine.Enqueue(ApplyPatch{{}, patch_apply_path});
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (_dialog == Dialog::None)
        return;
    constexpr std::array popup_titles{"Action###ggui action", "Clone repository###ggui action",
        "Commit change###ggui action", "Edit metadata###ggui action", "Rebase change###ggui action", "Squash changes###ggui action",
        "Split change###ggui action", "Abandon change###ggui action", "Restore files###ggui action",
        "Create bookmark###ggui action", "Rename bookmark###ggui action", "Create tag###ggui action", "Add remote###ggui action",
        "Add workspace###ggui action", "Rename workspace###ggui action", "Push bookmark###ggui action", "Credentials###ggui action",
        "Confirm operation###ggui action", "Locked commit warning###ggui action"};
    if (!ImGui::IsPopupOpen("ggui action"))
        ImGui::OpenPopup("ggui action");
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(560.0f, 0.0f), ImVec2(560.0f, std::numeric_limits<float>::max()));
    const auto close_dialog = [this]() {
        if (_dialog == Dialog::Credentials) _engine.CancelCredential();
        if (_dialog == Dialog::ConfirmLocked)
        {
            _pending_commands.clear();
            _pending_change_info_save = false;
        }
        _dialog = Dialog::None;
        ImGui::ClearActiveID();
    };
    bool open = true;
    if (!ImGui::BeginPopupModal(
            popup_titles[static_cast<std::size_t>(_dialog)], &open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (!open) close_dialog();
        return;
    }
    const bool focus_first = ImGui::IsWindowAppearing();
    const float browse_width = ImGui::CalcTextSize("Browse").x + ImGui::GetStyle().FramePadding.x * 2.0f;

    switch (_dialog)
    {
    case Dialog::Clone:
        ImGui::TextUnformatted("Clone repository");
        DialogInput("URL", "https://host/owner/repository.git", &_input_primary, focus_first);
        ImGui::TextUnformatted("Destination");
        ImGui::SetNextItemWidth(-browse_width - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("###Destination", "/path/to/repository", &_input_secondary);
        ImGui::SameLine();
        if (ImGui::Button("Browse")) _input_secondary = PickFolder();
        break;
    case Dialog::Commit:
        ImGui::TextUnformatted("Commit working change and create a new one");
        DialogMultiline("Description", &_input_primary, 90.0f, focus_first);
        DialogMultiline("Filesets", &_input_filesets, 70.0f);
        ImGui::TextDisabled("Leave empty to commit all changed files.");
        break;
    case Dialog::Metaedit:
        TextLabelledId("Edit metadata for ", _selected_revision, RevisionPrefix(_selected_revision),
            CommitIdColor(_selected_revision == _snapshot->working_copy));
        DialogMultiline("Description", &_input_primary, 100.0f, focus_first);
        DialogInput("Author", "Name <email>", &_input_secondary);
        break;
    case Dialog::Rebase:
        TextLabelledId("Rebase ", _selected_revision, RevisionPrefix(_selected_revision),
            CommitIdColor(_selected_revision == _snapshot->working_copy));
        DialogInput("Destination", "change ID, bookmark, or commit ID", &_input_primary, focus_first);
        break;
    case Dialog::Squash:
        TextLabelledId("Squash ", _selected_revision, RevisionPrefix(_selected_revision),
            CommitIdColor(_selected_revision == _snapshot->working_copy));
        DialogInput("Into", "defaults to parent", &_input_secondary, focus_first);
        DialogMultiline("Combined description", &_input_primary, 90.0f);
        break;
    case Dialog::Split:
        TextLabelledId("Split ", _selected_revision, RevisionPrefix(_selected_revision),
            CommitIdColor(_selected_revision == _snapshot->working_copy));
        DialogMultiline("Selected filesets", &_input_filesets, 90.0f, focus_first);
        DialogInput("Selected description", "optional", &_input_primary);
        break;
    case Dialog::Restore:
        TextLabelledId("Restore into ", _selected_revision, RevisionPrefix(_selected_revision),
            CommitIdColor(_selected_revision == _snapshot->working_copy));
        DialogInput("From", "defaults to parent", &_input_primary, focus_first);
        DialogMultiline("Filesets", &_input_filesets, 90.0f);
        ImGui::TextDisabled("Leave empty to restore all files.");
        break;
    case Dialog::Abandon:
    {
        TextLabelledId("Abandon ", _selected_revision, RevisionPrefix(_selected_revision),
            CommitIdColor(_selected_revision == _snapshot->working_copy));
        ImGui::SameLine();
        ImGui::TextWrapped("and restack its descendants. This remains undoable.");
        if (ImGui::Checkbox("Also abandon all descendants (full branch)", &_input_flag_tertiary)
            && _input_flag_tertiary)
        {
            const std::vector<std::string> revisions = AbandonRevisions(_selected_revision, true);
            _input_flag = _input_flag || std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
                return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK
                    && std::ranges::find(revisions, ref.target) != revisions.end();
            });
        }
        const std::vector<std::string> revisions =
            AbandonRevisions(_selected_revision, _input_flag_tertiary);
        if (_input_flag_tertiary)
            ImGui::TextDisabled("%zu changes will be abandoned.", revisions.size());
        ImGui::Checkbox("Retain bookmarks", &_input_flag);
        const std::vector<RemoteBookmarkDelete> remote_bookmarks = RemoteBookmarksAt(revisions);
        if (!remote_bookmarks.empty())
        {
            ImGui::Checkbox("Also delete bookmark from remote", &_input_flag_secondary);
            if (_input_flag_secondary)
                for (const RemoteBookmarkDelete& bookmark : remote_bookmarks)
                    ImGui::TextDisabled("%s/%s", bookmark.remote.c_str(), bookmark.bookmark.c_str());
        }
        break;
    }
    case Dialog::Bookmark:
        ImGui::TextUnformatted("Create bookmark");
        DialogInput("Name", "bookmark name", &_input_primary, focus_first);
        DialogInput("Revision", "defaults to selected change", &_input_secondary);
        break;
    case Dialog::BookmarkRename:
    {
        ImGui::Text("Rename bookmark %s", _input_secondary.c_str());
        DialogInput("New name", "bookmark name", &_input_primary, focus_first);
        const bool conflict = _snapshot != nullptr && std::ranges::any_of(_snapshot->refs, [this](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == _input_primary;
        });
        if (_input_primary == _input_secondary)
            ImGui::TextDisabled("Choose a different name.");
        else if (conflict)
            ImGui::TextDisabled("A local bookmark already uses this name.");
        break;
    }
    case Dialog::Tag:
        ImGui::TextUnformatted("Create or move tag");
        DialogInput("Name", "tag name", &_input_primary, focus_first);
        DialogInput("Revision", "defaults to selected change", &_input_secondary);
        ImGui::Checkbox("Allow move", &_input_flag);
        break;
    case Dialog::RemoteAdd:
        ImGui::TextUnformatted("Add remote");
        DialogInput("Name", "origin", &_input_primary, focus_first);
        DialogInput("URL", "https://host/owner/repository.git", &_input_secondary);
        break;
    case Dialog::WorkspaceAdd:
        ImGui::TextUnformatted("Add workspace");
        ImGui::TextUnformatted("Destination");
        if (focus_first)
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-browse_width - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("###Destination", "/path/to/workspace", &_input_primary);
        ImGui::SameLine();
        if (ImGui::Button("Browse")) _input_primary = PickFolder();
        DialogInput("Name", "derived from directory if empty", &_input_secondary);
        DialogInput("Revision", "defaults to @", &_input_tertiary);
        break;
    case Dialog::WorkspaceRename:
        ImGui::TextUnformatted("Rename current workspace");
        DialogInput("New name", "workspace name", &_input_primary, focus_first);
        break;
    case Dialog::PushTo:
        ImGui::Text("Push bookmark %s", _input_secondary.c_str());
        ImGui::TextUnformatted("Remote");
        if (focus_first)
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("###Remote", _input_primary.c_str()))
        {
            for (const Remote& remote : _snapshot->remotes)
                if (ImGui::Selectable(remote.name.c_str(), remote.name == _input_primary))
                    _input_primary = remote.name;
            ImGui::EndCombo();
        }
        break;
    case Dialog::Credentials:
        ImGui::TextWrapped("Credentials requested by %s", _credential_request.url.c_str());
        ImGui::TextUnformatted("Method");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("###Method", &_input_mode, "Username / token\0SSH agent\0SSH key\0");
        DialogInput("Username", "username", &_input_primary, focus_first);
        if (_input_mode == 2)
        {
            DialogInput("Private key", "/path/to/private/key", &_input_secondary);
            DialogInput("Public key", "/path/to/public/key", &_input_tertiary);
        }
        if (_input_mode != 1)
            DialogInput("Token or passphrase", "secret", &_input_filesets, false, ImGuiInputTextFlags_Password);
        break;
    case Dialog::ConfirmDrop:
    {
        const char* action = _pending_drop.action == DropAction::Squash ? "Squash"
            : _pending_drop.action == DropAction::Rebase               ? "Rebase"
            : _pending_drop.action == DropAction::ReorderAfter         ? "Move after"
                                                                       : "Move before";
        TextLabelledId(std::string(action) + " ", _pending_drop.source, RevisionPrefix(_pending_drop.source),
            CommitIdColor(_pending_drop.source == _snapshot->working_copy));
        TextLabelledId("Target: ", _pending_drop.target, RevisionPrefix(_pending_drop.target),
            CommitIdColor(_pending_drop.target == _snapshot->working_copy));
        int affected = 0;
        for (const Revision& revision : _snapshot->revisions)
            affected += std::ranges::find(revision.parents, _pending_drop.source) != revision.parents.end();
        int refs = 0;
        for (const NamedRef& ref : _snapshot->refs)
            refs += ref.target == _pending_drop.source || ref.target == _pending_drop.target;
        ImGui::TextWrapped("gg will restack affected descendants and move associated refs atomically. Direct children: %d; refs on source/target: %d. Conflicts remain editable and this operation can be undone.",
            affected, refs);
        break;
    }
    case Dialog::ConfirmLocked:
        ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.24f, 1.0f), "Warning: locked commit");
        ImGui::TextWrapped("%s", _locked_warning.c_str());
        ImGui::TextWrapped("Locked commits have already been pushed. Continuing can make local history diverge from the remote and require a force push.");
        break;
    case Dialog::None: break; // GCOV_EXCL_LINE: RenderDialogs returns before switching on None
    }

    const bool modifies_locked = DialogModifiesLockedCommit();
    if (modifies_locked && _dialog != Dialog::ConfirmLocked)
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.24f, 1.0f), "Warning: this operation modifies a locked commit.");
        ImGui::TextWrapped("Locked commits have already been pushed. Continuing can make local history diverge from the remote.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const bool can_submit = CanSubmitDialog();
    const bool operation_blocks_submit = !_active_operation.empty() && _dialog != Dialog::Credentials;
    const bool submit_shortcut = ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter);
    const bool cancel_shortcut = ImGui::IsKeyPressed(ImGuiKey_Escape);
    const bool focus_submit = _dialog == Dialog::ConfirmDrop;
    const bool focus_cancel = _dialog == Dialog::Abandon || _dialog == Dialog::ConfirmLocked;
    if (focus_first && focus_submit)
        ImGui::SetKeyboardFocusHere();
    ImGui::BeginDisabled(operation_blocks_submit || !can_submit);
    const char* submit_label = _dialog == Dialog::ConfirmDrop || _dialog == Dialog::ConfirmLocked ? "Confirm" : "Apply";
    const bool submit = (modifies_locked ? DangerButton(submit_label, ImVec2(110.0f, 0.0f))
                                         : ImGui::Button(submit_label, ImVec2(110.0f, 0.0f)))
        || (submit_shortcut && !operation_blocks_submit && can_submit);
    if (!can_submit && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Fill in the required fields before applying.");
    if (submit)
        SubmitDialog();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (focus_first && focus_cancel)
        ImGui::SetKeyboardFocusHere();
    const bool cancel = ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)) || cancel_shortcut;
    ImGui::SameLine();
    ImGui::TextDisabled("Ctrl+Enter apply | Esc cancel");
    if (cancel)
    {
        close_dialog();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::SubmitDialog()
{
    switch (_dialog)
    {
    case Dialog::Clone: _engine.Enqueue(CloneRepository{_input_primary, _input_secondary}); break;
    case Dialog::Commit: _engine.Enqueue(Commit{_input_primary, SplitLines(_input_filesets)}); break;
    case Dialog::Metaedit: _engine.Enqueue(Metaedit{_selected_revision, _input_primary, _input_secondary}); break;
    case Dialog::Rebase: _engine.Enqueue(Rebase{_selected_revision, _input_primary}); break;
    case Dialog::Squash: _engine.Enqueue(Squash{_selected_revision, _input_secondary, _input_primary}); break;
    case Dialog::Split: _engine.Enqueue(Split{_selected_revision, _input_primary, SplitLines(_input_filesets)}); break;
    case Dialog::Abandon:
    {
        const std::vector<std::string> revisions =
            AbandonRevisions(_selected_revision, _input_flag_tertiary);
        _engine.Enqueue(Abandon{revisions, _input_flag, false,
            _input_flag_secondary ? RemoteBookmarksAt(revisions)
                                  : std::vector<RemoteBookmarkDelete>{}});
        break;
    }
    case Dialog::Restore:
        _engine.Enqueue(Restore{_input_primary, _selected_revision, SplitLines(_input_filesets)});
        break;
    case Dialog::Bookmark:
        _engine.Enqueue(Bookmark{GG_BOOKMARK_CREATE, {_input_primary},
            _input_secondary.empty() ? _selected_revision : _input_secondary, {}});
        break;
    case Dialog::BookmarkRename:
        _engine.Enqueue(Bookmark{GG_BOOKMARK_RENAME, {_input_secondary}, {}, _input_primary});
        break;
    case Dialog::Tag:
        _engine.Enqueue(Tag{GG_TAG_SET, {_input_primary},
            _input_secondary.empty() ? _selected_revision : _input_secondary, _input_flag});
        break;
    case Dialog::RemoteAdd: _engine.Enqueue(AddRemote{_input_primary, _input_secondary}); break;
    case Dialog::WorkspaceAdd:
        _engine.Enqueue(WorkspaceAdd{_input_primary, _input_secondary,
            _input_tertiary.empty() ? "@" : _input_tertiary, {}});
        break;
    case Dialog::WorkspaceRename: _engine.Enqueue(WorkspaceRename{_input_primary}); break;
    case Dialog::PushTo: _engine.Enqueue(Push{_input_secondary, _input_primary}); break;
    case Dialog::Credentials:
    {
        CredentialResponse response;
        response.method = static_cast<CredentialResponse::Method>(_input_mode);
        response.username = _input_primary;
        response.secret = _input_filesets;
        response.private_key = _input_secondary;
        response.public_key = _input_tertiary;
        _engine.SubmitCredential(std::move(response));
        std::fill(_input_filesets.begin(), _input_filesets.end(), '\0');
        break;
    }
    case Dialog::ConfirmDrop:
        if (_pending_drop.action == DropAction::Squash)
            _engine.Enqueue(Squash{_pending_drop.source, _pending_drop.target, {}});
        else if (_pending_drop.action == DropAction::Rebase)
            _engine.Enqueue(Rebase{_pending_drop.source, _pending_drop.target});
        else
            _engine.Enqueue(Reorder{
                _pending_drop.source, _pending_drop.target, DropPlacement(_pending_drop.action)});
        break;
    case Dialog::ConfirmLocked:
        for (Command& command : _pending_commands)
            _engine.Enqueue(std::move(command));
        _pending_commands.clear();
        if (_pending_change_info_save)
            _change_info_dirty = false;
        _pending_change_info_save = false;
        break;
    case Dialog::None: break; // GCOV_EXCL_LINE: no dialog can submit None
    }
    _dialog = Dialog::None;
    ImGui::ClearActiveID();
    ImGui::CloseCurrentPopup();
}

void Application::SelectRevision(const std::string& oid, bool additive)
{
    if (_snapshot != nullptr && oid == _snapshot->working_copy)
    {
        _compare_to.clear();
        _file_comparison = false;
    }
    const auto selected = std::ranges::find(_selected_revisions, oid);
    if (!additive)
        _selected_revisions = {oid};
    else if (selected == _selected_revisions.end())
        _selected_revisions.push_back(oid);
    else
        _selected_revisions.erase(selected);

    _selected_revision = std::ranges::find(_selected_revisions, oid) != _selected_revisions.end()
        ? oid
        : _selected_revisions.empty() ? ""
                                      : _selected_revisions.back();
    if (_selected_revision.empty())
    {
        _compare_to.clear();
        _file_comparison = false;
    }
    RequestDiff(true);
}

void Application::RequestDiff(bool fallback_to_first)
{
    if (_selected_revision.empty())
    {
        _selected_file.clear();
        _pending_revision.clear();
        _diff = {};
        _diff_loading = false;
    }
    else
    {
        _pending_revision = fallback_to_first ? _selected_revision : "";
        _diff_loading = true;
        _engine.Enqueue(LoadDiff{_selected_revision, fallback_to_first ? _preferred_file : _selected_file,
            fallback_to_first, {_diff_whitespace_mode, _diff_context_lines}, _compare_to, _file_comparison});
    }
}

void Application::ToggleComparison(bool file_comparison)
{
    if (!_compare_to.empty() && _file_comparison == file_comparison)
    {
        _compare_to.clear();
        _file_comparison = false;
    }
    else if (_snapshot != nullptr && !_selected_revision.empty() && !_snapshot->working_copy.empty()
        && _selected_revision != _snapshot->working_copy)
    {
        _compare_to = _snapshot->working_copy;
        _file_comparison = file_comparison;
    }
    RequestDiff(true);
}

void Application::RevealRevision(const std::string& oid)
{
    _graph_filter.clear();
    _show_history = true;
    _reveal_revision = oid;
    SelectRevision(oid);
}

bool Application::CanCreateChange() const
{
    return _snapshot != nullptr && !_selected_revisions.empty()
        && std::ranges::all_of(_selected_revisions, [this](const std::string& oid) {
               return std::ranges::any_of(
                   _snapshot->revisions, [&](const Revision& revision) { return revision.oid == oid; });
           });
}

std::vector<std::string> Application::SelectedParentRevisions() const
{
    std::vector<std::string> parents;
    parents.reserve(_selected_revisions.size());
    for (const std::string& oid : _selected_revisions)
    {
        if (oid == _snapshot->working_copy)
        {
            parents.emplace_back("@");
            continue;
        }
        const auto revision = std::ranges::find_if(
            _snapshot->revisions, [&](const Revision& candidate) { return candidate.oid == oid; });
        parents.push_back(revision == _snapshot->revisions.end() || revision->change_id.empty()
                ? oid
                : revision->change_id);
    }
    return parents;
}

void Application::CreateChange()
{
    if (!_active_operation.empty())
        return;
    const auto selected = _selected_revisions.size() == 1
        ? std::ranges::find(_snapshot->revisions, _selected_revisions.front(), &Revision::oid)
        : _snapshot->revisions.end();
    if (selected != _snapshot->revisions.end() && selected->oid == _snapshot->working_copy && selected->empty)
    {
        _engine.Enqueue(Refresh{});
        return;
    }
    _engine.Enqueue(NewChange{{}, SelectedParentRevisions(), {}, {}, false});
}

bool Application::IsLocked(const std::string& identifier) const
{
    if (_snapshot == nullptr || identifier.empty())
        return false;
    std::string oid = identifier == "@" ? _snapshot->working_copy : identifier;
    const auto ref = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& candidate) {
        return candidate.name == oid;
    });
    if (ref != _snapshot->refs.end())
        oid = ref->target;
    const auto revision = std::ranges::find_if(_snapshot->revisions, [&](const Revision& candidate) {
        return candidate.oid == oid || candidate.change_id == oid;
    });
    return revision != _snapshot->revisions.end() && revision->pushed;
}

bool Application::DialogModifiesLockedCommit() const
{
    switch (_dialog)
    {
    case Dialog::Commit: return IsLocked(_snapshot->working_copy);
    case Dialog::Metaedit:
    case Dialog::Split: return IsLocked(_selected_revision);
    case Dialog::Abandon:
    {
        const std::vector<std::string> revisions =
            AbandonRevisions(_selected_revision, _input_flag_tertiary);
        return std::ranges::any_of(
            revisions, [this](const std::string& revision) { return IsLocked(revision); });
    }
    case Dialog::Rebase: return IsLocked(_selected_revision) || IsLocked(_input_primary);
    case Dialog::Squash:
    {
        std::string destination = _input_secondary;
        if (destination.empty())
        {
            const auto source = std::ranges::find(_snapshot->revisions, _selected_revision, &Revision::oid);
            if (source != _snapshot->revisions.end() && !source->parents.empty())
                destination = source->parents.front();
        }
        return IsLocked(_selected_revision) || IsLocked(destination);
    }
    case Dialog::Restore: return IsLocked(_selected_revision) || IsLocked(_input_primary);
    case Dialog::ConfirmDrop: return IsLocked(_pending_drop.source) || IsLocked(_pending_drop.target);
    case Dialog::ConfirmLocked: return true;
    default: return false;
    }
}

void Application::QueueCommands(
    std::vector<Command> commands, const std::vector<std::string>& revisions, std::string warning)
{
    if (!_active_operation.empty())
        return;
    if (std::ranges::none_of(revisions, [this](const std::string& revision) { return IsLocked(revision); }))
    {
        for (Command& command : commands)
            _engine.Enqueue(std::move(command));
        return;
    }
    _pending_commands = std::move(commands);
    _locked_warning = std::move(warning);
    OpenDialog(Dialog::ConfirmLocked);
}

std::vector<std::string> Application::AbandonRevisions(
    const std::string& selected, bool include_descendants) const
{
    std::vector<std::string> result{selected};
    if (!include_descendants)
        return result;
    bool added = true;
    while (added)
    {
        added = false;
        for (const Revision& revision : _snapshot->revisions)
        {
            if (std::ranges::find(result, revision.oid) != result.end())
                continue;
            if (std::ranges::any_of(revision.parents, [&](const std::string& parent) {
                    return std::ranges::find(result, parent) != result.end();
                }))
            {
                result.push_back(revision.oid);
                added = true;
            }
        }
    }
    return result;
}

std::vector<RemoteBookmarkDelete> Application::RemoteBookmarksAt(
    const std::vector<std::string>& revisions) const
{
    std::vector<RemoteBookmarkDelete> result;
    for (const NamedRef& local : _snapshot->refs)
    {
        if (local.kind != GG_NAMED_REF_LOCAL_BOOKMARK
            || std::ranges::find(revisions, local.target) == revisions.end())
            continue;
        for (const NamedRef& remote : _snapshot->refs)
        {
            if (remote.kind != GG_NAMED_REF_REMOTE_BOOKMARK || remote.name != local.name
                || remote.target != local.target || remote.remote.empty())
                continue;
            const RemoteBookmarkDelete deletion{local.name, remote.remote};
            if (std::ranges::none_of(result, [&](const RemoteBookmarkDelete& existing) {
                    return existing.bookmark == deletion.bookmark && existing.remote == deletion.remote;
                }))
                result.push_back(deletion);
        }
    }
    return result;
}

void Application::RequestAbandon(const std::string& revision)
{
    if (!_active_operation.empty() || revision.empty())
        return;
    if (_selected_revision != revision)
        SelectRevision(revision);
    const auto selected = std::ranges::find(_snapshot->revisions, revision, &Revision::oid);
    const bool has_refs = std::ranges::any_of(
        _snapshot->refs, [&](const NamedRef& ref) { return ref.target == revision; });
    if (selected != _snapshot->revisions.end() && selected->empty && !has_refs && !selected->pushed)
        _engine.Enqueue(Abandon{{revision}, false, false, {}});
    else
    {
        OpenDialog(Dialog::Abandon);
        _input_flag = std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.target == revision;
        });
    }
}

bool Application::CanSubmitDialog() const
{
    switch (_dialog)
    {
    case Dialog::Clone: return HasText(_input_primary) && HasText(_input_secondary);
    case Dialog::Rebase: return HasText(_input_primary);
    case Dialog::Split: return HasText(_input_filesets);
    case Dialog::Bookmark:
        return HasText(_input_primary);
    case Dialog::BookmarkRename:
        return HasText(_input_primary) && _input_primary != _input_secondary && _snapshot != nullptr
            && std::ranges::none_of(_snapshot->refs, [this](const NamedRef& ref) {
                   return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == _input_primary;
               });
    case Dialog::Tag:
    case Dialog::WorkspaceAdd:
    case Dialog::WorkspaceRename: return HasText(_input_primary);
    case Dialog::RemoteAdd: return HasText(_input_primary) && HasText(_input_secondary);
    case Dialog::PushTo: return HasText(_input_primary) && HasText(_input_secondary);
    case Dialog::Credentials:
        return HasText(_input_primary) && (_input_mode == 1
            || (_input_mode == 2 ? HasText(_input_secondary) : HasText(_input_filesets)));
    case Dialog::None: return false; // GCOV_EXCL_LINE: RenderDialogs returns before validation
    default: return true;
    }
}

gg_reorder_placement Application::DropPlacement(DropAction action)
{
    return action == DropAction::ReorderAfter ? GG_REORDER_BEFORE : GG_REORDER_AFTER;
}

std::string Application::DropTooltip(DropAction action, std::string_view target)
{
    const std::string_view verb = action == DropAction::ReorderBefore ? "Move before "
        : action == DropAction::ReorderAfter                           ? "Move after "
        : action == DropAction::Squash                                 ? "Squash into "
                                                                       : "Rebase onto ";
    return std::string(verb) + std::string(target);
}

void Application::SelectFile(const std::string& path)
{
    _selected_file = path;
    _preferred_file = path;
    _pending_revision.clear();
    if (!_selected_revision.empty())
    {
        RequestDiff(false);
    }
}

std::pair<std::string, std::string> Application::AdjacentRevisions(const std::string& oid) const
{
    std::string parent;
    std::string child;
    if (_snapshot == nullptr)
        return {parent, child};
    const auto source = std::ranges::find(_snapshot->revisions, oid, &Revision::oid);
    if (source == _snapshot->revisions.end())
        return {parent, child};
    if (source->parents.size() == 1)
        parent = source->parents.front();
    int children = 0;
    for (const Revision& revision : _snapshot->revisions)
    {
        if (std::ranges::find(revision.parents, source->oid) == revision.parents.end())
            continue;
        child = revision.oid;
        ++children;
    }
    if (children != 1)
        child.clear();
    return {parent, child};
}

void Application::ResetRepositoryState()
{
    _snapshot.reset();
    _diff = {};
    _visible_revisions.clear();
    _graph_rows.clear();
    _graph_generation = 0;
    _revision_prefixes.clear();
    _change_prefixes.clear();
    _operation_prefixes.clear();
    _selected_revision.clear();
    _selected_revisions.clear();
    _selected_file.clear();
    _preferred_file.clear();
    _pending_revision.clear();
    _compare_to.clear();
    _file_comparison = false;
    _open_save_patch = false;
    _open_apply_patch = false;
    _bookmark_filter.clear();
    _tag_filter.clear();
    _changes_filter.clear();
    _graph_filter.clear();
    _built_filter.clear();
    _diff_loading = false;
    _default_layout = true;
    _status_message.clear();
    _error_message.clear();
    if (_window != nullptr)
        SDL_SetWindowTitle(_window, "ggui");
}

bool Application::FileMatchesFilter(const StatusEntry& file) const
{
    const std::string status = DeltaName(file.status);
    return ContainsInsensitive(status, _changes_filter) || ContainsInsensitive(file.path, _changes_filter)
        || ContainsInsensitive(file.old_path, _changes_filter);
}

bool Application::CanNavigateChangedFile(int direction) const
{
    std::vector<const StatusEntry*> files;
    for (const StatusEntry& file : _diff.files)
        if (FileMatchesFilter(file))
            files.push_back(&file);
    if (files.empty())
        return false;
    const auto selected = std::ranges::find_if(files,
        [this](const StatusEntry* file) { return file->path == _selected_file; });
    if (selected == files.end())
        return true;
    return direction < 0 ? selected != files.begin() : std::next(selected) != files.end();
}

void Application::NavigateChangedFile(int direction)
{
    std::vector<const StatusEntry*> files;
    for (const StatusEntry& file : _diff.files)
        if (FileMatchesFilter(file))
            files.push_back(&file);
    if (files.empty())
        return;
    auto selected = std::ranges::find_if(files,
        [this](const StatusEntry* file) { return file->path == _selected_file; });
    if (selected == files.end())
        selected = direction < 0 ? std::prev(files.end()) : files.begin();
    else if (direction < 0 && selected != files.begin())
        --selected;
    else if (direction > 0 && std::next(selected) != files.end())
        ++selected;
    else
        return;
    SelectFile((*selected)->path);
}

std::optional<std::filesystem::path> Application::WorkingCopyPath(
    const std::string& root, const std::string& relative)
{
    if (root.empty() || relative.empty())
        return std::nullopt;
    const std::filesystem::path relative_path(relative);
    if (relative_path.is_absolute())
        return std::nullopt;
    std::error_code error;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error)
        return std::nullopt;
    const std::filesystem::path candidate = std::filesystem::weakly_canonical(canonical_root / relative_path, error);
    if (error)
        return std::nullopt;
    const std::filesystem::path within = candidate.lexically_relative(canonical_root);
    if (within.empty() || within.is_absolute() || *within.begin() == "..")
        return std::nullopt;
    return candidate;
}

// GCOV_EXCL_START: OS-default file handlers are platform integrations
void Application::OpenExternalPath(const std::filesystem::path& path, std::string_view description)
{
    if (!SDL_OpenURL(FileUrl(path.string()).c_str()))
        _error_message = SDL_GetError();
    else
        _status_message = std::string(description) + " opened";
}

void Application::OpenExternalDiff(const std::string& path, const std::string& compare_to)
{
    if (_snapshot == nullptr || _diff.revision.empty() || path.empty())
        return;
    const std::string revision = _diff.revision + "^!";
    std::vector<const char*> arguments{"git", "-C", _snapshot->root.c_str(), "difftool", "--no-prompt",
        compare_to.empty() ? revision.c_str() : _diff.revision.c_str()};
    if (!compare_to.empty())
        arguments.push_back(compare_to.c_str());
    arguments.insert(arguments.end(), {"--", path.c_str(), nullptr});
    SDL_Process* process = SDL_CreateProcess(arguments.data(), false); // GCOV_EXCL_LINE: external application handoff
    if (process == nullptr)
        _error_message = SDL_GetError(); // GCOV_EXCL_LINE: platform process failure
    else
    {
        SDL_DestroyProcess(process); // GCOV_EXCL_LINE: external process owns its lifetime
        _status_message = "External diff opened";
    }
}
// GCOV_EXCL_STOP

// GCOV_EXCL_START: nativefiledialog owns the platform-dependent modal interaction
void Application::PickAndOpen(bool initialize)
{
    if (!_active_operation.empty())
        return;
    const std::string path = PickFolder();
    if (!path.empty())
        _engine.Enqueue(initialize ? Command{InitRepository{path}} : Command{OpenRepository{path}});
}

std::string Application::PickFolder(const std::string& initial)
{
    if (NFD_Init() != NFD_OKAY)
    {
        _error_message = NFD_GetError() == nullptr ? "Could not initialize folder picker" : NFD_GetError();
        return {};
    }
    nfdu8char_t* path = nullptr;
    nfdpickfolderu8args_t arguments{};
    arguments.defaultPath = initial.empty() ? nullptr : initial.c_str();
    const nfdresult_t result = NFD_PickFolderU8_With(&path, &arguments);
    std::string selected;
    if (result == NFD_OKAY && path != nullptr)
        selected = path;
    else if (result == NFD_ERROR)
        _error_message = NFD_GetError() == nullptr ? "Folder picker failed" : NFD_GetError();
    if (path != nullptr)
        NFD_FreePathU8(path);
    NFD_Quit();
    return selected;
}
// GCOV_EXCL_STOP

void Application::ApplyTheme()
{
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
        colors[ImGuiCol_TabActive] = ImVec4(0.098f, 0.129f, 0.176f, 1.0f);
        colors[ImGuiCol_TabUnfocused] = ImVec4(0.074f, 0.094f, 0.129f, 1.0f);
        colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.090f, 0.114f, 0.157f, 1.0f);
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
}

#ifdef IMGUI_BUILD_TESTING
void Application::InitializeTestEngine(const std::string& filter)
{
    _test_engine = ImGuiTestEngine_CreateContext();
    if (_test_engine == nullptr)
        throw std::runtime_error("could not create ImGui Test Engine"); // GCOV_EXCL_LINE: forced test-engine allocation failure
    RegisterUiTests(_test_engine);
    ImGuiTestEngine_Start(_test_engine, ImGui::GetCurrentContext());
    ImGuiTestEngineIO& io = ImGuiTestEngine_GetIO(_test_engine);
    io.ConfigLogToTTY = true;
    io.ConfigCaptureEnabled = std::getenv("GGUI_CAPTURE_MANUAL") != nullptr;
    io.ScreenCaptureFunc = CaptureFramebuffer;
    io.ConfigFixedDeltaTime = 1.0f / 60.0f;
    ImGuiTestEngine_QueueTests(
        _test_engine, ImGuiTestGroup_Unknown, filter.c_str(), ImGuiTestRunFlags_RunFromCommandLine);
}

Application& Application::Instance()
{
    return *test_application;
}

void Application::OpenTestRepository(const std::string& path)
{
    _test_snapshot_mode = false;
#ifdef GGUI_TESTING
    _engine.SetCommandsSuppressedForTest(false);
#endif
    _engine.Enqueue(OpenRepository{path});
}

void Application::RefreshForTest()
{
    _engine.Enqueue(Refresh{});
}

std::shared_ptr<const RepoSnapshot> Application::SnapshotForTest() const
{
    return _snapshot;
}

const std::string& Application::SelectedFileForTest() const
{
    return _selected_file;
}

const std::vector<std::string>& Application::SelectedRevisionsForTest() const
{
    return _selected_revisions;
}

bool Application::DiffSideBySideForTest() const
{
    return _diff_side_by_side;
}

const std::string& Application::CompareToForTest() const
{
    return _compare_to;
}

bool Application::FileComparisonForTest() const
{
    return _file_comparison;
}

bool Application::CanNavigateChangedFileForTest(int direction) const
{
    return CanNavigateChangedFile(direction);
}

void Application::NavigateChangedFileForTest(int direction)
{
    NavigateChangedFile(direction);
}

void Application::ToggleComparisonForTest()
{
    ToggleComparison(false);
}

void Application::ToggleFileComparisonForTest()
{
    ToggleComparison(true);
}

void Application::SelectRevisionForTest(const std::string& oid, bool additive)
{
    SelectRevision(oid, additive);
}

std::vector<std::string> Application::SelectedParentsForTest() const
{
    return SelectedParentRevisions();
}

std::vector<std::string> Application::AbandonRevisionsForTest(const std::string& revision) const
{
    return AbandonRevisions(revision, true);
}

std::vector<std::string> Application::DialogFilesetsForTest() const
{
    return SplitLines(_input_filesets);
}

void Application::ApplyEventForTest(Event event)
{
    ApplyEvent(std::move(event));
}

void Application::ShowDropConfirmationForTest(const std::string& source, const std::string& target, int action)
{
    _pending_drop = {source, target, static_cast<DropAction>(std::clamp(action, 0, 3))};
    OpenDialog(Dialog::ConfirmDrop);
}

void Application::ShowWorkspaceRenameForTest()
{
    OpenDialog(Dialog::WorkspaceRename);
}

void Application::ShowBookmarkRenameForTest(const std::string& name)
{
    OpenDialog(Dialog::BookmarkRename);
    _input_primary = name;
    _input_secondary = name;
}

void Application::SetSnapshotForTest(RepoSnapshot snapshot)
{
    _test_snapshot_mode = true;
#ifdef GGUI_TESTING
    _engine.SetCommandsSuppressedForTest(true);
#endif
    _snapshot = std::make_shared<RepoSnapshot>(std::move(snapshot));
    RebuildIdPrefixes();
    _selected_revision = _snapshot->working_copy.empty()
        ? (_snapshot->revisions.empty() ? "" : _snapshot->revisions.front().oid)
        : _snapshot->working_copy;
    _selected_revisions = _selected_revision.empty() ? std::vector<std::string>{}
                                                     : std::vector{_selected_revision};
    _selected_file.clear();
    _preferred_file.clear();
    _pending_revision.clear();
    _compare_to.clear();
    _file_comparison = false;
    _diff = {_snapshot->generation, _selected_revision, {}, {}, {}, false, _snapshot->status};
    _diff_loading = false;
    _graph_generation = 0;
}

void Application::ClearSnapshotForTest()
{
    ResetRepositoryState();
}

void Application::AddRecentForTest(const std::string& path)
{
    RememberRepository(path);
}

void Application::ProcessEventForTest(SDL_Event event)
{
    const bool running = _running;
    ProcessEvent(event);
    _running = running;
}

void Application::SetDarkThemeForTest(bool dark)
{
    _dark_theme = dark;
    ApplyTheme();
}

std::vector<std::string> Application::SplitLinesForTest(const std::string& text)
{
    return SplitLines(text);
}

std::string Application::DeltaNameForTest(git_delta_t status)
{
    return DeltaName(status);
}

bool Application::ContainsInsensitiveForTest(const std::string& text, const std::string& query)
{
    return ContainsInsensitive(text, query);
}

unsigned int Application::IdColorForTest(bool change_id, bool working_copy)
{
    return change_id ? ChangeIdColor(working_copy) : CommitIdColor(working_copy);
}

bool Application::SupportsDiffLanguageForTest(const std::string& path)
{
    return DiffLanguage(path) != nullptr;
}

std::string Application::FileUrlForTest(const std::string& path)
{
    return FileUrl(path);
}

std::optional<std::filesystem::path> Application::WorkingCopyPathForTest(
    const std::string& root, const std::string& relative)
{
    return WorkingCopyPath(root, relative);
}

std::string Application::LimitLinesForTest(const std::string& text, std::size_t maximum)
{
    return LimitedLines(text, maximum);
}

std::string Application::ReferenceLabelForTest(const NamedRef& ref)
{
    return ReferenceLabel(ref);
}

std::pair<std::string, std::size_t> Application::ReferenceBadgeLabelForTest(
    const NamedRef& ref, const std::vector<NamedRef>& refs)
{
    return ReferenceBadgeLabel(ref, refs);
}

unsigned int Application::BookmarkColorForTest(const std::string& name, const std::vector<NamedRef>& refs)
{
    return BookmarkBadgeColor(name, refs);
}

std::string Application::FormatTimestampForTest(std::int64_t timestamp)
{
    return FormatTimestamp(timestamp);
}

int Application::DropPlacementForTest(int action)
{
    return DropPlacement(static_cast<DropAction>(std::clamp(action, 0, 3)));
}

std::string Application::DropTooltipForTest(int action, const std::string& target)
{
    return DropTooltip(static_cast<DropAction>(std::clamp(action, 0, 3)), target);
}
#endif

} // namespace Ggui

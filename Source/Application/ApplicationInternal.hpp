// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "Application.hpp"

#include <TextEditor.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ggui
{

extern "C" const unsigned char ggui_icon_font_data[];
extern "C" const unsigned int ggui_icon_font_data_len;
extern "C" const unsigned char ggui_ui_font_data[];
extern "C" const unsigned int ggui_ui_font_data_len;
extern "C" const unsigned char ggui_diff_font_data[];
extern "C" const unsigned int ggui_diff_font_data_len;

namespace ApplicationInternal
{

inline bool IsWorkingTreeRevision(std::string_view revision)
{
    return revision.starts_with("working-tree:");
}

// Working-tree IDs change with the repository generation but name one tree.
inline bool SameDiffRevision(std::string_view left, std::string_view right)
{
    return left == right || (IsWorkingTreeRevision(left) && IsWorkingTreeRevision(right));
}

SDL_Rect FitWindowToDisplays(SDL_Rect window, std::span<const SDL_Rect> displays);

inline constexpr float kRowHeight = 34.0f;
inline constexpr float kLaneWidth = 12.0f;
inline constexpr float kDotRadius = 5.0f;
inline constexpr float kGraphPadding = 12.0f;

inline float HistoryLaneWidth(float, int)
{
    return kLaneWidth;
}

inline float HistoryContentOffset(float lane_width, int row_columns)
{
    return std::max(1, row_columns) * lane_width + kGraphPadding * 2.0f + 8.0f;
}

inline constexpr ImU32 kTextMuted = IM_COL32(125, 133, 144, 255);
inline constexpr ImU32 kBadgeTextMuted = IM_COL32(255, 255, 255, 145);
inline constexpr ImU32 kRowBackground = IM_COL32(22, 27, 34, 255);
inline constexpr ImU32 kRowHover = IM_COL32(28, 34, 43, 255);
inline constexpr ImU32 kRowSelected = IM_COL32(33, 52, 74, 255);
inline constexpr ImU32 kRowBorder = IM_COL32(48, 54, 61, 180);
inline constexpr ImU32 kGraphBackground = IM_COL32(18, 22, 29, 255);
inline constexpr ImU32 kBadgeBookmark = IM_COL32(9, 105, 218, 235);
inline constexpr ImU32 kBadgeBookmarkSynced = IM_COL32(31, 136, 61, 235);
inline constexpr ImU32 kBadgeBookmarkDiverged = IM_COL32(219, 109, 40, 235);
inline constexpr ImU32 kBadgeTag = IM_COL32(88, 70, 155, 235);
inline constexpr ImU32 kBadgeRemote = IM_COL32(66, 68, 90, 235);
inline constexpr ImU32 kBadgeWorkingCopy = IM_COL32(31, 136, 61, 235);
inline constexpr ImU32 kStatusAdded = IM_COL32(46, 160, 67, 255);
inline constexpr ImU32 kStatusModified = IM_COL32(210, 153, 34, 255);
inline constexpr ImU32 kStatusDeleted = IM_COL32(248, 81, 73, 255);
inline constexpr ImU32 kStatusRenamed = IM_COL32(47, 129, 247, 255);
inline constexpr ImU32 kStatusSpecial = IM_COL32(166, 91, 216, 255);
inline constexpr ImU32 kStatusConflict = IM_COL32(255, 123, 114, 255);
inline constexpr ImU32 kHistoryConflict = IM_COL32(220, 38, 38, 255);
inline constexpr ImU32 kStatusPushed = IM_COL32(246, 248, 250, 255);
inline constexpr ImU32 kStatusUnpushed = IM_COL32(219, 109, 40, 255);
inline constexpr ImU32 kCommitId = IM_COL32(47, 129, 247, 255);
inline constexpr ImU32 kWorkingCommitId = IM_COL32(100, 181, 246, 255);
inline constexpr std::array<ImU32, 8> kLaneColors{
    IM_COL32(47, 129, 247, 255), IM_COL32(166, 91, 216, 255), IM_COL32(46, 160, 67, 255),
    IM_COL32(210, 153, 34, 255), IM_COL32(248, 81, 73, 255), IM_COL32(57, 197, 187, 255),
    IM_COL32(219, 109, 40, 255), IM_COL32(163, 113, 247, 255)};

inline constexpr std::array<const char*, 2> kDiffViewChoices{"Unified", "Side by Side"};
inline constexpr std::array<const char*, 3> kDiffWhitespaceChoices{
    "Normal", "Ignore Whitespace", "Ignore All Whitespace"};
inline constexpr std::array kDiffContextChoices{0, 1, 3, 5, 10, 25, -1};
inline constexpr std::array<const char*, 7> kDiffContextLabels{
    "0 lines", "1 line", "3 lines", "5 lines", "10 lines", "25 lines", "Full"};

#ifdef IMGUI_BUILD_TESTING
extern Application* test_application;
bool CaptureFramebuffer(
    ImGuiID viewport_id, int x, int y, int width, int height, unsigned int* pixels, void* user_data);
#endif

float FontPx(float value);
std::string IconLabel(std::string_view icon, std::string_view label);
bool ActionMenuItem(std::string_view icon, std::string_view label, const char* shortcut = nullptr,
    bool enabled = true);
bool ActionButton(std::string_view icon, std::string_view label, const ImVec2& size = {});
void IdCopyMenuItems(std::string_view name, std::string_view id, std::size_t unique_length);
bool DangerButton(const char* label, const ImVec2& size = {});
std::string LimitedLines(std::string_view text, std::size_t maximum);
std::string FormatTimestamp(std::int64_t timestamp);
void LoadUiFont();
ImFont* DiffFont();
ImU32 StatusColor(git_delta_t status);
const TextEditor::Language* DiffLanguage(const std::string& path);

template <std::size_t Size>
bool DiffCombo(const char* label, int& index, const std::array<const char*, Size>& choices)
{
    bool changed = false;
    const bool open = ImGui::BeginCombo(label, choices[static_cast<std::size_t>(index)]);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const float wheel = ImGui::GetIO().MouseWheel;
    if (hovered && wheel != 0.0f)
    {
        index = (index + (wheel < 0.0f ? 1 : static_cast<int>(Size) - 1)) % static_cast<int>(Size);
        changed = true;
    }
    if (open)
    {
        for (int choice = 0; choice < static_cast<int>(Size); ++choice)
        {
            const bool selected = choice == index;
            if (ImGui::Selectable(choices[static_cast<std::size_t>(choice)], selected))
            {
                index = choice;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

int WhitespaceModeIndex(DiffWhitespaceMode mode);
DiffWhitespaceMode WhitespaceModeFromIndex(int index);
int ContextLineChoiceIndex(int context_lines);
bool IsImagePath(const std::string& path);
bool IsSymlinkMode(unsigned int mode);
bool IsSubmoduleMode(unsigned int mode);
std::string FormatFileMode(unsigned int mode);
const char* ModeKind(unsigned int mode);
ImU32 BookmarkBadgeColor(std::string_view name, const std::vector<NamedRef>& refs);
ImU32 RefBadgeColor(const NamedRef& ref, const std::vector<NamedRef>& refs);
std::string ReferenceLabel(const NamedRef& ref);
std::pair<std::string, std::size_t> ReferenceBadgeLabel(
    const NamedRef& ref, const std::vector<NamedRef>& refs);
const Remote* DefaultRemote(const RepoSnapshot& snapshot);
const NamedRef* BookmarkAt(const RepoSnapshot& snapshot, const std::string& revision);
std::string RemoteForBookmark(const RepoSnapshot& snapshot, std::string_view bookmark);
std::string RefRemotes(const std::vector<NamedRef>& refs, std::string_view name, gg_named_ref_kind kind);
void DrawBadge(ImDrawList* draw, ImVec2& cursor, float center_y, std::string_view label, ImU32 color,
    std::size_t dimmed_prefix = 0);
void DrawElidedText(ImDrawList* draw, ImVec2 position, float maximum_x, std::string_view text, ImU32 color);
bool DrawTextWithin(ImDrawList* draw, ImVec2 position, float maximum_x, std::string_view text, ImU32 color);
void DrawElidedBadge(ImDrawList* draw, ImVec2 cursor, float center_y, float maximum_x, std::string_view label,
    ImU32 color, std::size_t dimmed_prefix = 0);
bool BadgedSelectable(std::string_view label, bool selected, float height, ImU32 color,
    std::string_view suffix = {}, bool* out_elided = nullptr);
float DrawHighlightedId(
    ImDrawList* draw, ImVec2 position, std::string_view id, std::size_t unique_length, ImU32 prefix_color);
bool DrawHighlightedIdWithin(ImDrawList* draw, ImVec2 position, float maximum_x, std::string_view id,
    std::size_t unique_length, ImU32 prefix_color);
void TextHighlightedId(std::string_view id, std::size_t unique_length, ImU32 prefix_color);
bool HighlightedIdButton(std::string_view id, std::size_t unique_length, ImU32 prefix_color);
void TextLabelledId(std::string_view label, std::string_view id, std::size_t unique_length, ImU32 prefix_color);
void DialogInput(const char* label, const char* hint, std::string* value, bool focus = false,
    ImGuiInputTextFlags flags = 0);
void DialogMultiline(const char* label, std::string* value, float height, bool focus = false);
ImU32 CommitIdColor(bool working);
std::vector<std::string> SplitLines(std::string_view text);
bool HasText(std::string_view value);
const char* DeltaName(git_delta_t status);
bool ContainsInsensitive(std::string_view haystack, std::string_view needle);
std::string FileUrl(const std::string& path);
std::string RepositoryName(const std::string& root);
const std::string& CurrentCommit(const RepoSnapshot& snapshot);
const Revision* ResolveSnapshotRevision(const RepoSnapshot& snapshot, std::string_view identifier,
    std::span<const Revision> revisions = {});
const Revision* RebaseBranchRoot(
    const RepoSnapshot& snapshot, std::string_view source, std::string_view destination,
    std::span<const Revision> revisions = {});
std::string LimitedFragment(std::string_view text, std::size_t maximum);

} // namespace ApplicationInternal
} // namespace Ggui

// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <TextDiff.h>
#include <IconsMaterialSymbols.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

namespace
{
class DiffViewer : public TextDiff
{
public:
    void PreserveScrollY(float y)
    {
        ensureCursorIsVisible = false;
        scrollToLineNumber = -1;
        ImGui::SetNextWindowScroll(ImVec2(-1.0f, y));
    }
};

struct DiffGap
{
    int row = 0;
    int old_line = 0;
    int new_line = 0;
    int count = 0;
};

struct DiffRange
{
    int old_line = 0;
    int new_line = 0;
    int count = 0;
};

struct DiffView
{
    std::string before;
    std::string after;
    std::vector<DiffLine> lines;
    std::vector<DiffGap> gaps;
};

std::vector<std::string_view> TextLines(const std::string& text)
{
    std::vector<std::string_view> result;
    std::size_t begin = 0;
    for (std::size_t end = 0; (end = text.find('\n', begin)) != std::string::npos; begin = end + 1)
        result.emplace_back(text.data() + begin, end - begin);
    result.emplace_back(text.data() + begin, text.size() - begin);
    return result;
}

DiffView BuildDiffView(const DiffResult& source, const std::vector<DiffRange>& revealed)
{
    DiffView result;
    if ((source.full_before.empty() && source.full_after.empty()) || source.lines.empty())
    {
        result.before = source.before;
        result.after = source.after;
        result.lines = source.lines;
        return result;
    }

    const std::vector<std::string_view> before = TextLines(source.full_before);
    const std::vector<std::string_view> after = TextLines(source.full_after);
    const int before_count = source.full_before.empty() ? 0
        : static_cast<int>(before.size() - (source.full_before.ends_with('\n') ? 1U : 0U));
    const int after_count = source.full_after.empty() ? 0
        : static_cast<int>(after.size() - (source.full_after.ends_with('\n') ? 1U : 0U));
    bool first_before = true;
    bool first_after = true;
    const auto append = [](std::string& text, bool& first, std::string_view line) {
        if (!first)
            text += '\n';
        text += line;
        first = false;
    };
    const auto line_at = [](const std::vector<std::string_view>& lines, int line) {
        return line >= 0 && line < static_cast<int>(lines.size())
            ? lines[static_cast<std::size_t>(line)] : std::string_view{};
    };
    const auto append_context = [&](int old_line, int new_line, int hunk = -1) {
        const std::string_view text = old_line >= 0 ? line_at(before, old_line) : line_at(after, new_line);
        append(result.before, first_before, text);
        append(result.after, first_after, text);
        result.lines.push_back({DiffLineKind::Context, old_line, new_line, hunk});
    };
    const auto append_omission = [&](int old_line, int new_line, int count) {
        const auto is_revealed = [&](int offset) {
            return std::ranges::any_of(revealed, [&](const DiffRange& range) {
                const int range_offset = old_line + offset - range.old_line;
                return range_offset >= 0 && range_offset < range.count
                    && new_line + offset == range.new_line + range_offset;
            });
        };
        for (int offset = 0; offset < count;)
        {
            if (is_revealed(offset))
            {
                append_context(old_line + offset, new_line + offset);
                ++offset;
                continue;
            }
            const int first = offset;
            while (offset < count && !is_revealed(offset))
                ++offset;
            result.gaps.push_back({static_cast<int>(result.lines.size()), old_line + first,
                new_line + first, offset - first});
            append(result.before, first_before, {});
            append(result.after, first_after, {});
            result.lines.push_back({});
        }
    };

    int next_old = 0;
    int next_new = 0;
    for (const DiffLine& line : source.lines)
    {
        if (line.old_line < 0 && line.new_line < 0)
            continue;
        int omitted = -1;
        if (line.old_line >= 0)
            omitted = std::max(line.old_line - next_old, 0);
        if (line.new_line >= 0)
        {
            const int new_omitted = std::max(line.new_line - next_new, 0);
            omitted = omitted < 0 ? new_omitted : std::min(omitted, new_omitted);
        }
        omitted = std::min({std::max(omitted, 0), before_count - next_old, after_count - next_new});
        if (omitted > 0)
        {
            append_omission(next_old, next_new, omitted);
            next_old += omitted;
            next_new += omitted;
        }

        if (line.kind == DiffLineKind::Context)
            append_context(line.old_line, line.new_line, line.hunk);
        else if (line.kind == DiffLineKind::Deletion)
        {
            append(result.before, first_before, line_at(before, line.old_line));
            result.lines.push_back(line);
        }
        else
        {
            append(result.after, first_after, line_at(after, line.new_line));
            result.lines.push_back(line);
        }
        if (line.old_line >= 0)
            next_old = line.old_line + 1;
        if (line.new_line >= 0)
            next_new = line.new_line + 1;
    }

    const int trailing = std::min(before_count - next_old, after_count - next_new);
    if (trailing > 0)
        append_omission(next_old, next_new, trailing);
    if (source.full_before.ends_with('\n') && source.full_after.ends_with('\n'))
    {
        append(result.before, first_before, {});
        append(result.after, first_after, {});
        result.lines.push_back({});
    }
    return result;
}
}

void Application::RenderDiff()
{
    if (!ImGui::Begin("Diff", &_show_diff))
    {
        ImGui::End();
        return;
    }

    // Working-copy comparison control
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

    // Empty and loading states
    if (_diff_loading && _diff.revision.empty())
    {
        const std::array<const char*, 4> spinner{"◐", "◓", "◑", "◒"};
        const int frame = static_cast<int>(ImGui::GetTime() * 8.0) & 3;
        ImGui::Text("%s Loading...", spinner[static_cast<std::size_t>(frame)]);
        ImGui::End();
        return;
    }
    if (_diff.revision.empty())
    {
        ImGui::TextWrapped("Select a change or file to inspect its diff.");
        ImGui::End();
        return;
    }
    if (_diff.path.empty())
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Selected change is empty.");
        ImGui::End();
        return;
    }

    const git_delta_t status = _diff.selected_status;
    const bool plain = status == GIT_DELTA_ADDED || status == GIT_DELTA_UNTRACKED || status == GIT_DELTA_DELETED;

    // Diff option helpers
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

    // Diff toolbar
    ImGui::SetNextItemWidth(combo_width("Side by Side"));
    int view_index = _diff_side_by_side ? 1 : 0;
    if (DiffCombo("View", view_index, kDiffViewChoices))
        _diff_side_by_side = view_index == 1;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(combo_width("Ignore All Whitespace"));
    int whitespace_index = WhitespaceModeIndex(_diff_whitespace_mode);
    if (DiffCombo("##whitespace mode", whitespace_index, kDiffWhitespaceChoices))
        reload(WhitespaceModeFromIndex(whitespace_index), _diff_context_lines);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Choose how whitespace-only changes are displayed.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(combo_width("25 lines"));
    int context_index = ContextLineChoiceIndex(_diff_context_lines);
    if (DiffCombo("##context lines", context_index, kDiffContextLabels))
        reload(_diff_whitespace_mode, kDiffContextChoices[static_cast<std::size_t>(context_index)]);
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

    // File mode details
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

    // Unsupported diff previews
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

    // Persistent diff and file viewers
    static DiffViewer diff;
    static TextEditor editor;
    static std::string loaded_before;
    static std::string loaded_after;
    static std::string loaded_full_before;
    static std::string loaded_full_after;
    static std::string loaded_path;
    static std::string loaded_revision;
    static std::string loaded_compare_to;
    static DiffWhitespaceMode loaded_whitespace = DiffWhitespaceMode::Normal;
    static int loaded_context_lines = 3;
    static bool loaded_plain = false;
    static bool dark_palette = !_dark_theme;
    static bool viewer_dirty = false;
    static float reveal_scroll_y = -1.0f;
    static std::vector<DiffRange> revealed_context;
    static std::vector<DiffLine> viewer_lines;
    static std::vector<DiffGap> viewer_gaps;

    // Viewer content
    const bool visit_changed = loaded_path != _diff.path || loaded_revision != _diff.revision
        || loaded_compare_to != _diff.compare_to;
    const bool options_changed = loaded_whitespace != _diff_whitespace_mode
        || loaded_context_lines != _diff_context_lines;
    if (loaded_path != _selected_file && !revealed_context.empty())
    {
        revealed_context.clear();
        viewer_dirty = true;
    }
    if (loaded_before != _diff.before || loaded_after != _diff.after || loaded_path != _diff.path
        || loaded_full_before != _diff.full_before || loaded_full_after != _diff.full_after
        || loaded_revision != _diff.revision || loaded_compare_to != _diff.compare_to
        || loaded_whitespace != _diff_whitespace_mode || loaded_context_lines != _diff_context_lines
        || loaded_plain != plain || viewer_dirty)
    {
        if (visit_changed || options_changed || loaded_plain != plain)
            revealed_context.clear();
        viewer_dirty = false;
        loaded_before = _diff.before;
        loaded_after = _diff.after;
        loaded_full_before = _diff.full_before;
        loaded_full_after = _diff.full_after;
        loaded_path = _diff.path;
        loaded_revision = _diff.revision;
        loaded_compare_to = _diff.compare_to;
        loaded_whitespace = _diff_whitespace_mode;
        loaded_context_lines = _diff_context_lines;
        loaded_plain = plain;
        if (plain)
        {
            viewer_lines = _diff.lines;
            viewer_gaps.clear();
            editor.SetLanguage(DiffLanguage(_diff.path));
            editor.SetText(status == GIT_DELTA_DELETED ? loaded_before : loaded_after);
            editor.SetReadOnlyEnabled(true);
        }
        else
        {
            DiffView view = BuildDiffView(_diff, revealed_context);
            viewer_lines = std::move(view.lines);
            viewer_gaps = std::move(view.gaps);
            diff.SetLanguage(DiffLanguage(_diff.path));
            diff.SetText(view.before, view.after);
            std::vector<std::pair<int, int>> line_numbers;
            line_numbers.reserve(viewer_lines.size());
            for (const DiffLine& line : viewer_lines)
                line_numbers.emplace_back(line.old_line + 1, line.new_line + 1);
            diff.SetLineNumbers(line_numbers);
            if (reveal_scroll_y >= 0.0f)
            {
                diff.PreserveScrollY(reveal_scroll_y);
                reveal_scroll_y = -1.0f;
            }
        }
    }

    // Viewer palette
    if (dark_palette != _dark_theme)
    {
        dark_palette = _dark_theme;
        TextEditor::Palette palette = _dark_theme ? TextEditor::GetDarkPalette() : TextEditor::GetLightPalette();
        palette[static_cast<std::size_t>(TextEditor::Color::selection)] =
            ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
        diff.SetPalette(palette);
        editor.SetPalette(palette);
        diff.SetColors(_dark_theme ? IM_COL32(46, 160, 67, 55) : IM_COL32(46, 160, 67, 38),
            _dark_theme ? IM_COL32(248, 81, 73, 55) : IM_COL32(248, 81, 73, 38));
    }

    // Diff context-menu state
    static int context_row = -1;
    static bool context_has_selection = false;
    static std::vector<DiffLine> context_line;
    static std::vector<DiffLine> context_region;
    static std::vector<DiffLine> context_hunk;
    static std::string context_revision;
    static std::string context_path;

    // Diff line and hunk context menu
    const auto render_move_context = [&](auto& view, bool side_by_side) {
        ImGuiWindow* view_window = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
        IM_ASSERT(view_window->ChildId == ImGui::GetItemID());
        const ImGuiIO& io = ImGui::GetIO();
        if (side_by_side && view.AnyCursorHasSelection()
            && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
            && io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_C))
            view.Copy();
        const float line_height = std::max(view.GetLineHeight(), 1.0f);
        const float content_y = ImGui::GetItemRectMin().y + ImGui::GetStyle().WindowPadding.y;
        const auto mouse_row = [&] {
            return std::clamp(view.GetFirstVisibleLine()
                    + static_cast<int>(std::max(ImGui::GetMousePos().y - content_y, 0.0f) / line_height),
                0, std::max(0, static_cast<int>(viewer_lines.size()) - 1));
        };
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            context_row = mouse_row();
            context_revision = _diff.revision;
            context_path = _diff.path;
            context_line.clear();
            context_region.clear();
            context_hunk.clear();
            context_has_selection = false;
            if (context_row >= 0 && context_row < static_cast<int>(viewer_lines.size()))
            {
                const DiffLine& clicked = viewer_lines[static_cast<std::size_t>(context_row)];
                if (clicked.kind != DiffLineKind::Context)
                    context_line.push_back(clicked);

                int first = context_row;
                int last = context_row;
                if (view.AnyCursorHasSelection())
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
                    last = static_cast<int>(viewer_lines.size()) - 1;
                }
                const int hunk = clicked.hunk;
                for (int row = std::max(first, 0);
                     row <= last && row < static_cast<int>(viewer_lines.size()); ++row)
                {
                    const DiffLine& line = viewer_lines[static_cast<std::size_t>(row)];
                    if (line.kind != DiffLineKind::Context
                        && (context_has_selection || (hunk >= 0 && line.hunk == hunk)))
                        context_region.push_back(line);
                }
                if (hunk >= 0)
                    std::ranges::copy_if(viewer_lines, std::back_inserter(context_hunk), [&](const DiffLine& line) {
                        return line.kind != DiffLineKind::Context && line.hunk == hunk;
                    });
            }
            ImGui::OpenPopup("Diff line context");
        }

        if (!ImGui::BeginPopup("Diff line context"))
            return;
        ImGui::BeginDisabled(!view.AnyCursorHasSelection());
        if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy", "Ctrl+C"))
            view.Copy();
        ImGui::EndDisabled();
        ImGui::Separator();
        const auto [parent, child] = AdjacentRevisions(_diff.revision);
        const auto source_revision = std::ranges::find(_history_revisions, _diff.revision, &Revision::oid);
        const auto child_revision = std::ranges::find(_history_revisions, child, &Revision::oid);
        const bool linear_source = source_revision != _history_revisions.end()
            && source_revision->parents.size() == 1;
        const bool linear_child = child_revision != _history_revisions.end()
            && child_revision->parents.size() == 1 && child_revision->parents.front() == _diff.revision;
        const bool conflicted = std::ranges::any_of(_diff.files, [&](const StatusEntry& file) {
            return file.path == _diff.path && file.conflicted;
        });
        const bool stale = context_revision != _diff.revision || context_path != _diff.path;
        const bool unsupported = stale || !_compare_to.empty() || !_active_operation.empty()
            || conflicted || _diff.selected_status == GIT_DELTA_RENAMED
            || _diff.selected_status == GIT_DELTA_COPIED || _diff.selected_status == GIT_DELTA_TYPECHANGE
            || IsSymlinkMode(_diff.old_mode)
            || IsSymlinkMode(_diff.new_mode) || IsSubmoduleMode(_diff.old_mode) || IsSubmoduleMode(_diff.new_mode)
            || (_diff.old_mode != 0 && _diff.new_mode != 0 && _diff.old_mode != _diff.new_mode);
        const bool move_unsupported = unsupported || !linear_source;
        if (!move_unsupported && !context_line.empty() && (linear_child || !parent.empty()))
        {
            const float line_height = std::max(view.GetLineHeight(), 1.0f);
            const float line_spacing = std::max(line_height - ImGui::GetTextLineHeight(), 0.0f);
            const float line_y = view_window->DC.CursorStartPos.y + context_row * line_height - line_spacing * 0.5f;
            ImVec2 highlight_minimum(view_window->InnerClipRect.Min.x, line_y);
            ImVec2 highlight_maximum(view_window->InnerClipRect.Max.x, line_y + line_height);
            if (side_by_side)
            {
                const float split_x = view_window->DC.CursorStartPos.x + view_window->Size.x * 0.5f;
                if (context_line.front().kind == DiffLineKind::Deletion)
                    highlight_maximum.x = split_x;
                else
                    highlight_minimum.x = split_x;
            }
            view_window->DrawList->PushClipRect(view_window->InnerClipRect.Min, view_window->InnerClipRect.Max, true);
            view_window->DrawList->AddRectFilled(
                highlight_minimum, highlight_maximum, ImGui::GetColorU32(ImGuiCol_NavHighlight, 0.28f));
            view_window->DrawList->PopClipRect();
        }
        const auto move = [&](std::string_view icon, const char* label, const std::string& destination,
                              const std::vector<DiffLine>& lines, bool target_valid) {
            ImGui::BeginDisabled(move_unsupported || !target_valid || lines.empty());
            if (ActionMenuItem(icon, label))
                QueueCommands({MoveDiffLines{_diff.revision, destination, _diff.path, lines}},
                    {_diff.revision, destination},
                    "Moving these lines will rewrite a locked source or destination commit.");
            ImGui::EndDisabled();
        };
        const auto& move_lines = context_has_selection ? context_region : context_line;
        move(ICON_MS_ARROW_UPWARD, context_has_selection ? "Move lines to child" : "Move line to child",
            child, move_lines, linear_child);
        move(ICON_MS_ARROW_DOWNWARD, context_has_selection ? "Move lines to parent" : "Move line to parent",
            parent, move_lines, !parent.empty());
        if (!context_has_selection)
        {
            ImGui::Separator();
            move(ICON_MS_ARROW_UPWARD, "Move hunk to child", child, context_region, linear_child);
            move(ICON_MS_ARROW_DOWNWARD, "Move hunk to parent", parent, context_region, !parent.empty());
        }
        if (!stale && _snapshot != nullptr)
        {
            ImGui::Separator();
            if (_diff.revision == _snapshot->working_copy)
            {
                ImGui::BeginDisabled(unsupported || context_line.empty());
                if (ActionMenuItem(ICON_MS_RESTORE, "Revert line"))
                    QueueCommands({RevertDiffLines{_diff.revision, _diff.path, context_line}}, {_diff.revision},
                        "Reverting these lines will rewrite the locked working-copy commit.");
                ImGui::EndDisabled();
            }
            ImGui::BeginDisabled(unsupported || context_hunk.empty() || _snapshot->working_copy.empty());
            if (ActionMenuItem(ICON_MS_RESTORE, "Revert hunk"))
                QueueCommands({RevertFile{_diff.revision, _diff.path, _diff.path, context_hunk}}, {"@"},
                    "Reverting this hunk will rewrite the locked working-copy commit.");
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    };

    // Diff or plain-file viewer
    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (!plain)
        diff.SetSideBySideMode(_diff_side_by_side);
    int hovered_gap = -1;
    int clicked_gap = -1;
    bool reveal_all = false;
    ImGui::PushFont(DiffFont(), 0.0f);
    if (plain)
        editor.Render("##file view", available, true);
    else
    {
        diff.Render("##diff view", available, true);
        ImGuiWindow* view_window = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
        IM_ASSERT(view_window->ChildId == ImGui::GetItemID());
        const float line_height = std::max(diff.GetLineHeight(), 1.0f);
        const int edge = std::max(_diff_context_lines, 1);
        const bool reveal_all_modifier = ImGui::GetIO().KeyShift;
        const bool view_hovered = ImGui::IsItemHovered();
        ImDrawList* draw = view_window->DrawList;
        draw->PushClipRect(view_window->InnerClipRect.Min, view_window->InnerClipRect.Max, true);
        for (int index = 0; index < static_cast<int>(viewer_gaps.size()); ++index)
        {
            const DiffGap& gap = viewer_gaps[static_cast<std::size_t>(index)];
            const float y = view_window->DC.CursorStartPos.y + gap.row * line_height;
            if (y + line_height < view_window->InnerClipRect.Min.y || y > view_window->InnerClipRect.Max.y)
                continue;

            const ImU32 muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            const std::string info = std::to_string(gap.count)
                + (gap.count == 1 ? " line hidden" : " lines hidden");
            const int reveal_count = reveal_all_modifier ? gap.count : std::min(gap.count, edge * 2);
            const std::string action = "Reveal " + std::to_string(reveal_count);
            const ImVec2 info_size = ImGui::CalcTextSize(info.c_str());
            const ImVec2 action_size = ImGui::CalcTextSize(action.c_str());
            const ImVec2 padding = ImGui::GetStyle().FramePadding;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float button_width = action_size.x + padding.x * 2.0f;
            const float total_width = info_size.x + spacing + button_width;
            const float left = view_window->InnerClipRect.GetCenter().x - total_width * 0.5f;
            const ImRect button(ImVec2(left + info_size.x + spacing, y + 1.0f),
                ImVec2(left + total_width, y + line_height - 1.0f));
            const bool hovered = view_hovered && button.Contains(ImGui::GetMousePos());
            draw->AddText(ImVec2(left, y + (line_height - info_size.y) * 0.5f), muted, info.c_str());
            draw->AddRectFilled(button.Min, button.Max,
                ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button),
                ImGui::GetStyle().FrameRounding);
            draw->AddText(ImVec2(button.Min.x + padding.x, y + (line_height - action_size.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), action.c_str());
            if (hovered)
            {
                hovered_gap = index;
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    clicked_gap = index;
                    reveal_all = reveal_all_modifier;
                    reveal_scroll_y = view_window->Scroll.y;
                }
            }
        }
        draw->PopClipRect();
    }
    ImGui::PopFont();

    // Viewer context menu
    if (plain)
        render_move_context(editor, false);
    else
        render_move_context(diff, _diff_side_by_side);
    if (hovered_gap >= 0)
        ImGui::SetTooltip("Reveal surrounding context. Hold Shift while clicking to reveal the entire section.");
    if (clicked_gap >= 0)
    {
        const DiffGap gap = viewer_gaps[static_cast<std::size_t>(clicked_gap)];
        const int edge = std::max(_diff_context_lines, 1);
        const int first_end = reveal_all ? gap.count : std::min(edge, gap.count);
        const int last_begin = reveal_all ? 0 : std::max(first_end, gap.count - edge);
        if (reveal_all)
            revealed_context.push_back({gap.old_line, gap.new_line, gap.count});
        else if (first_end > 0)
            revealed_context.push_back({gap.old_line, gap.new_line, first_end});
        if (!reveal_all && last_begin < gap.count)
            revealed_context.push_back({gap.old_line + last_begin, gap.new_line + last_begin,
                gap.count - last_begin});
        viewer_dirty = true;
    }
    ImGui::End();
}

} // namespace Ggui

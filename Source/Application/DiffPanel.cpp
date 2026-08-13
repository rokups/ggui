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
    static std::string loaded_revision;
    static std::string loaded_compare_to;
    static DiffWhitespaceMode loaded_whitespace = DiffWhitespaceMode::Normal;
    static int loaded_context_lines = 3;
    static bool loaded_plain = false;
    static bool dark_palette = !_dark_theme;
    if (loaded_before != _diff.before || loaded_after != _diff.after || loaded_path != _diff.path
        || loaded_revision != _diff.revision || loaded_compare_to != _diff.compare_to
        || loaded_whitespace != _diff_whitespace_mode || loaded_context_lines != _diff_context_lines
        || loaded_plain != plain)
    {
        loaded_before = _diff.before;
        loaded_after = _diff.after;
        loaded_path = _diff.path;
        loaded_revision = _diff.revision;
        loaded_compare_to = _diff.compare_to;
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
            std::vector<std::pair<int, int>> line_numbers;
            line_numbers.reserve(_diff.lines.size());
            for (const DiffLine& line : _diff.lines)
                line_numbers.emplace_back(line.old_line + 1, line.new_line + 1);
            diff.SetLineNumbers(line_numbers);
        }
    }
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

    static int context_row = -1;
    static bool context_has_selection = false;
    static std::vector<DiffLine> context_line;
    static std::vector<DiffLine> context_region;
    static std::vector<DiffLine> context_hunk;
    static std::string context_revision;
    static std::string context_path;
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
                0, std::max(0, static_cast<int>(_diff.lines.size()) - 1));
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
            if (context_row >= 0 && context_row < static_cast<int>(_diff.lines.size()))
            {
                const DiffLine& clicked = _diff.lines[static_cast<std::size_t>(context_row)];
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
                if (hunk >= 0)
                    std::ranges::copy_if(_diff.lines, std::back_inserter(context_hunk), [&](const DiffLine& line) {
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

    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (!plain)
        diff.SetSideBySideMode(_diff_side_by_side);
    ImGui::PushFont(DiffFont(), 0.0f);
    if (plain)
        editor.Render("##file view", available, true);
    else
        diff.Render("##diff view", available, true);
    ImGui::PopFont();
    if (plain)
        render_move_context(editor, false);
    else
        render_move_context(diff, _diff_side_by_side);
    ImGui::End();
}

} // namespace Ggui

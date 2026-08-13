// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>

namespace Ggui
{
using namespace ApplicationInternal;

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
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##changes filter", "Filter changed files", &_changes_filter);
    ImGui::BeginChild("file list");
    const ImGuiIO& io = ImGui::GetIO();
    const bool keyboard_navigation = !io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper
        && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const ImGuiID navigation_owner = ImGui::GetID("changes arrow navigation");
    if (keyboard_navigation)
    {
        ImGui::SetKeyOwner(ImGuiKey_UpArrow, navigation_owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_DownArrow, navigation_owner, ImGuiInputFlags_LockThisFrame);
    }
    const bool navigate_up = keyboard_navigation
        && ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, navigation_owner);
    const bool navigate_down = keyboard_navigation
        && ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, navigation_owner);
    const std::string previous_file = _selected_file;
    if (navigate_up || navigate_down)
        NavigateChangedFile(navigate_down ? 1 : -1);
    const bool selection_navigated = previous_file != _selected_file;
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
        if (selection_navigated && file.path == _selected_file)
            ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
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
            ImGui::BeginDisabled(
                actions_locked || comparison_active || _diff_loading || _snapshot->working_copy.empty());
            if (ActionMenuItem(ICON_MS_RESTORE, "Revert"))
                QueueCommands({RevertFile{_diff.revision, file.old_path, file.path, {}}}, {"@"},
                    "Reverting this file will rewrite the locked working-copy commit.");
            ImGui::EndDisabled();
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
                if (ActionMenuItem(ICON_MS_ADD, "Track"))
                    QueueCommands({TrackPaths{{file.path}}}, {"@"},
                        "Tracking this file will rewrite the locked working-copy commit.");
                if (ActionMenuItem(ICON_MS_DELETE, "Untrack"))
                    QueueCommands({UntrackPaths{{file.path}}}, {"@"},
                        "Untracking this file will rewrite the locked working-copy commit.");
                ImGui::Separator();
                if (ActionMenuItem(ICON_MS_DELETE, "Delete file", nullptr, file_exists))
                    QueueCommands({DeleteFile{file.path}}, {"@"},
                        "Deleting this file will rewrite the locked working-copy commit.");
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
    if (ImGui::IsItemHovered() && !revision->author_email.empty())
        ImGui::SetTooltip("%s", revision->author_email.c_str());
    if (ImGui::BeginPopupContextItem("author context"))
    {
        if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy author", nullptr, !revision->author.empty()))
            ImGui::SetClipboardText(revision->author.c_str());
        if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy email", nullptr, !revision->author_email.empty()))
            ImGui::SetClipboardText(revision->author_email.c_str());
        ImGui::Separator();
        if (ActionMenuItem(ICON_MS_EDIT, "Edit author", nullptr, _active_operation.empty()))
        {
            OpenDialog(Dialog::Metaedit);
            _input_flag = true;
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 12.0f);
    const std::string date = FormatTimestamp(revision->timestamp);
    ImGui::Text("%s%s", date.c_str(), revision->pushed ? "  locked" : "");
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::BeginGroup();
    TextLabelledId("Commit ", revision->oid, RevisionPrefix(revision->oid), CommitIdColor(revision->working_copy));
    ImGui::EndGroup();
    if (ImGui::BeginPopupContextItem("commit ID context"))
    {
        IdCopyMenuItems("commit ID", revision->oid, RevisionPrefix(revision->oid));
        for (std::size_t index = 0; index < revision->aliases.size(); ++index)
            IdCopyMenuItems("alias " + std::to_string(index + 1), revision->aliases[index],
                RevisionPrefix(revision->aliases[index]));
        ImGui::EndPopup();
    }
    if (!revision->aliases.empty())
    {
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextDisabled("%zu alias%s", revision->aliases.size(), revision->aliases.size() == 1 ? "" : "es");
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            for (const std::string& alias : revision->aliases)
                ImGui::TextUnformatted(alias.c_str());
            ImGui::EndTooltip();
        }
    }
    const float button_height = ImGui::GetFrameHeight();
    const float message_height = std::max(46.0f, ImGui::GetContentRegionAvail().y - button_height - 12.0f);
    if (ImGui::InputTextMultiline("##commit message", &_change_info_message, ImVec2(-1.0f, message_height)))
        _change_info_dirty = true;
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

} // namespace Ggui

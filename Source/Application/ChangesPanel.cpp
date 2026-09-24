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

    // Comparison controls and file count
    const bool actions_locked = !_active_operation.empty();
    const bool comparison_active = !_compare_to.empty();
    bool comparing = comparison_active && !_file_comparison;
    ImGui::BeginDisabled(_selected_revision.empty() || IsWorkingTreeRevision(_selected_revision)
        || CurrentCommit(*_snapshot).empty()
        || (!comparison_active && _selected_revision == CurrentCommit(*_snapshot)));
    if (ImGui::Checkbox("Compare with @", &comparing))
        ToggleComparison(false);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Compare the entire selected change with the active commit (@).");
    if (comparing)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s → @ %s", ShortId(_selected_revision).c_str(), ShortId(_compare_to).c_str());
    }
    ImGui::SameLine();
    const auto [parent, child] = AdjacentRevisions(_diff.revision);
    const bool working_tree_diff = IsWorkingTreeRevision(_diff.revision);
    const bool active_commit_diff = _diff.revision == CurrentCommit(*_snapshot);
    const std::string move_parent = working_tree_diff ? CurrentCommit(*_snapshot) : parent;
    const std::string move_child = active_commit_diff
        ? "working-tree:" + std::to_string(_snapshot->repository_generation) : child;
    // Reloads of the listed change (such as working-tree updates) keep the
    // current list and count on screen.
    if (_diff_loading && !_pending_revision.empty() && !SameDiffRevision(_diff.revision, _selected_revision))
        ImGui::TextDisabled("Loading selected change...");
    else if (IsWorkingTreeRevision(_selected_revision) && !working_tree_diff && !_diff_loading)
        ImGui::TextDisabled("Working tree not scanned. Refresh (F5) to load its changes.");
    else if (_diff.files.empty())
        ImGui::TextDisabled(comparing ? "The comparison has no differences." : "Selected change is empty.");
    else
        ImGui::TextDisabled("%zu %s file%s", _diff.files.size(), comparing ? "differing" : "changed",
            _diff.files.size() == 1 ? "" : "s");

    // File filter
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##changes filter", "Filter changed files", &_changes_filter);

    // Changed file list
    ImGui::BeginChild("file list");

    // Keyboard file navigation
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

    // File rows
    const ImVec2 item_spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0f));
    for (const StatusEntry& file : _diff.files)
    {
        const git_delta_t display_status = file.conflicted ? GIT_DELTA_CONFLICTED : file.status;
        const std::string status = DeltaName(display_status);
        if (!FileMatchesFilter(file))
            continue;
        ImGui::PushID(&file);
        const std::string label = status + "  " + file.path;
        const std::string item_id = "###" + label;
        const ImU32 accent = StatusColor(display_status);
        const bool selected = ImGui::Selectable(item_id.c_str(), file.path == _selected_file, 0, ImVec2(0.0f, 26.0f));
#ifdef IMGUI_ENABLE_TEST_ENGINE
        {
            ImGuiContext& g = *GImGui;
            IMGUI_TEST_ENGINE_ITEM_INFO(ImGui::GetItemID(), label.c_str(), ImGuiItemStatusFlags_None);
        }
#endif
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
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            if (file.conflicted)
                OpenConflictInMergeTool(file.path);
            else
                OpenFileInEditor(file.path);
        }

        // File drag source
        if (!actions_locked && !comparison_active && !IsWorkingTreeRevision(_diff.revision)
            && ImGui::BeginDragDropSource())
        {
            std::string payload = _diff.revision;
            payload.push_back('\0');
            payload += file.path;
            payload.push_back('\0');
            ImGui::SetDragDropPayload("GGUI_FILE", payload.data(), payload.size());
            ImGui::Text("Move %s", file.path.c_str());
            ImGui::EndDragDropSource();
        }

        // File context menu
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
            if (file.conflicted)
            {
                if (ActionMenuItem(ICON_MS_MERGE, "Resolve with merge tool"))
                    OpenConflictInMergeTool(file.path);
                ImGui::BeginDisabled(_selected_revision != CurrentCommit(*_snapshot));
                if (ActionMenuItem(ICON_MS_CHECK, "Mark current file resolved"))
                    MarkConflictResolved(file.path);
                ImGui::EndDisabled();
                ImGui::Separator();
            }
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
            // Blame needs the file to exist in the selected commit. Added,
            // deleted, and untracked entries only exist on one side of the
            // comparison and would otherwise produce a noisy repository-read
            // error from libgit2. Working-tree files are read from disk and
            // blamed against @, so only deleted files are unavailable there.
            const bool working_tree_file = IsWorkingTreeRevision(_diff.revision);
            const bool blame_available = !_diff.revision.empty() && !file.path.empty()
                && file.status != GIT_DELTA_DELETED && file.status != GIT_DELTA_TYPECHANGE
                && (working_tree_file || (file.status != GIT_DELTA_ADDED && file.status != GIT_DELTA_UNTRACKED))
                && !file.conflicted;
            ImGui::BeginDisabled(!blame_available);
            if (ActionMenuItem(ICON_MS_PERSON, "Blame file"))
                RequestBlame(_diff.revision, file.path);
            ImGui::EndDisabled();
            const std::string external_diff_label = IconLabel(ICON_MS_OPEN_IN_NEW, "External diff");
            if (ImGui::BeginMenu(external_diff_label.c_str(), !IsWorkingTreeRevision(_diff.revision)))
            {
                ImGui::BeginDisabled(_diff.revision == CurrentCommit(*_snapshot) || CurrentCommit(*_snapshot).empty());
                if (ActionMenuItem(ICON_MS_OPEN_IN_NEW, "vs @"))
                    OpenExternalDiff(file.path, CurrentCommit(*_snapshot));
                ImGui::EndDisabled();
                ImGui::BeginDisabled(parent.empty());
                if (ActionMenuItem(ICON_MS_OPEN_IN_NEW, "vs parent"))
                    OpenExternalDiff(file.path, {});
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }
            ImGui::Separator();
            // A commit's file change is reverted by applying its inverse to the
            // working tree; a working-tree file is restored as it is in @.
            ImGui::BeginDisabled(
                actions_locked || comparison_active || _diff_loading || CurrentCommit(*_snapshot).empty()
                    || (working_tree_diff && file.conflicted));
            if (ActionMenuItem(ICON_MS_RESTORE, "Revert"))
            {
                if (working_tree_diff)
                    RequestRevertWorkingFile(file);
                else
                    QueueCommands({RevertFile{_diff.revision, file.old_path, file.path, {}}}, {"@"},
                        "Reverting this file will rewrite the locked active commit.");
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::BeginDisabled(actions_locked || comparison_active || move_child.empty());
            if (ActionMenuItem(ICON_MS_ARROW_UPWARD,
                    active_commit_diff ? "Move to Working tree" : "Move to child"))
                QueueCommands({MoveFiles{_diff.revision, move_child, {file.path}}}, {_diff.revision, move_child},
                    "Moving this file will rewrite a locked source or destination commit.");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(actions_locked || comparison_active || move_parent.empty());
            if (ActionMenuItem(ICON_MS_ARROW_DOWNWARD,
                    working_tree_diff ? "Move to active commit" : "Move to parent"))
                QueueCommands({MoveFiles{_diff.revision, move_parent, {file.path}}}, {_diff.revision, move_parent},
                    "Moving this file will rewrite a locked source or destination commit.");
            ImGui::EndDisabled();
            if (_selected_revision == CurrentCommit(*_snapshot) || IsWorkingTreeRevision(_selected_revision))
            {
                ImGui::Separator();
                ImGui::BeginDisabled(actions_locked || comparison_active);
                if (ActionMenuItem(ICON_MS_ADD, "Track"))
                    QueueCommands({TrackPaths{{file.path}}}, {"@"},
                        "Tracking this file will rewrite the locked active commit.");
                if (ActionMenuItem(ICON_MS_DELETE, "Untrack"))
                    QueueCommands({UntrackPaths{{file.path}}}, {"@"},
                        "Untracking this file will rewrite the locked active commit.");
                ImGui::Separator();
                if (ActionMenuItem(ICON_MS_DELETE, "Delete file", nullptr, file_exists))
                    QueueCommands({DeleteFile{file.path}}, {"@"},
                        "Deleting this file will rewrite the locked active commit.");
                ImGui::EndDisabled();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();

        // Elided file details
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
    const auto revision = std::ranges::find(_history_revisions, _selected_revision, &Revision::oid);
    if (revision == _history_revisions.end())
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

    // Author, date, and commit ID
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
                TextHighlightedId(alias, RevisionPrefix(alias), CommitIdColor(revision->working_copy));
            ImGui::EndTooltip();
        }
    }
    if (!revision->parents.empty())
    {
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextUnformatted(revision->parents.size() == 1 ? "Parent" : "Parents");
        ImGui::PushID("parents");
        for (const std::string& parent : revision->parents)
        {
            ImGui::SameLine(0.0f, 6.0f);
            if (HighlightedIdButton(parent, RevisionPrefix(parent), CommitIdColor(false)))
                RevealRevision(parent);
            if (ImGui::IsItemHovered())
                RenderRevisionTooltip("Select", parent);
            if (ImGui::BeginPopupContextItem())
            {
                IdCopyMenuItems("commit ID", parent, RevisionPrefix(parent));
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
    }

    // Commit message editor
    const float button_height = ImGui::GetFrameHeight();
    const float message_height = std::max(46.0f, ImGui::GetContentRegionAvail().y - button_height - 12.0f);
    if (ImGui::InputTextMultiline("##commit message", &_change_info_message, ImVec2(-1.0f, message_height)))
        _change_info_dirty = true;
    const bool message_changed = _change_info_dirty && _change_info_message != revision->description;
    const bool modifies_locked = RewritesLockedCommit(revision->oid);
    ImGui::BeginDisabled(!message_changed || !_active_operation.empty());
    const bool save = modifies_locked ? DangerButton("Save message") : ImGui::Button("Save message");
    ImGui::EndDisabled();

    // Save message action
    if (save)
    {
        if (modifies_locked)
        {
            _pending_change_info_save = true;
            QueueCommands({Describe{revision->oid, _change_info_message}}, {revision->oid},
                "Saving the message will rewrite this locked commit.");
        }
        else
        {
            EnqueueAction(Describe{revision->oid, _change_info_message});
            _change_info_dirty = false;
        }
    }
    ImGui::End();
}

} // namespace Ggui

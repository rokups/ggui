// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace Ggui
{
using namespace ApplicationInternal;

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
                CreateChange(true);
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
        ImGui::Separator();
        if (ActionMenuItem(ICON_MS_ARROW_DOWNWARD, "Move working copy to previous")) _engine.Enqueue(MoveChange{GG_MOVE_PREVIOUS});
        if (ActionMenuItem(ICON_MS_ARROW_UPWARD, "Move working copy to next")) _engine.Enqueue(MoveChange{GG_MOVE_NEXT});
        ImGui::Separator();
        RenderSelectedChangeActions(_selected_revision, false);
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

void Application::RenderSelectedChangeActions(const std::string& revision, bool select_revision)
{
    const bool enabled = !revision.empty();
    const auto select = [&] {
        if (select_revision)
            SelectRevision(revision);
    };
    const auto dialog = [&](std::string_view icon, const char* label, const char* shortcut, Dialog action,
                            bool action_enabled = true) {
        if (ActionMenuItem(icon, label, shortcut, enabled && action_enabled))
        {
            select();
            OpenDialog(action);
        }
    };

    dialog(ICON_MS_INFO, "Metaedit...", nullptr, Dialog::Metaedit);
    if (ActionMenuItem(ICON_MS_EDIT, "Edit", "E", enabled))
    {
        select();
        _engine.Enqueue(Edit{revision});
    }
    const bool can_rebase = !select_revision || (!_selected_revision.empty() && revision != _selected_revision);
    if (ActionMenuItem(ICON_MS_REBASE, "Rebase...", nullptr, enabled && can_rebase))
    {
        OpenDialog(Dialog::Rebase);
        if (select_revision)
            _input_primary = revision;
    }
    dialog(ICON_MS_MERGE, "Squash...", nullptr, Dialog::Squash);
    dialog(ICON_MS_DIFFERENCE, "Split...", "S", Dialog::Split);
    dialog(ICON_MS_RESTORE, "Restore...", nullptr, Dialog::Restore, _compare_to.empty());
    if (ActionMenuItem(ICON_MS_DELETE, "Abandon...", "A", enabled))
    {
        select();
        RequestAbandon(revision);
    }
    if (ActionMenuItem(ICON_MS_FORMAT_LIST_BULLETED, "Simplify parents", nullptr, enabled))
    {
        select();
        QueueCommands({SimplifyParents{{revision}}}, {revision},
            "Simplifying the parents will rewrite a locked commit.");
    }
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
    ImGui::BeginDisabled(!_active_operation.empty() || _recent_repositories.empty());
    const float dropdown_height = ImGui::GetFrameHeight();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    const bool open_recent = ImGui::Button(ICON_MS_KEYBOARD_ARROW_DOWN "###Recent repositories",
        ImVec2(0.0f, dropdown_height));
    ImGui::PopFont();
    if (open_recent)
        ImGui::OpenPopup("Recent repositories popup");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Switch repository");
    ImGui::SameLine();
    if (ActionButton(ICON_MS_FOLDER, repository_name))
        OpenExternalPath(_snapshot->root, "Repository directory"); // GCOV_EXCL_LINE: external application handoff
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nClick to open the repository directory.", _snapshot->root.c_str());
    ImGui::PopStyleColor(3);
    if (ImGui::BeginPopup("Recent repositories popup"))
    {
        for (const std::string& path : _recent_repositories)
        {
            const std::string label = IconLabel(ICON_MS_FOLDER, path);
            if (ImGui::MenuItem(label.c_str(), nullptr, path == _snapshot->root, path != _snapshot->root))
                _engine.Enqueue(OpenRepository{path});
        }
        ImGui::EndPopup();
    }
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
        if (const NamedRef* closest = ClosestBookmark(*_snapshot, _snapshot->working_copy); closest != nullptr)
        {
            ImGui::SameLine();
            const std::string label = ReferenceLabel(*closest);
            ImGui::TextDisabled("%s%s", ICON_MS_BOOKMARK, label.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Closest bookmark to the working copy");
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

} // namespace Ggui

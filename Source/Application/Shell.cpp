// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

namespace
{
struct RecentRepository
{
    std::string path;
    std::vector<std::string> components;
    std::string parent;
    std::string name;
    std::string visible;
};

std::string RecentSuffix(const RecentRepository& repository, std::size_t count)
{
    const std::size_t begin = repository.components.size() > count
        ? repository.components.size() - count
        : 0;
    std::string result;
    for (std::size_t index = begin; index < repository.components.size(); ++index)
    {
        if (!result.empty()) result += '/';
        result += repository.components[index];
    }
    return result;
}

std::vector<RecentRepository> RecentRepositories(const std::vector<std::string>& paths)
{
    std::vector<RecentRepository> result;
    result.reserve(paths.size());
    for (const std::string& path : paths)
    {
        RecentRepository repository;
        repository.path = path;
        const std::filesystem::path normalized = std::filesystem::path(path).lexically_normal();
        for (const std::filesystem::path& component : normalized)
        {
            const std::string value = component.string();
            if (!value.empty() && value != "." && value != normalized.root_name()
                && value != normalized.root_directory())
                repository.components.push_back(value);
        }
        if (repository.components.empty()) repository.components.push_back(RepositoryName(path));
        repository.name = repository.components.back();
        result.push_back(std::move(repository));
    }

    for (std::size_t first = 0; first < result.size(); ++first)
    {
        std::vector<std::size_t> duplicates;
        for (std::size_t index = 0; index < result.size(); ++index)
            if (result[index].name == result[first].name)
                duplicates.push_back(index);
        if (duplicates.size() < 2 || duplicates.front() != first)
            continue;

        std::size_t visible_components = 2;
        while (true)
        {
            std::set<std::string> labels;
            for (const std::size_t index : duplicates)
                labels.insert(RecentSuffix(result[index], visible_components));
            if (labels.size() == duplicates.size()) break;
            if (std::ranges::none_of(duplicates,
                    [&](std::size_t index) { return result[index].components.size() > visible_components; }))
                break;
            ++visible_components;
        }
        for (const std::size_t index : duplicates)
        {
            result[index].visible = RecentSuffix(result[index], visible_components);
            result[index].parent = result[index].visible.substr(
                0, result[index].visible.size() - result[index].name.size());
        }
    }
    for (RecentRepository& repository : result)
        if (repository.visible.empty()) repository.visible = repository.name;
    return result;
}
} // namespace

#ifdef IMGUI_BUILD_TESTING
std::vector<std::pair<std::string, std::string>> Application::RecentRepositoryLabelsForTest(
    const std::vector<std::string>& paths)
{
    std::vector<std::pair<std::string, std::string>> result;
    for (RecentRepository& repository : RecentRepositories(paths))
        result.emplace_back(std::move(repository.parent), std::move(repository.name));
    return result;
}
#endif

void Application::RenderFrame()
{
    PollMergeTool();

    // Main menu bar
    RenderMenuBar();

    // Global keyboard shortcuts
    ImGuiIO& io = ImGui::GetIO();
    if (_dialog == Dialog::None)
    {
        if (_active_operation.empty() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) PickAndOpen(false);
        if (_snapshot != nullptr && _active_operation.empty() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_W))
            EnqueueAction(CloseRepository{});
        if (CanCreateChange() && _active_operation.empty() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N))
            CreateChange();
    }
    if (!io.WantTextInput)
    {
        if (_snapshot != nullptr && _snapshot->can_undo && _active_operation.empty() && io.KeyCtrl
            && ImGui::IsKeyPressed(ImGuiKey_Z))
            EnqueueAction(Undo{});
        if (_snapshot != nullptr && _snapshot->can_redo && _active_operation.empty() && io.KeyCtrl
            && ImGui::IsKeyPressed(ImGuiKey_Y))
            EnqueueAction(Redo{});
        if (_snapshot != nullptr && _active_operation.empty() && ImGui::IsKeyPressed(ImGuiKey_F5))
            EnqueueAction(Refresh{true, {}, true});
        if (_snapshot != nullptr && _dialog == Dialog::None && ImGui::IsKeyPressed(ImGuiKey_F6))
            NavigateChangedFile(io.KeyShift ? -1 : 1);
    }

    // Repository workspace or welcome screen
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
        if (_show_reflog) RenderReflog();
        if (_show_blame) RenderBlame();
        if (_show_changes) RenderChanges();
        if (_show_change_info) RenderChangeInformation();
        if (_show_diff) RenderDiff();
        if (_show_operations) RenderOperations();
    }

    // Modal and settings windows
    RenderDialogs();
    if (_show_settings) RenderSettings();
}

void Application::SetupDockspace()
{
    // Full-viewport dockspace window
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

    // Docking area
    const ImGuiID dockspace = ImGui::GetID("ggui main dockspace");
    ImGui::DockSpace(dockspace, {}, ImGuiDockNodeFlags_PassthruCentralNode);

    // Default panel layout
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
        ImGui::DockBuilderDockWindow("Reflog", diff);
        ImGui::DockBuilderDockWindow("Blame", diff);
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

    // Repository menu
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
            RenderRecentRepositories();
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
                EnqueueAction(CloseRepository{});
            ImGui::Separator();
        }
        if (ActionMenuItem(ICON_MS_REFRESH, "Refresh", "F5", _snapshot != nullptr))
            EnqueueAction(Refresh{true, {}, true});
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ActionMenuItem(ICON_MS_SETTINGS, "Settings..."))
            OpenSettings();
        if (ActionMenuItem(ICON_MS_CLOSE, "Quit"))
            _running = false; // GCOV_EXCL_LINE: terminating the host aborts an in-process test queue
        ImGui::EndMenu();
    }

    // Change menu
    if (ImGui::BeginMenu("Change", _snapshot != nullptr && !actions_locked))
    {
        if (ActionMenuItem(ICON_MS_ADD, "New change", "Ctrl+N", CanCreateChange() && _active_operation.empty()))
            CreateChange();
        if (ActionMenuItem(ICON_MS_COMMIT, "Commit...", nullptr, _compare_to.empty())) OpenDialog(Dialog::Commit);
        ImGui::Separator();
        if (ActionMenuItem(ICON_MS_ARROW_DOWNWARD, "Move working copy to previous")) EnqueueAction(MoveChange{GG_MOVE_PREVIOUS});
        if (ActionMenuItem(ICON_MS_ARROW_UPWARD, "Move working copy to next")) EnqueueAction(MoveChange{GG_MOVE_NEXT});
        ImGui::Separator();
        RenderSelectedChangeActions(_selected_revision, false);
        ImGui::EndMenu();
    }

    // Edit menu
    if (ImGui::BeginMenu("Edit", _snapshot != nullptr && !actions_locked))
    {
        if (ActionMenuItem(ICON_MS_UNDO, "Undo", "Ctrl+Z", _snapshot->can_undo && _active_operation.empty()))
            EnqueueAction(Undo{});
        if (ActionMenuItem(ICON_MS_REDO, "Redo", "Ctrl+Y", _snapshot->can_redo && _active_operation.empty()))
            EnqueueAction(Redo{});
        ImGui::Separator();
        if (ActionMenuItem(ICON_MS_UPLOAD_FILE, "Apply patch...", nullptr, _compare_to.empty()))
            _open_apply_patch = true;
        ImGui::EndMenu();
    }

    // View menu
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
            ImGui::MenuItem("Reflog", nullptr, &_show_reflog);
            ImGui::MenuItem("Blame", nullptr, &_show_blame);
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
            _show_reflog = _show_blame = false;
            _show_operations = false;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Application::RenderRecentRepositories()
{
    // Repository filter
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(FontPx(300.0f));
    ImGui::InputTextWithHint("##recent repository filter", "Filter repositories", &_recent_filter);
    ImGui::Separator();

    // Repository list
    bool any_visible = false;
    std::string remove;
    for (const RecentRepository& repository : RecentRepositories(_recent_repositories))
    {
        if (!ContainsInsensitive(repository.visible, _recent_filter)) continue;
        any_visible = true;
        const bool selected = _snapshot != nullptr && repository.path == _snapshot->root;
        ImGui::PushID(repository.path.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        const bool open = ImGui::Selectable(
            (repository.visible + "###repository").c_str(), selected, ImGuiSelectableFlags_SpanAvailWidth);
        ImGui::PopStyleColor();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        ImVec2 text{minimum.x + ImGui::GetStyle().FramePadding.x,
            minimum.y + (maximum.y - minimum.y - ImGui::GetTextLineHeight()) * 0.5f};
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddText(text, ImGui::GetColorU32(ImGuiCol_TextDisabled), repository.parent.c_str());
        text.x += ImGui::CalcTextSize(repository.parent.c_str()).x;
        draw->AddText(text, ImGui::GetColorU32(ImGuiCol_Text), repository.name.c_str());
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", repository.path.c_str());
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) remove = repository.path;
        }
        if (open && !selected && remove.empty()) EnqueueAction(OpenRepository{repository.path});
        if (selected) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
    }
    if (!remove.empty()) ForgetRepository(remove);
    if (!any_visible) ImGui::TextDisabled("No matching repositories.");
}

void Application::RenderSelectedChangeActions(const std::string& revision, bool select_revision)
{
    // Shared change action state
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

    // Change actions
    if (ActionMenuItem(ICON_MS_EDIT, "Edit", "E", enabled))
    {
        select();
        EnqueueAction(Edit{revision});
    }
    const std::string duplicate_label = IconLabel(ICON_MS_CONTENT_COPY, "Duplicate");
    if (ImGui::BeginMenu(duplicate_label.c_str(), enabled))
    {
        if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Change", "D"))
        {
            select();
            EnqueueAction(Duplicate{revision, false});
        }
        if (ActionMenuItem(ICON_MS_ACCOUNT_TREE, "Branch", "Shift+D"))
        {
            select();
            EnqueueAction(Duplicate{revision, true});
        }
        ImGui::EndMenu();
    }
    const std::string& current_commit = CurrentCommit(*_snapshot);
    const bool can_rebase = !current_commit.empty() && (!select_revision || revision != current_commit);
    if (ActionMenuItem(ICON_MS_REBASE, "Rebase...", nullptr, enabled && can_rebase))
    {
        OpenDialog(Dialog::Rebase);
        if (select_revision)
            _input_primary = revision;
    }
    const bool squash_descendants = select_revision && ImGui::GetIO().KeyShift;
    if (ActionMenuItem(ICON_MS_MERGE,
            squash_descendants ? "Squash with descendants..." : "Squash...",
            squash_descendants ? "Shift+S" : "S", enabled))
    {
        select();
        RequestSquash(revision, squash_descendants);
    }
    if (!select_revision && ActionMenuItem(
            ICON_MS_MERGE, "Squash with descendants...", "Shift+S", enabled))
        RequestSquash(revision, true);
    if (!select_revision)
    {
        dialog(ICON_MS_DIFFERENCE, "Split...", "Alt+S", Dialog::Split);
        dialog(ICON_MS_RESTORE, "Restore...", nullptr, Dialog::Restore, _compare_to.empty());
    }
    const bool abandon_branch = select_revision && ImGui::GetIO().KeyShift;
    if (ActionMenuItem(ICON_MS_DELETE, abandon_branch ? "Abandon branch..." : "Abandon...",
            abandon_branch ? "Shift+A" : "A", enabled))
    {
        select();
        RequestAbandon(revision, abandon_branch);
    }
    if (!select_revision && ActionMenuItem(ICON_MS_DELETE, "Abandon branch...", "Shift+A", enabled))
    {
        select();
        RequestAbandon(revision, true);
    }
    if (ActionMenuItem(ICON_MS_FORMAT_LIST_BULLETED, "Simplify parents", nullptr, enabled))
    {
        select();
        const Revision* selected = ResolveSnapshotRevision(*_snapshot, revision, _history_revisions);
        bool redundant = false;
        if (selected != nullptr)
            for (const std::string& parent : selected->parents)
            {
                const auto descendants = AbandonRevisions(parent, true);
                redundant |= std::ranges::any_of(selected->parents, [&](const std::string& other) {
                    return other != parent && std::ranges::find(descendants, other) != descendants.end();
                }) || std::ranges::count(selected->parents, parent) > 1;
            }
        if (redundant)
            QueueCommands({SimplifyParents{{revision}}}, {revision},
                "Simplifying the parents will rewrite a locked commit.");
    }
}

void Application::RenderToolbar()
{
    // Change actions
    const std::string& current_commit = CurrentCommit(*_snapshot);
    const bool expansion_loading = !_history_expansion_pending.empty();
    const bool expansion_feedback = expansion_loading
        || std::chrono::steady_clock::now() < _history_expansion_feedback_until;
    ImGui::SetCursorPos(ImVec2(10.0f, 8.0f));
    ImGui::BeginDisabled(!_active_operation.empty());
    ImGui::BeginDisabled(current_commit.empty());
    if (ActionButton(ICON_MS_ADD, "New")) CreateChange("@");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Create and edit a new empty change on @.");
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
    if (ActionButton(ICON_MS_ARROW_DOWNWARD, "Prev")) EnqueueAction(MoveChange{GG_MOVE_PREVIOUS});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Move the working copy to its parent. This modifies the repository and can be undone.");
    ImGui::SameLine();
    if (ActionButton(ICON_MS_ARROW_UPWARD, "Next")) EnqueueAction(MoveChange{GG_MOVE_NEXT});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Move the working copy to its child. This modifies the repository and can be undone.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!_snapshot->can_undo);
    if (ActionButton(ICON_MS_UNDO, "Undo")) EnqueueAction(Undo{});
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!_snapshot->can_redo);
    if (ActionButton(ICON_MS_REDO, "Redo")) EnqueueAction(Redo{});
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ActionButton(ICON_MS_REFRESH, "Refresh")) EnqueueAction(Refresh{true, {}, true});

    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    // A cancellable history search takes precedence over repository metadata
    // in the fixed-width toolbar. Rendering it before the selector keeps the
    // cancel control reachable instead of placing a working button beyond the
    // right edge on ordinary window sizes.
    const bool reveal_loaded = RevealRevisionLoaded();
    const bool reveal_visible = reveal_loaded && std::ranges::any_of(_visible_revisions, [&](const int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= _history_revisions.size()) return false;
        const Revision& revision = _history_revisions[static_cast<std::size_t>(index)];
        return revision.oid == _reveal_revision
            || std::ranges::find(revision.aliases, _reveal_revision) != revision.aliases.end();
    });
    const bool reveal_searching = !_reveal_revision.empty() && !reveal_visible;
    const bool history_searching = reveal_searching;
    std::string history_activity;
    if (reveal_searching)
        history_activity = "Finding " + ShortId(_reveal_revision) + " ("
            + std::to_string(_history_revisions.size()) + " visible)...";
    else if (!_history_expansion_pending.empty())
        history_activity = _history_expansion_pending.starts_with("region:")
            ? "Loading more commits..." : "Updating commit graph...";
    else if (_history_view == nullptr || _history_view->repository_generation != _snapshot->repository_generation)
        history_activity = "Preparing commit graph...";
    std::string foreground_activity;
    if (!_active_operation.empty())
    {
        foreground_activity = "Working: " + _active_operation;
        if (!_progress_phase.empty())
        {
            foreground_activity += " (" + _progress_phase;
            if (_progress_total != 0)
                foreground_activity += " " + std::to_string(_progress_completed) + "/"
                    + std::to_string(_progress_total);
            foreground_activity += ")";
        }
    }
    else
        foreground_activity = history_activity;
    const bool activity_running = !foreground_activity.empty() || !_background_activities.empty();
    const bool completion_feedback = expansion_feedback && !expansion_loading;
    // Repository metadata yields the limited toolbar space while transient
    // history work needs a reachable cancel control.
    if (!activity_running && !completion_feedback)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("REPOSITORY");
        ImGui::SameLine();
        const std::string repository_name = RepositoryName(_snapshot->root);
        const bool can_switch_repository = std::any_of(_recent_repositories.begin(), _recent_repositories.end(),
            [&](const std::string& path) { return path != _snapshot->root; });
        ImGui::BeginDisabled(!_active_operation.empty() || !can_switch_repository);
        const bool repository_combo_open =
            ImGui::BeginCombo("###Repository", repository_name.c_str(), ImGuiComboFlags_WidthFitPreview);
        const bool repository_combo_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (repository_combo_open)
        {
            RenderRecentRepositories();
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (repository_combo_hovered)
            ImGui::SetTooltip("%s\nSwitch repository.", _snapshot->root.c_str());
        ImGui::SameLine();
        if (ImGui::Button(ICON_MS_FOLDER_OPEN "###Open repository folder"))
            OpenExternalPath(_snapshot->root, "Repository directory"); // GCOV_EXCL_LINE: external application handoff
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nOpen repository folder.", _snapshot->root.c_str());
    }

    // Current commit and bookmark. Transient activity takes this same compact
    // status slot so it remains visible even when the action bar is crowded.
    if (!current_commit.empty() && !activity_running && !completion_feedback)
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.78f, 0.42f, 1.0f));
        ImGui::BeginGroup();
        TextLabelledId(_snapshot->working_copy.empty() ? "HEAD " : "@ ", current_commit,
            RevisionPrefix(current_commit), CommitIdColor(true));
        ImGui::EndGroup();
        ImGui::PopStyleColor();
        if (ImGui::BeginPopupContextItem("current commit ID context"))
        {
            IdCopyMenuItems("commit ID", current_commit, RevisionPrefix(current_commit));
            ImGui::EndPopup();
        }
    }

    // All repository work shares one status indicator. Interactive work gets
    // the label; otherwise background work remains visible without competing
    // spinners. The tooltip always exposes every concurrent background task.
    if (activity_running)
    {
        ImGui::SameLine();
        static constexpr std::array spinner{'|', '/', '-', '\\'};
        const std::size_t frame = static_cast<std::size_t>(ImGui::GetTime() * 8.0) % spinner.size();
        const std::string label = foreground_activity.empty()
            ? std::string("Background activity") : foreground_activity;
        const std::string visible = std::string(1, spinner[frame]) + " " + label;
        ImVec2 position = ImGui::GetCursorScreenPos();
        position.y += ImGui::GetStyle().FramePadding.y;
        ImGui::InvisibleButton("Repository activity",
            ImVec2(ImGui::CalcTextSize(visible.c_str()).x, ImGui::GetFrameHeight()));
        ImGui::GetWindowDrawList()->AddText(position, kTextMuted, visible.c_str());
        if (ImGui::IsItemHovered())
        {
            if (!_background_activities.empty())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Background work may temporarily slow repository reads.");
                ImGui::Separator();
                for (const auto& activity : _background_activities)
                    ImGui::BulletText("%s", activity.second.c_str());
                ImGui::EndTooltip();
            }
        }
        if (!_active_operation.empty())
        {
            ImGui::SameLine();
            if (ActionButton(ICON_MS_CLOSE, "Cancel")) _engine.Cancel();
        }
        else if (history_searching)
        {
            const float cancel_width = ImGui::CalcTextSize("Cancel").x
                + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine(std::max(0.0f, ImGui::GetWindowWidth() - cancel_width - 10.0f));
            const bool cancel_history = ImGui::Button("Cancel###Cancel history search");
            if (cancel_history || ImGui::IsItemClicked())
                CancelHistorySearch();
        }
    }
    else if (completion_feedback)
    {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Commits loaded");
    }

    // Error banner
    if (!_error_message.empty())
    {
        ImGui::SetCursorPosX(10.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
            _dark_theme ? ImVec4(0.22f, 0.07f, 0.08f, 1.0f) : ImVec4(1.0f, 0.88f, 0.88f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(FontPx(6.0f), FontPx(2.0f)));
        ImGui::BeginChild("error banner", {}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
            ImGuiWindowFlags_NoScrollbar);
        const bool dismiss = ImGui::SmallButton(ICON_MS_CLOSE "###Dismiss error");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dismiss error");
        ImGui::SameLine();
        ImGui::TextWrapped("Error: %s", _error_message.c_str());
        if (dismiss) _error_message.clear();
        ImGui::EndChild();
        ImGui::PopStyleVar();
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
        std::string remove;
        for (const std::string& path : _recent_repositories)
        {
            const bool open = ImGui::Selectable(path.c_str(), false, 0, ImVec2(width, 34.0f));
            if (ImGui::IsItemHovered())
            {
                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) remove = path;
            }
            if (open && remove.empty())
                EnqueueAction(OpenRepository{path});
        }
        ImGui::EndDisabled();
        if (!remove.empty()) ForgetRepository(remove);
    }
    ImGui::EndGroup();
    ImGui::End();
}

} // namespace Ggui

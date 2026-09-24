// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "Core/RepositoryEngine.hpp"
#include "Core/Settings.hpp"
#include "Graph/Layout.hpp"

#include <SDL3/SDL.h>

#ifdef DeleteFile
#undef DeleteFile
#endif

#include <cstdint>
#include <array>
#include <filesystem>
#include <future>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef IMGUI_BUILD_TESTING
struct ImGuiTestEngine;
#endif
struct ImGuiContext;
struct ImGuiSettingsHandler;
struct ImGuiTextBuffer;

namespace Ggui
{

class Application
{
public:
    int Run(int argc, char** argv);

#ifdef IMGUI_BUILD_TESTING
    static Application& Instance();
    void OpenTestRepository(const std::string& path);
    void RefreshForTest();
    std::shared_ptr<const RepoSnapshot> SnapshotForTest() const;
    const std::string& SelectedFileForTest() const;
    const std::vector<std::string>& SelectedRevisionsForTest() const;
    bool DiffSideBySideForTest() const;
    DiffWhitespaceMode DiffWhitespaceModeForTest() const;
    int DiffContextLinesForTest() const;
    const std::string& CompareToForTest() const;
    bool FileComparisonForTest() const;
    bool CanNavigateChangedFileForTest(int direction) const;
    void NavigateChangedFileForTest(int direction);
    void ToggleComparisonForTest();
    void ToggleFileComparisonForTest();
    void SelectRevisionForTest(const std::string& oid, bool additive = false);
    std::vector<std::string> SelectedParentsForTest() const;
    std::vector<std::string> DialogFilesetsForTest() const;
    const std::string& DialogDestinationForTest() const;
    const std::string& DialogDescriptionForTest() const;
    const MoveDiffLines& PendingMoveDiffLinesForTest() const;
    std::string RebaseSourceForTest() const;
    bool DialogModifiesLockedCommitForTest() const;
    void ApplyEventForTest(Event event);
    void ShowDropConfirmationForTest(
        const std::string& source, const std::string& target, int action, bool entire_branch = false);
    std::pair<int, bool> PendingDropActionForTest() const;
    bool PendingDropCopyForTest() const;
    void ShowWorkspaceRenameForTest();
    void ShowBranchRenameForTest(const std::string& name);
    void SetSnapshotForTest(RepoSnapshot snapshot);
    void ClearSnapshotForTest();
    void AddRecentForTest(const std::string& path);
    const std::vector<std::string>& RecentRepositoriesForTest() const;
    void ProcessEventForTest(SDL_Event event);
    void SetDarkThemeForTest(bool dark);
    static std::vector<std::string> SplitLinesForTest(const std::string& text);
    static std::string DeltaNameForTest(git_delta_t status);
    static bool ContainsInsensitiveForTest(const std::string& text, const std::string& query);
    static unsigned int IdColorForTest(bool working_copy);
    static bool SupportsDiffLanguageForTest(const std::string& path);
    static std::string FileUrlForTest(const std::string& path);
    static std::vector<std::pair<std::string, std::string>> RecentRepositoryLabelsForTest(
        const std::vector<std::string>& paths);
    static std::optional<std::filesystem::path> WorkingCopyPathForTest(
        const std::string& root, const std::string& relative);
    static std::string LimitLinesForTest(const std::string& text, std::size_t maximum);
    static std::string ReferenceLabelForTest(const NamedRef& ref);
    static std::pair<std::string, std::size_t> ReferenceBadgeLabelForTest(
        const NamedRef& ref, const std::vector<NamedRef>& refs);
    static unsigned int BranchColorForTest(const std::string& name, const std::vector<NamedRef>& refs);
    static std::string FormatTimestampForTest(std::int64_t timestamp);
    static int DropPlacementForTest(int action);
    static std::string DropTooltipForTest(int action, bool entire_branch = false, bool copy = false);
    std::vector<std::string> AbandonRevisionsForTest(const std::string& revision) const;
    const std::string& PendingEditorRevisionForTest() const;
    const std::filesystem::path& OpenedEditorPathForTest() const;
    bool HistoryLoadPendingForTest() const;
    bool HistoryExpansionPendingForTest() const;
    void CancelHistorySearchForTest();
    const std::vector<std::string>& VisibleBranchesForTest() const;
    const std::vector<std::string>& SelectedTagsForTest() const;
    const std::vector<std::string>& SelectedRemotesForTest() const;
    std::vector<std::string> VisibleHistoryRevisionsForTest() const;
    const std::vector<Revision>& HistoryRevisionsForTest() const;
    std::size_t RenderedHistoryRowsForTest() const;
    const std::string& ActiveOperationForTest() const;
    void RequestBlameForTest(const std::string& revision, const std::string& path);
    std::vector<std::string> SelectedFilesForTest() const;
    const std::string& FocusedFileForTest() const;
    std::pair<std::string, std::string> BlameLocationForTest() const;
    const std::string& ErrorMessageForTest() const;
    void CreateChangeForTest(const std::string& parent);
    bool UsesSdrSwapchainForTest() const;
    bool CaptureFramebufferForTest(
        int x, int y, int width, int height, unsigned int* pixels);
#endif

private:
    enum class Dialog
    {
        None,
        Clone,
        Commit,
        Metaedit,
        Rebase,
        Squash,
        Split,
        Abandon,
        Restore,
        Branch,
        BranchRename,
        Tag,
        RemoteAdd,
        WorkspaceAdd,
        WorkspaceRename,
        WorkspaceRemove,
        PushTo,
        Reconcile,
        Credentials,
        ConfirmDrop,
        ConfirmLocked,
        ConfirmBranchMove,
        ConfirmBranchDelete,
        ConfirmWorkingFiles,
    };

    enum class DropAction
    {
        ReorderBefore,
        ReorderAfter,
        Squash,
        Rebase,
    };

    struct PendingDrop
    {
        std::string source;
        std::string target;
        DropAction action = DropAction::ReorderBefore;
        bool entire_branch = false;
        bool copy = false;
    };

    struct PendingBranchDelete
    {
        std::string name;
        bool local = false;
        std::vector<std::string> remotes;
    };

    bool Initialize();
    void Shutdown();
    bool SettingsPersistenceEnabled() const;
    void LoadSettings();
    void SaveSettings();
    void RegisterWindowSettings();
    static void* WindowSettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name);
    static void WindowSettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line);
    static void WindowSettingsApplyAll(ImGuiContext*, ImGuiSettingsHandler*);
    static void WindowSettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler*, ImGuiTextBuffer* output);
    void RememberRepository(const std::string& path);
    bool EnqueueAction(Command command);
    void PollEngine();
    void ApplyEvent(Event event);
    void ProcessEvent(SDL_Event& event);
    void RenderFrame();
    void RenderMenuBar();
    void RenderSelectedChangeActions(const std::string& revision, bool select_revision);
    void RenderToolbar();
    void RenderWelcome();
    void RenderSettings();
    void OpenSettings();
    void ReloadNativeSettings();
    void ForgetRepository(const std::string& path);
    void RenderBranches();
    void RenderBranchDeleteMenu(const std::string& name, bool nested = false);
    void RenderTags();
    void RenderWorkspaces();
    void RenderRemotes();
    void RenderHistory();
    void RenderReflog();
    void RenderBlame();
    // Draws the Blame window's contents. Navigation and new blame requests
    // are returned so they apply after the frame's references are used.
    void RenderBlameContents(int& navigation, std::optional<std::pair<std::string, std::string>>& request);
    void RenderChanges();
    void RenderChangeInformation();
    void RenderDiff();
    void RenderOperations();
    void RenderDialogs();
    void RenderRecentRepositories();
    void RefreshRecentSummaries();
    void SetupDockspace();
    void UpdateGraphBuild();
    void EnsureVisibleBranchSelection();
    void EnsureTagSelection();
    void EnsureRemoteSelection();
    bool IsSelectedRemote(std::string_view remote) const;
    bool IsVisibleBranchRef(const NamedRef& ref) const;
    void RestoreRepositorySelections(const std::string& root);
    void RememberRepositorySelections();
    std::string VisibleBranchesKey() const;
    void RebuildIdPrefixes();
    void CancelHistorySearch();
    bool RevealRevisionLoaded() const;
    bool HistoryTargetConnected(std::string_view target) const;
    void ExpandGraphRow(std::size_t visible_row, bool merge_history = false);
    std::size_t RevisionPrefix(const std::string& oid) const;
    std::size_t OperationPrefix(const std::string& oid) const;
    void RevealRevision(const std::string& oid);
    void SelectRevision(const std::string& oid, bool additive = false);
    // Selects one file, replacing any multi-selection.
    void SelectFile(const std::string& path);
    // Shows a file in Diff without changing the multi-selection.
    void FocusFile(const std::string& path);
    // Applies a click in the Changes list: Ctrl toggles, Shift selects a range.
    void ClickChangedFile(const std::string& path, bool toggle, bool range);
    void SelectAllChangedFiles();
    bool IsChangedFileSelected(const std::string& path) const;
    // Selected files in list order; the focused file alone without a selection.
    std::vector<const StatusEntry*> SelectedChangedFiles() const;
    std::vector<const StatusEntry*> VisibleChangedFiles() const;
    void RequestDiff(bool fallback_to_first);
    // Opens a blame view and records it in the Back/Forward history.
    void RequestBlame(const std::string& revision, const std::string& path);
    // Reloads the current blame view, keeping it on screen until the result.
    void ReloadBlame();
    // Moves through the blame history; negative is Back.
    void NavigateBlame(int direction);
    bool CanNavigateBlame(int direction) const;
    void LoadBlameView(const std::string& revision, const std::string& path, bool keep_content);
    void ClearBlame();
    void ToggleComparison(bool file_comparison);
    void ResetRepositoryState();
    std::pair<std::string, std::string> AdjacentRevisions(const std::string& revision) const;
    bool FileMatchesFilter(const StatusEntry& file) const;
    bool CanNavigateChangedFile(int direction) const;
    // Moves the focused file; extend grows the selection from its anchor.
    void NavigateChangedFile(int direction, bool extend = false);
    static std::optional<std::filesystem::path> WorkingCopyPath(
        const std::string& root, const std::string& relative);
    void OpenFileInEditor(const std::string& path);
    void OpenTemporaryFileInEditor(const FileContentReady& file);
    void OpenEditorPath(const std::filesystem::path& path);
    void OpenConflictInMergeTool(const std::string& path);
    void PollMergeTool();
    void MarkConflictResolved(const std::string& path);
    void FinishConflictMerge(bool resolved);
    void ClearConflictMerge();
    void RenderRevisionTooltip(
        std::string_view label, const std::string& revision, std::string_view hint = {});
    void OpenExternalPath(const std::filesystem::path& path, std::string_view description);
    void OpenWorkspaceInNewWindow(const std::string& path);
    void OpenExternalDiff(const std::string& path, const std::string& compare_to);
    std::vector<std::string> SelectedParentRevisions() const;
    std::vector<std::string> AbandonRevisions(
        const std::string& revision, bool include_descendants) const;
    void RequestAbandonRevisions(const std::string& revision);
    void PollAbandonRevisions();
    std::vector<RemoteBranchDelete> RemoteBranchesAt(
        const std::vector<std::string>& revisions) const;
    void RequestBranchDelete(const std::string& name, bool local, std::vector<std::string> remotes);
    // Asks before reverting or deleting working-tree files, which discards
    // edits that no operation records.
    void RequestWorkingFiles(std::vector<StatusEntry> files, bool remove);
    void MoveBranch(const NamedRef& branch, const std::string& revision);
    // Alt+N (detach) never advances a branch; HEAD detaches at the new change.
    void CreateChange(const std::string& parent = {}, bool detach = false);
    // Git checkout semantics for Edit: the tip of exactly one free local branch
    // checks that branch out; anything else is checked out detached.
    std::string CheckoutTarget(const std::string& revision) const;
    void RequestAbandon(const std::string& revision, bool include_descendants = false);
    void RequestSquash(const std::string& revision, bool include_descendants = false);
    void QueueCommands(std::vector<Command> commands, const std::vector<std::string>& revisions,
        std::string warning);
    bool IsLocked(const std::string& revision) const;
    bool RewritesLockedCommit(const std::string& revision) const;
    std::vector<std::string> DropRewriteRoots() const;
    bool DialogModifiesLockedCommit() const;
    const Revision* RebaseSource() const;
    static gg_reorder_placement DropPlacement(DropAction action);
    static std::string_view DropTooltip(DropAction action, bool entire_branch = false, bool copy = false);
    bool CanCreateChange() const;
    bool CanSubmitDialog() const;
    void OpenDialog(Dialog dialog);
    void SubmitDialog();
    void PickAndOpen(bool initialize);
    std::string PickFolder(const std::string& initial = {});
    void ApplyTheme();
#ifdef IMGUI_BUILD_TESTING
    void InitializeTestEngine(const std::string& filter);
#endif

    RepositoryEngine _engine;
    std::shared_ptr<const RepoSnapshot> _snapshot;
    std::shared_ptr<const HistoryView> _history_view;
    std::vector<Revision> _history_revisions;
    DiffResult _diff;
    BlameResult _blame;
    std::vector<int> _visible_revisions;
    std::vector<GraphRow> _graph_rows;
    std::size_t _rendered_history_rows = 0;
    int _history_hovered_track = -1;
    std::string _history_branch_drag;
    int _history_hovered_commit_row = -1;
    std::uint64_t _graph_generation = 0;
    std::uint64_t _history_requested_generation = 0;
    std::uint64_t _history_applied_request = 0;
    std::uint64_t _history_search_scrolled_request = 0;
    std::string _history_requested_key;
    std::string _history_requested_filter;
    std::string _history_observed_filter;
    std::chrono::steady_clock::time_point _history_filter_changed{};
    std::string _history_anchor;
    float _history_anchor_offset = 0.0f;
    std::string _history_expansion_pending;
    std::string _branch_filter;
    std::vector<std::string> _visible_branches;
    bool _visible_branches_user_selected = false;
    std::vector<std::string> _selected_tags;
    std::vector<std::string> _selected_remotes;
    bool _selected_remotes_user_selected = false;
    std::unordered_map<std::string, std::vector<std::string>> _repository_visible_branches;
    std::unordered_map<std::string, std::vector<std::string>> _repository_selected_tags;
    std::unordered_map<std::string, std::vector<std::string>> _repository_selected_remotes;
    std::string _tag_filter;
    std::string _reflog_filter;
    std::string _changes_filter;
    std::string _graph_filter;
    std::string _recent_filter;
    std::string _built_filter;
    std::string _built_branches;
    std::string _reveal_revision;
    float _history_scroll_target = -1.0f;
    int _history_scroll_frames = 0;
    std::unordered_map<std::string, std::size_t> _revision_prefixes;
    std::unordered_map<std::string, std::size_t> _operation_prefixes;
    std::unordered_map<std::string, std::vector<std::size_t>> _history_refs_by_revision;
    std::string _selected_revision;
    std::vector<std::string> _selected_revisions;
    std::string _selected_file;
    std::string _preferred_file;
    // Changes multi-selection. _selected_file is the focused file shown in
    // Diff; the selection applies only while it holds that file and belongs
    // to the listed change.
    std::vector<std::string> _selected_files;
    std::string _selected_files_revision;
    std::string _file_selection_anchor;
    std::string _pending_revision;
    std::string _pending_editor_revision;
    std::string _pending_editor_path;
    std::filesystem::path _editor_temp_directory;
    SDL_Process* _merge_process = nullptr;
    std::filesystem::path _merge_temp_directory;
    std::filesystem::path _merge_result_path;
    std::string _merge_revision;
    std::string _merge_conflict_path;
    bool _open_merge_confirmation = false;
    int _merge_exit_code = 0;
    std::string _compare_to;
    bool _file_comparison = false;
    std::vector<std::string> _recent_repositories;
    // Checkout context shown beside recent repositories, read off-thread.
    std::unordered_map<std::string, RepositorySummary> _recent_summaries;
    std::future<std::vector<std::pair<std::string, RepositorySummary>>> _recent_summaries_future;
    bool _drop_batch_active = false;
    bool _drop_batch_opened = false;
    std::filesystem::path _settings_path;
    std::filesystem::path _executable_path;
    std::string _imgui_ini_path;
    std::string _status_message;
    std::string _error_message;
    std::string _pending_created_branch;
    std::vector<std::string> _abandon_revisions;
    std::string _abandon_revisions_revision;
    std::string _abandon_revisions_requested;
    std::string _abandon_revisions_in_flight;
    bool _abandon_revisions_complete = false;
    std::future<std::vector<std::string>> _abandon_revisions_future;
    std::vector<RemoteBranchDelete> _abandon_remote_branches;
    bool _abandon_modifies_locked = false;
    std::string _active_operation;
    std::map<std::uint64_t, std::string> _background_activities;
    std::string _progress_phase;
    std::size_t _progress_completed = 0;
    std::size_t _progress_total = 0;
    CredentialRequest _credential_request;
    PendingDrop _pending_drop;
    PendingBranchDelete _pending_branch_delete;
    // Files awaiting the working-tree revert or delete confirmation.
    std::string _pending_working_revision;
    std::vector<StatusEntry> _pending_working_files;
    bool _pending_working_delete = false;
    bool _open_drop_actions = false;
    bool _open_save_patch = false;
    bool _open_apply_patch = false;
    std::vector<Command> _pending_commands;
    std::string _locked_warning;
    bool _pending_change_info_save = false;
    std::uint64_t _dialog_snapshot_generation = 0;

    Dialog _dialog = Dialog::None;
    std::string _dialog_revision;
    std::string _input_primary;
    std::string _input_secondary;
    std::string _input_tertiary;
    std::string _input_filesets;
    bool _input_flag = false;
    bool _input_flag_secondary = false;
    bool _input_flag_tertiary = false;
    int _input_mode = 0;
    std::string _change_info_revision;
    std::string _change_info_message;
    bool _change_info_dirty = false;

    SDL_Window* _window = nullptr;
    SDL_GPUDevice* _gpu_device = nullptr;
#ifdef IMGUI_BUILD_TESTING
    SDL_GPUTexture* _capture_texture = nullptr;
    unsigned int _capture_width = 0;
    unsigned int _capture_height = 0;
#endif
    int _window_x = 0;
    int _window_y = 0;
    int _window_width = 1440;
    int _window_height = 900;
    bool _window_has_position = false;
    bool _window_has_size = false;
    bool _window_maximized = false;
    bool _running = true;
    bool _default_layout = true;
    bool _dark_theme = true;
    bool _show_branches = true;
    bool _show_tags = true;
    bool _show_workspaces = true;
    bool _show_remotes = true;
    bool _show_history = true;
    bool _show_reflog = false;
    bool _show_blame = false;
    bool _show_changes = true;
    bool _show_change_info = true;
    bool _show_diff = true;
    bool _show_operations = false;
    bool _show_settings = false;
    bool _settings_select_user = false;
    int _ui_scale_percent = 100;
    std::string _settings_repository;
    MaxNewFileSizeValues _max_new_file_size_values;
    std::array<std::string, 3> _max_new_file_size_inputs;
    std::array<std::string, 3> _max_new_file_size_errors;
    EditorValues _editor_values;
    std::array<std::string, 3> _editor_inputs;
    bool _diff_side_by_side = false;
    DiffWhitespaceMode _diff_whitespace_mode = DiffWhitespaceMode::Normal;
    int _diff_context_lines = 3;
    bool _diff_loading = false;
    // A snapshot changed the selected working tree after its diff loaded.
    bool _working_tree_diff_outdated = false;
    bool _blame_loading = false;
    std::string _blame_revision;
    std::string _blame_path;
    std::string _blame_filter;
    struct BlameLocation
    {
        std::string revision;
        std::string path;
        float scroll_y = 0.0f;
    };
    std::vector<BlameLocation> _blame_history;
    std::size_t _blame_history_index = 0;
    // Scroll of the rendered blame view, and the scroll to apply when the
    // next result loads (negative keeps the current view's position).
    float _blame_scroll_y = 0.0f;
    float _blame_restore_scroll = -1.0f;
#ifdef IMGUI_BUILD_TESTING
    ImGuiTestEngine* _test_engine = nullptr;
    bool _test_mode = false;
    bool _test_snapshot_mode = false;
    bool _smoke_mode = false;
    int _test_result = 1;
    std::filesystem::path _opened_editor_path_for_test;
#endif
};

} // namespace Ggui

// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "Core.hpp"
#include "Graph.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef IMGUI_BUILD_TESTING
struct ImGuiTestEngine;
#endif

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
    void ApplyEventForTest(Event event);
    void ShowDropConfirmationForTest(const std::string& source, const std::string& target, int action);
    void ShowWorkspaceRenameForTest();
    void SetSnapshotForTest(RepoSnapshot snapshot);
    void ClearSnapshotForTest();
    void AddRecentForTest(const std::string& path);
    void ProcessEventForTest(SDL_Event event);
    static std::vector<std::string> SplitLinesForTest(const std::string& text);
    static std::string DeltaNameForTest(git_delta_t status);
    static bool ContainsInsensitiveForTest(const std::string& text, const std::string& query);
    static unsigned int DiffMarkerColorForTest(const std::string& line);
    static unsigned int IdColorForTest(bool change_id, bool working_copy);
    static std::string FileUrlForTest(const std::string& path);
#endif

private:
    enum class Dialog
    {
        None,
        Clone,
        New,
        Commit,
        Describe,
        Metaedit,
        Rebase,
        Squash,
        Split,
        Abandon,
        Restore,
        Bookmark,
        Tag,
        WorkspaceAdd,
        WorkspaceRename,
        Credentials,
        ConfirmDrop,
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
    };

    bool Initialize();
    void Shutdown();
    void LoadSettings();
    void SaveSettings();
    void RememberRepository(const std::string& path);
    void PollEngine();
    void ApplyEvent(Event event);
    void ProcessEvent(SDL_Event& event);
    void RenderFrame();
    void RenderMenuBar();
    void RenderToolbar();
    void RenderWelcome();
    void RenderBookmarks();
    void RenderTags();
    void RenderWorkspaces();
    void RenderRemotes();
    void RenderHistory();
    void RenderChanges();
    void RenderDiff();
    void RenderOperations();
    void RenderDialogs();
    void SetupDockspace();
    void RebuildGraph();
    void RebuildIdPrefixes();
    std::size_t RevisionPrefix(const std::string& oid) const;
    std::size_t ChangePrefix(const std::string& id) const;
    std::size_t OperationPrefix(const std::string& oid) const;
    void SelectRevision(const std::string& oid);
    void SelectFile(const std::string& path);
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
    DiffResult _diff;
    std::vector<int> _visible_revisions;
    std::vector<GraphRow> _graph_rows;
    std::uint64_t _graph_generation = 0;
    std::string _graph_filter;
    std::string _built_filter;
    std::unordered_map<std::string, std::size_t> _revision_prefixes;
    std::unordered_map<std::string, std::size_t> _change_prefixes;
    std::unordered_map<std::string, std::size_t> _operation_prefixes;
    std::string _selected_revision;
    std::string _selected_file;
    std::string _preferred_file;
    std::vector<std::string> _recent_repositories;
    std::filesystem::path _settings_path;
    std::string _imgui_ini_path;
    std::string _status_message;
    std::string _error_message;
    std::string _active_operation;
    std::string _progress_phase;
    std::size_t _progress_completed = 0;
    std::size_t _progress_total = 0;
    CredentialRequest _credential_request;
    PendingDrop _pending_drop;

    Dialog _dialog = Dialog::None;
    std::string _input_primary;
    std::string _input_secondary;
    std::string _input_tertiary;
    std::string _input_filesets;
    bool _input_flag = false;
    int _input_mode = 0;

    SDL_Window* _window = nullptr;
    SDL_GLContext _gl_context = nullptr;
    bool _running = true;
    bool _default_layout = true;
    bool _dark_theme = true;
    bool _show_bookmarks = true;
    bool _show_tags = true;
    bool _show_workspaces = true;
    bool _show_remotes = true;
    bool _show_history = true;
    bool _show_changes = true;
    bool _show_diff = true;
    bool _show_operations = false;
#ifdef IMGUI_BUILD_TESTING
    ImGuiTestEngine* _test_engine = nullptr;
    bool _test_mode = false;
    bool _test_snapshot_mode = false;
    bool _smoke_mode = false;
    int _test_result = 1;
#endif
};

} // namespace Ggui

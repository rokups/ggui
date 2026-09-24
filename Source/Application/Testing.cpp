// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#ifdef IMGUI_BUILD_TESTING
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

#ifdef IMGUI_BUILD_TESTING
void RegisterUiTests(ImGuiTestEngine* engine);

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
    if (_test_snapshot_mode)
        ResetRepositoryState();
    _test_snapshot_mode = false;
#ifdef GGUI_TESTING
    _engine.SetCommandsSuppressedForTest(false);
#endif
    EnqueueAction(OpenRepository{path});
}

void Application::RefreshForTest()
{
    EnqueueAction(Refresh{true, {}, true});
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

const std::vector<std::string>& Application::VisibleBranchesForTest() const
{
    return _visible_branches;
}

const std::vector<std::string>& Application::SelectedTagsForTest() const
{
    return _selected_tags;
}

const std::vector<std::string>& Application::SelectedRemotesForTest() const
{
    return _selected_remotes;
}

std::vector<std::string> Application::VisibleHistoryRevisionsForTest() const
{
    std::vector<std::string> result;
    if (_snapshot == nullptr) return result;
    result.reserve(_visible_revisions.size());
    for (const int index : _visible_revisions)
        if (index >= 0 && static_cast<std::size_t>(index) < _history_revisions.size())
            result.push_back(_history_revisions[static_cast<std::size_t>(index)].oid);
    return result;
}

std::size_t Application::RenderedHistoryRowsForTest() const
{
    return _rendered_history_rows;
}

const std::string& Application::ActiveOperationForTest() const
{
    return _active_operation;
}

std::vector<std::string> Application::SelectedFilesForTest() const
{
    std::vector<std::string> result;
    for (const StatusEntry* file : SelectedChangedFiles())
        result.push_back(file->path);
    return result;
}

const std::string& Application::FocusedFileForTest() const
{
    return _selected_file;
}

void Application::RequestBlameForTest(const std::string& revision, const std::string& path)
{
    RequestBlame(revision, path);
}

std::pair<std::string, std::string> Application::BlameLocationForTest() const
{
    return {_blame_revision, _blame_path};
}

bool Application::DiffSideBySideForTest() const
{
    return _diff_side_by_side;
}

DiffWhitespaceMode Application::DiffWhitespaceModeForTest() const
{
    return _diff_whitespace_mode;
}

int Application::DiffContextLinesForTest() const
{
    return _diff_context_lines;
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

const std::string& Application::DialogDestinationForTest() const
{
    return _input_primary;
}

const std::string& Application::DialogDescriptionForTest() const
{
    return _input_primary;
}

const MoveDiffLines& Application::PendingMoveDiffLinesForTest() const
{
    return std::get<MoveDiffLines>(_pending_commands.front());
}

std::string Application::RebaseSourceForTest() const
{
    const Revision* source = RebaseSource();
    return source == nullptr ? "" : source->oid;
}

bool Application::DialogModifiesLockedCommitForTest() const
{
    return DialogModifiesLockedCommit();
}

void Application::ApplyEventForTest(Event event)
{
    ApplyEvent(std::move(event));
}

void Application::ShowDropConfirmationForTest(
    const std::string& source, const std::string& target, int action, bool entire_branch)
{
    const DropAction drop_action = static_cast<DropAction>(std::clamp(action, 0, 3));
    _pending_drop = {source, target, drop_action, entire_branch};
    OpenDialog(Dialog::ConfirmDrop);
}

std::pair<int, bool> Application::PendingDropActionForTest() const
{
    return {static_cast<int>(_pending_drop.action), _pending_drop.entire_branch};
}

bool Application::PendingDropCopyForTest() const
{
    return _pending_drop.copy;
}

void Application::ShowWorkspaceRenameForTest()
{
    OpenDialog(Dialog::WorkspaceRename);
}

void Application::ShowBranchRenameForTest(const std::string& name)
{
    OpenDialog(Dialog::BranchRename);
    _input_primary = name;
    _input_secondary = name;
}

void Application::SetSnapshotForTest(RepoSnapshot snapshot)
{
    _test_snapshot_mode = true;
    _pending_created_branch.clear();
#ifdef GGUI_TESTING
    _engine.SetCommandsSuppressedForTest(true);
#endif
    std::vector<Revision> revisions = std::move(snapshot.revisions);
    // Synthetic fixtures may list parents first; history views require children
    // first. Preserve fixture order wherever the graph does not constrain it.
    std::vector<Revision> ordered_revisions;
    ordered_revisions.reserve(revisions.size());
    std::unordered_map<std::string, std::size_t> revision_indices;
    for (std::size_t index = 0; index < revisions.size(); ++index)
        if (!revision_indices.emplace(revisions[index].oid, index).second)
            throw std::invalid_argument("test history contains a duplicate revision");
    std::vector<std::size_t> child_counts(revisions.size());
    for (const Revision& revision : revisions)
        for (const std::string& parent : revision.parents)
            if (const auto found = revision_indices.find(parent); found != revision_indices.end())
                ++child_counts[found->second];
    std::set<std::size_t> ready;
    for (std::size_t index = 0; index < revisions.size(); ++index)
        if (child_counts[index] == 0) ready.insert(index);
    while (!ready.empty())
    {
        const std::size_t index = *ready.begin();
        ready.erase(ready.begin());
        for (const std::string& parent : revisions[index].parents)
            if (const auto found = revision_indices.find(parent); found != revision_indices.end())
                if (--child_counts[found->second] == 0) ready.insert(found->second);
        ordered_revisions.push_back(std::move(revisions[index]));
    }
    if (ordered_revisions.size() != revisions.size())
        throw std::invalid_argument("test history contains a cycle");
    revisions = std::move(ordered_revisions);
    if (snapshot.repository_generation == 0)
        snapshot.repository_generation = snapshot.generation == 0 ? 1 : snapshot.generation;
    _snapshot = std::make_shared<RepoSnapshot>(std::move(snapshot));
    _history_refs_by_revision.clear();
    for (std::size_t index = 0; index < _snapshot->refs.size(); ++index)
        _history_refs_by_revision[_snapshot->refs[index].target].push_back(index);
    auto view = std::make_shared<HistoryView>();
    view->repository_generation = _snapshot->repository_generation;
    view->request = ++_history_applied_request;
    std::unordered_set<std::string> ids;
    for (const Revision& revision : revisions) ids.insert(revision.oid);
    for (Revision& revision : revisions)
    {
        HistoryItem item;
        item.id = revision.oid;
        item.kind = HistoryItemKind::Commit;
        item.revision = std::move(revision);
        for (const std::string& parent : item.revision.parents)
            if (ids.contains(parent)) item.parents.push_back(parent);
        view->items.push_back(std::move(item));
    }
    _history_view = view;
    _history_revisions.clear();
    _visible_revisions.clear();
    std::vector<GraphNode> nodes;
    for (const HistoryItem& item : view->items)
    {
        _visible_revisions.push_back(static_cast<int>(_history_revisions.size()));
        _history_revisions.push_back(item.revision);
        nodes.push_back({item.id, item.parents});
    }
    _graph_rows = BuildGraphLayout(nodes, _snapshot->working_copy);
    _history_search_scrolled_request = 0;
    _history_hovered_track = -1;
    _history_hovered_commit_row = -1;
    RebuildIdPrefixes();
    const std::string& current = CurrentCommit(*_snapshot);
    _selected_revision = current.empty()
        ? (_history_revisions.empty() ? "" : _history_revisions.front().oid)
        : current;
    _selected_revisions = _selected_revision.empty() ? std::vector<std::string>{}
                                                     : std::vector{_selected_revision};
    _selected_file.clear();
    _preferred_file.clear();
    _pending_revision.clear();
    _pending_editor_revision.clear();
    _pending_editor_path.clear();
    _opened_editor_path_for_test.clear();
    _compare_to.clear();
    _file_comparison = false;
    _diff = {_snapshot->generation, _selected_revision, {}, {}, {}, false, _snapshot->status};
    _diff_loading = false;
    ClearBlame();
    _graph_generation = 0;
    _graph_filter.clear();
    _reflog_filter.clear();
    _history_observed_filter.clear();
    _history_requested_filter.clear();
    _history_anchor.clear();
    _history_expansion_pending.clear();
    _visible_branches.clear();
    _visible_branches_user_selected = false;
    _selected_tags.clear();
    _selected_remotes.clear();
    _selected_remotes_user_selected = false;
    _built_branches.clear();
}

bool Application::HistoryLoadPendingForTest() const
{
    return !_reveal_revision.empty() && !RevealRevisionLoaded();
}

bool Application::HistoryExpansionPendingForTest() const
{
    return !_history_expansion_pending.empty();
}

void Application::CancelHistorySearchForTest()
{
    CancelHistorySearch();
}

const std::string& Application::ErrorMessageForTest() const
{
    return _error_message;
}

const std::vector<Revision>& Application::HistoryRevisionsForTest() const
{
    return _history_revisions;
}

void Application::CreateChangeForTest(const std::string& parent)
{
    CreateChange(parent);
}

void Application::ClearSnapshotForTest()
{
    ResetRepositoryState();
}

void Application::AddRecentForTest(const std::string& path)
{
    RememberRepository(path);
}

const std::vector<std::string>& Application::RecentRepositoriesForTest() const
{
    return _recent_repositories;
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

unsigned int Application::IdColorForTest(bool working_copy)
{
    return CommitIdColor(working_copy);
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

unsigned int Application::BranchColorForTest(const std::string& name, const std::vector<NamedRef>& refs)
{
    return BranchBadgeColor(name, refs);
}

std::string Application::FormatTimestampForTest(std::int64_t timestamp)
{
    return FormatTimestamp(timestamp);
}

int Application::DropPlacementForTest(int action)
{
    return DropPlacement(static_cast<DropAction>(std::clamp(action, 0, 3)));
}

std::string Application::DropTooltipForTest(int action, bool entire_branch, bool copy)
{
    return std::string(
        DropTooltip(static_cast<DropAction>(std::clamp(action, 0, 3)), entire_branch, copy));
}

const std::string& Application::PendingEditorRevisionForTest() const
{
    return _pending_editor_revision;
}

const std::filesystem::path& Application::OpenedEditorPathForTest() const
{
    return _opened_editor_path_for_test;
}
#endif

} // namespace Ggui

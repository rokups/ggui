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
#include <stdexcept>
#include <string>
#include <tuple>
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
    _test_snapshot_mode = false;
#ifdef GGUI_TESTING
    _engine.SetCommandsSuppressedForTest(false);
#endif
    _engine.Enqueue(OpenRepository{path});
}

void Application::RefreshForTest()
{
    _engine.Enqueue(Refresh{});
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

const std::vector<std::string>& Application::VisibleBookmarksForTest() const
{
    return _visible_bookmarks;
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

void Application::ShowBookmarkRenameForTest(const std::string& name)
{
    OpenDialog(Dialog::BookmarkRename);
    _input_primary = name;
    _input_secondary = name;
}

void Application::SetSnapshotForTest(RepoSnapshot snapshot)
{
    _test_snapshot_mode = true;
#ifdef GGUI_TESTING
    _engine.SetCommandsSuppressedForTest(true);
#endif
    std::vector<Revision> revisions = std::move(snapshot.revisions);
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
    _graph_rows = BuildGraphLayout(nodes);
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
    _graph_generation = 0;
    _graph_filter.clear();
    _history_observed_filter.clear();
    _history_requested_filter.clear();
    _history_anchor.clear();
    _history_expansion_pending.clear();
    _history_expansion_feedback_until = {};
    _visible_bookmarks.clear();
    _visible_bookmarks_user_selected = false;
    _selected_tags.clear();
    _selected_remotes.clear();
    _selected_remotes_user_selected = false;
    _built_bookmarks.clear();
}

bool Application::HistoryLoadPendingForTest() const
{
    return !_reveal_revision.empty() && !RevealRevisionLoaded();
}

bool Application::HistoryExpansionPendingForTest() const
{
    return !_history_expansion_pending.empty();
}

bool Application::HistoryExpansionFeedbackForTest() const
{
    return !_history_expansion_pending.empty()
        || std::chrono::steady_clock::now() < _history_expansion_feedback_until;
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

std::string Application::ClosestBookmarkForTest(const RepoSnapshot& snapshot, const std::string& revision)
{
    const NamedRef* bookmark = ClosestBookmark(snapshot, revision);
    return bookmark == nullptr ? "" : ReferenceLabel(*bookmark);
}

unsigned int Application::BookmarkColorForTest(const std::string& name, const std::vector<NamedRef>& refs)
{
    return BookmarkBadgeColor(name, refs);
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

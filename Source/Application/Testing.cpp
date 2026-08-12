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

void Application::ShowDropConfirmationForTest(const std::string& source, const std::string& target, int action)
{
    _pending_drop = {source, target, static_cast<DropAction>(std::clamp(action, 0, 3))};
    OpenDialog(Dialog::ConfirmDrop);
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
    _snapshot = std::make_shared<RepoSnapshot>(std::move(snapshot));
    RebuildIdPrefixes();
    _selected_revision = _snapshot->working_copy.empty()
        ? (_snapshot->revisions.empty() ? "" : _snapshot->revisions.front().oid)
        : _snapshot->working_copy;
    _selected_revisions = _selected_revision.empty() ? std::vector<std::string>{}
                                                     : std::vector{_selected_revision};
    _selected_file.clear();
    _preferred_file.clear();
    _pending_revision.clear();
    _compare_to.clear();
    _file_comparison = false;
    _diff = {_snapshot->generation, _selected_revision, {}, {}, {}, false, _snapshot->status};
    _diff_loading = false;
    _graph_generation = 0;
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

std::string Application::DropTooltipForTest(int action, const std::string& target)
{
    return DropTooltip(static_cast<DropAction>(std::clamp(action, 0, 3)), target);
}
#endif

} // namespace Ggui

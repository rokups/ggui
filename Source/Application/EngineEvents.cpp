// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <algorithm>
#include <ranges>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

void Application::PollEngine()
{
#ifdef IMGUI_BUILD_TESTING
    if (_test_snapshot_mode)
    {
        _engine.PollEvents();
        return;
    }
#endif
    for (Event& event : _engine.PollEvents())
        ApplyEvent(std::move(event));
}

void Application::ApplyEvent(Event event)
{
    std::visit(
            [this](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, SnapshotReady>)
                {
                    const std::string old_root = _snapshot == nullptr ? "" : _snapshot->root;
                    const std::string old_current = _snapshot == nullptr ? "" : CurrentCommit(*_snapshot);
                    const std::string old_selection = _selected_revision;
                    const std::string old_compare_to = _compare_to;
                    _snapshot = std::move(value.snapshot);
                    _history_refs_by_revision.clear();
                    for (std::size_t index = 0; index < _snapshot->refs.size(); ++index)
                        _history_refs_by_revision[_snapshot->refs[index].target].push_back(index);
                    if (!_pending_created_bookmark.empty()
                        && std::ranges::any_of(_snapshot->refs, [this](const NamedRef& ref) {
                            return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK
                                && ref.name == _pending_created_bookmark;
                        }))
                    {
                        _visible_bookmarks = {_pending_created_bookmark};
                        _visible_bookmarks_user_selected = true;
                        _pending_created_bookmark.clear();
                        RememberRepositorySelections();
                    }
                    const bool repository_changed = old_root != _snapshot->root;
                    if (repository_changed)
                    {
                        _history_view.reset();
                        _history_revisions.clear();
                        _visible_revisions.clear();
                        _graph_rows.clear();
                        _history_hovered_track = -1;
                        _history_hovered_commit_row = -1;
                        _history_requested_generation = 0;
                        _history_search_scrolled_request = 0;
                        _history_requested_key.clear();
                        _history_requested_filter.clear();
                        _history_anchor.clear();
                        _history_expansion_pending.clear();
                        _history_expansion_feedback_until = {};
                        RestoreRepositorySelections(_snapshot->root);
                        _selected_revision = CurrentCommit(*_snapshot);
                        _selected_revisions = _selected_revision.empty() ? std::vector<std::string>{}
                                                                        : std::vector{_selected_revision};
                        _preferred_file.clear();
                        _compare_to.clear();
                        _file_comparison = false;
                    }
                    else
                    {
                        const std::string current = CurrentCommit(*_snapshot);
                        if (old_current != current && _selected_revision == old_current)
                        {
                            _selected_revision = current;
                            _selected_revisions = current.empty() ? std::vector<std::string>{}
                                                                 : std::vector{current};
                        }
                    }
                    SDL_SetWindowTitle(_window, (RepositoryName(_snapshot->root) + " - ggui").c_str());
                    RememberRepository(_snapshot->root);
                    UpdateGraphBuild();
                    if (!_compare_to.empty())
                    {
                        if (_snapshot->working_copy.empty() || _selected_revision == _snapshot->working_copy)
                        {
                            _compare_to.clear();
                            _file_comparison = false;
                        }
                        else _compare_to = _snapshot->working_copy;
                    }
                    if (repository_changed || old_selection != _selected_revision || old_compare_to != _compare_to)
                        RequestDiff(true);
                }
                else if constexpr (std::is_same_v<T, HistoryReady>)
                {
                    if (_snapshot == nullptr || value.view == nullptr
                        || value.view->repository_generation != _snapshot->repository_generation
                        || value.view->search != _graph_filter
                        || value.view->request < _history_applied_request)
                        return;
                    _history_applied_request = value.view->request;
                    _history_view = std::move(value.view);
                    _history_hovered_track = -1;
                    _history_hovered_commit_row = -1;
                    if (!_history_view->skeleton && !_history_expansion_pending.empty())
                    {
                        _history_expansion_pending.clear();
                        // Fast expansions can finish between two rendered
                        // frames. Keep a short completion acknowledgement so
                        // the click never appears to have done nothing.
                        _history_expansion_feedback_until = std::chrono::steady_clock::now()
                            + std::chrono::seconds(2);
                    }
                    _history_revisions.clear();
                    _visible_revisions.clear();
                    std::vector<GraphNode> nodes;
                    nodes.reserve(_history_view->items.size());
                    for (const HistoryItem& item : _history_view->items)
                    {
                        nodes.push_back({item.id, item.parents});
                        if (item.kind == HistoryItemKind::Commit)
                        {
                            _visible_revisions.push_back(static_cast<int>(_history_revisions.size()));
                            _history_revisions.push_back(item.revision);
                        }
                        else _visible_revisions.push_back(-1);
                    }
                    try { _graph_rows = BuildGraphLayout(nodes, _snapshot->working_copy); }
                    catch (const std::exception& error)
                    {
                        _error_message = error.what();
                        _graph_rows.clear();
                    }
                    _graph_generation = _snapshot->repository_generation;
                    RebuildIdPrefixes();
                    // A bounded history view cannot prove that a missing
                    // selection was removed. It can identify a rewritten
                    // selection when a loaded commit carries its old ID.
                    std::unordered_map<std::string, std::string> current_ids;
                    for (const Revision& revision : _history_revisions)
                        current_ids.emplace(revision.oid, revision.oid);
                    for (const Revision& revision : _history_revisions)
                        for (const std::string& alias : revision.aliases)
                            current_ids.try_emplace(alias, revision.oid);
                    const auto current_id = [&](const std::string& id) {
                        const auto found = current_ids.find(id);
                        return found == current_ids.end() ? id : found->second;
                    };
                    const std::string old_selection = _selected_revision;
                    const std::string old_compare_to = _compare_to;
                    _selected_revision = current_id(_selected_revision);
                    std::vector<std::string> selections;
                    for (const std::string& selected : _selected_revisions)
                    {
                        std::string id = current_id(selected);
                        if (std::ranges::find(selections, id) == selections.end())
                            selections.push_back(std::move(id));
                    }
                    _selected_revisions = std::move(selections);
                    _compare_to = current_id(_compare_to);
                    if (!_compare_to.empty()
                        && (_selected_revision == _snapshot->working_copy || _selected_revision == _compare_to))
                    {
                        _compare_to.clear();
                        _file_comparison = false;
                    }
                    _history_anchor = current_id(_history_anchor);
                    if (!_history_view->skeleton && !_history_anchor.empty())
                    {
                        const auto found = std::ranges::find_if(_history_view->items, [this](const HistoryItem& item) {
                            return item.kind == HistoryItemKind::Commit && item.revision.oid == _history_anchor;
                        });
                        if (found != _history_view->items.end())
                        {
                            _history_scroll_target = static_cast<float>(found - _history_view->items.begin()) * kRowHeight;
                            _history_scroll_frames = 3;
                        }
                        _history_anchor.clear();
                    }
                    else if (!_history_view->search.empty()
                        && (_history_view->skeleton
                            || _history_search_scrolled_request != _history_view->request))
                    {
                        const auto match = std::ranges::find_if(_history_view->items,
                            [](const HistoryItem& item) { return item.search_match; });
                        if (match != _history_view->items.end())
                        {
                            _history_scroll_target = static_cast<float>(match - _history_view->items.begin())
                                * kRowHeight;
                            _history_scroll_frames = 3;
                            if (!_history_view->skeleton)
                                _history_search_scrolled_request = _history_view->request;
                        }
                    }
                    if (_selected_revision.empty() && !_history_revisions.empty())
                    {
                        _selected_revision = _history_revisions.front().oid;
                        _selected_revisions = {_selected_revision};
                    }
                    if (old_selection != _selected_revision || old_compare_to != _compare_to)
                        RequestDiff(true);
                }
                else if constexpr (std::is_same_v<T, ChangedFilesReady>)
                {
                    if (value.revision != _selected_revision || value.compare_to != _compare_to
                        || value.file_comparison != _file_comparison
                        || value.options.whitespace_mode != _diff_whitespace_mode
                        || value.options.context_lines != _diff_context_lines)
                        return;
                    if (!_pending_revision.empty() || _selected_file.empty())
                    {
                        _selected_file = value.selected_path;
                        if (_preferred_file.empty()) _preferred_file = _selected_file;
                        _pending_revision.clear();
                    }
                    _diff.files = std::move(value.files);
                }
                else if constexpr (std::is_same_v<T, DiffReady>)
                {
                    if (value.diff.revision != _selected_revision || value.diff.compare_to != _compare_to
                        || value.diff.file_comparison != _file_comparison
                        || value.diff.options.whitespace_mode != _diff_whitespace_mode
                        || value.diff.options.context_lines != _diff_context_lines)
                        return;
                    if (value.diff.revision == _pending_revision || _selected_file.empty())
                    {
                        _selected_file = value.diff.path;
                        if (_preferred_file.empty())
                            _preferred_file = _selected_file;
                        _pending_revision.clear();
                        _diff = std::move(value.diff);
                        _diff_loading = false;
                    }
                    else if (value.diff.path == _selected_file)
                    {
                        _diff = std::move(value.diff);
                        _diff_loading = false;
                    }
                }
                else if constexpr (std::is_same_v<T, FileContentReady>)
                    OpenTemporaryFileInEditor(value);
                else if constexpr (std::is_same_v<T, BackgroundActivityStarted>)
                    _background_activities.insert_or_assign(value.id, std::move(value.name));
                else if constexpr (std::is_same_v<T, BackgroundActivityFinished>)
                    _background_activities.erase(value.id);
                else if constexpr (std::is_same_v<T, OperationStarted>)
                {
                    _active_operation = value.name;
                    _error_message.clear();
                    _status_message.clear();
                    _progress_phase.clear();
                    _progress_completed = 0;
                    _progress_total = 0;
                    if (value.name == "open" || value.name == "init" || value.name == "clone")
                    {
                        _snapshot.reset();
                        SDL_SetWindowTitle(_window, "Opening repository - ggui");
                    }
                }
                else if constexpr (std::is_same_v<T, OperationProgress>)
                {
                    _progress_phase = value.phase;
                    _progress_completed = value.completed;
                    _progress_total = value.total;
                }
                else if constexpr (std::is_same_v<T, OperationFinished>)
                {
                    if (value.name == "close")
                        ResetRepositoryState();
                    else
                        _status_message = value.name + " completed";
                    _active_operation.clear();
                    _progress_phase.clear();
                }
                else if constexpr (std::is_same_v<T, ErrorEvent>)
                {
                    _error_message = value.message;
                    if (value.operation == _active_operation)
                    {
                        _active_operation.clear();
                        _progress_phase.clear();
                    }
                    if (value.operation == "diff")
                        _diff_loading = false;
                    if (value.operation == "history")
                        _history_expansion_pending.clear();
                    if (value.operation == "bookmark")
                        _pending_created_bookmark.clear();
                }
                else if constexpr (std::is_same_v<T, WorkspaceRemoved>)
                {
                    ForgetRepository(value.root);
                    if (value.current) ResetRepositoryState();
                }
                else if constexpr (std::is_same_v<T, CredentialRequest>)
                {
                    _credential_request = std::move(value);
                    OpenDialog(Dialog::Credentials);
                }
            },
            event);
}

} // namespace Ggui

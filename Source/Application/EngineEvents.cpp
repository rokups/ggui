// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <algorithm>
#include <ranges>
#include <string>
#include <type_traits>
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
                    const bool had_snapshot = _snapshot != nullptr;
                    const std::string old_root = _snapshot == nullptr ? "" : _snapshot->root;
                    const std::string old_current = _snapshot == nullptr ? "" : CurrentCommit(*_snapshot);
                    const std::string old_selection = _selected_revision;
                    const std::string old_compare_to = _compare_to;
                    const bool had_selection = !_selected_revisions.empty();
                    _snapshot = std::move(value.snapshot);
                    const std::string& current = CurrentCommit(*_snapshot);
                    SDL_SetWindowTitle(_window, (RepositoryName(_snapshot->root) + " - ggui").c_str());
                    RebuildIdPrefixes();
                    RememberRepository(_snapshot->root);
                    _graph_generation = 0;
                    const bool repository_changed = old_root != _snapshot->root;
                    if (repository_changed)
                    {
                        _preferred_file.clear();
                        _compare_to.clear();
                        _file_comparison = false;
                    }
                    if (repository_changed)
                    {
                        _selected_revision = !current.empty() ? current
                            : _snapshot->revisions.empty()    ? ""
                                                              : _snapshot->revisions.front().oid;
                        _selected_revisions = _selected_revision.empty() ? std::vector<std::string>{}
                                                                        : std::vector{_selected_revision};
                    }
                    else
                    {
                        if (old_current.empty() && !current.empty())
                        {
                            _selected_revision = current;
                            _selected_revisions = {_selected_revision};
                        }
                        else if (old_current != current)
                        {
                            const auto old = std::ranges::find(_selected_revisions, old_current);
                            if (old != _selected_revisions.end())
                            {
                                const auto replacement = std::ranges::find(_selected_revisions, current);
                                if (current.empty() || replacement != _selected_revisions.end())
                                    _selected_revisions.erase(old);
                                else
                                    *old = current;
                                if (_selected_revision == old_current)
                                    _selected_revision = current;
                            }
                        }
                        for (std::string& oid : _selected_revisions)
                        {
                            if (std::ranges::any_of(
                                    _snapshot->revisions, [&](const Revision& revision) { return revision.oid == oid; }))
                                continue;
                            const auto replacement = std::ranges::find_if(_snapshot->revisions,
                                [&](const Revision& revision) {
                                    return std::ranges::find(revision.aliases, oid) != revision.aliases.end();
                                });
                            if (replacement != _snapshot->revisions.end())
                            {
                                if (_selected_revision == oid)
                                    _selected_revision = replacement->oid;
                                oid = replacement->oid;
                            }
                        }
                        std::vector<std::string> unique_selection;
                        for (const std::string& oid : _selected_revisions)
                            if (std::ranges::find(unique_selection, oid) == unique_selection.end())
                                unique_selection.push_back(oid);
                        _selected_revisions = std::move(unique_selection);
                        std::erase_if(_selected_revisions, [this](const std::string& oid) {
                            return std::ranges::none_of(
                                _snapshot->revisions, [&](const Revision& revision) { return revision.oid == oid; });
                        });
                        if (std::ranges::find(_selected_revisions, _selected_revision) == _selected_revisions.end())
                        {
                            if (!_selected_revisions.empty())
                                _selected_revision = _selected_revisions.back();
                            else
                            {
                                _selected_revision = had_selection && !current.empty()
                                    ? current
                                    : had_selection && !_snapshot->revisions.empty()
                                    ? _snapshot->revisions.front().oid
                                    : "";
                                if (!_selected_revision.empty())
                                    _selected_revisions.push_back(_selected_revision);
                            }
                        }
                    }
                    if (!_compare_to.empty())
                    {
                        if (_snapshot->working_copy.empty() || _selected_revision == _snapshot->working_copy)
                        {
                            _compare_to.clear();
                            _file_comparison = false;
                        }
                        else
                            _compare_to = _snapshot->working_copy;
                    }
                    const bool selection_changed = !had_snapshot || old_selection != _selected_revision;
                    if (repository_changed || selection_changed || old_compare_to != _compare_to)
                        RequestDiff(true);
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
                    _active_operation.clear();
                    _progress_phase.clear();
                    if (value.operation == "diff")
                        _diff_loading = false;
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

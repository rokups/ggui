// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <nfd.h>

#include <filesystem>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

void Application::SelectRevision(const std::string& oid, bool additive)
{
    if (_snapshot != nullptr && oid == _snapshot->working_copy)
    {
        _compare_to.clear();
        _file_comparison = false;
    }
    const auto selected = std::ranges::find(_selected_revisions, oid);
    if (!additive)
        _selected_revisions = {oid};
    else if (selected == _selected_revisions.end())
        _selected_revisions.push_back(oid);
    else
        _selected_revisions.erase(selected);

    _selected_revision = std::ranges::find(_selected_revisions, oid) != _selected_revisions.end()
        ? oid
        : _selected_revisions.empty() ? ""
                                      : _selected_revisions.back();
    if (_selected_revision.empty())
    {
        _compare_to.clear();
        _file_comparison = false;
    }
    RequestDiff(true);
}

void Application::RequestDiff(bool fallback_to_first)
{
    if (_selected_revision.empty())
    {
        _selected_file.clear();
        _pending_revision.clear();
        _diff = {};
        _diff_loading = false;
    }
    else
    {
        _pending_revision = fallback_to_first ? _selected_revision : "";
        _diff_loading = true;
        _engine.Enqueue(LoadDiff{_selected_revision, fallback_to_first ? _preferred_file : _selected_file,
            fallback_to_first, {_diff_whitespace_mode, _diff_context_lines}, _compare_to, _file_comparison});
    }
}

void Application::ToggleComparison(bool file_comparison)
{
    if (!_compare_to.empty() && _file_comparison == file_comparison)
    {
        _compare_to.clear();
        _file_comparison = false;
    }
    else if (_snapshot != nullptr && !_selected_revision.empty() && !_snapshot->working_copy.empty()
        && _selected_revision != _snapshot->working_copy)
    {
        _compare_to = _snapshot->working_copy;
        _file_comparison = file_comparison;
    }
    RequestDiff(true);
}

void Application::RevealRevision(const std::string& oid)
{
    _graph_filter.clear();
    _show_history = true;
    _reveal_revision = oid;
    SelectRevision(oid);
}

bool Application::CanCreateChange() const
{
    return _snapshot != nullptr && !_selected_revisions.empty()
        && std::ranges::all_of(_selected_revisions, [this](const std::string& oid) {
               return std::ranges::any_of(
                   _snapshot->revisions, [&](const Revision& revision) { return revision.oid == oid; });
           });
}

std::vector<std::string> Application::SelectedParentRevisions() const
{
    std::vector<std::string> parents;
    parents.reserve(_selected_revisions.size());
    for (const std::string& oid : _selected_revisions)
    {
        if (oid == _snapshot->working_copy)
        {
            parents.emplace_back("@");
            continue;
        }
        parents.push_back(oid);
    }
    return parents;
}

void Application::CreateChange(bool force_child)
{
    if (!_active_operation.empty())
        return;
    const auto selected = _selected_revisions.size() == 1
        ? std::ranges::find(_snapshot->revisions, _selected_revisions.front(), &Revision::oid)
        : _snapshot->revisions.end();
    if (!force_child && selected != _snapshot->revisions.end()
        && selected->oid == _snapshot->working_copy && selected->empty)
    {
        _engine.Enqueue(Refresh{});
        return;
    }
    _engine.Enqueue(NewChange{{}, SelectedParentRevisions(), {}, {}, false});
}

bool Application::IsLocked(const std::string& identifier) const
{
    if (_snapshot == nullptr || identifier.empty())
        return false;
    std::string oid = identifier == "@" ? _snapshot->working_copy : identifier;
    const auto ref = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& candidate) {
        return candidate.name == oid;
    });
    if (ref != _snapshot->refs.end())
        oid = ref->target;
    const auto revision = std::ranges::find_if(_snapshot->revisions, [&](const Revision& candidate) {
        return candidate.oid == oid || std::ranges::find(candidate.aliases, oid) != candidate.aliases.end();
    });
    return revision != _snapshot->revisions.end() && revision->pushed;
}

bool Application::DialogModifiesLockedCommit() const
{
    switch (_dialog)
    {
    case Dialog::Commit: return IsLocked(_snapshot->working_copy);
    case Dialog::Metaedit:
    case Dialog::Split: return IsLocked(_selected_revision);
    case Dialog::Abandon:
    {
        const std::vector<std::string> revisions =
            AbandonRevisions(_selected_revision, _input_flag_tertiary);
        return std::ranges::any_of(
            revisions, [this](const std::string& revision) { return IsLocked(revision); });
    }
    case Dialog::Rebase:
    {
        const Revision* source = RebaseSource();
        return IsLocked(source == nullptr ? _selected_revision : source->oid);
    }
    case Dialog::Squash:
    {
        std::string destination = _input_secondary;
        if (destination.empty())
        {
            const auto source = std::ranges::find(_snapshot->revisions, _selected_revision, &Revision::oid);
            if (source != _snapshot->revisions.end() && !source->parents.empty())
                destination = source->parents.front();
        }
        return IsLocked(_selected_revision) || IsLocked(destination);
    }
    case Dialog::Restore: return IsLocked(_selected_revision) || IsLocked(_input_primary);
    case Dialog::ConfirmDrop: return IsLocked(_pending_drop.source) || IsLocked(_pending_drop.target);
    case Dialog::ConfirmLocked: return true;
    default: return false;
    }
}

const Revision* Application::RebaseSource() const
{
    if (_snapshot == nullptr)
        return nullptr;
    return _input_flag ? RebaseBranchRoot(*_snapshot, _selected_revision, _input_primary)
                       : ResolveSnapshotRevision(*_snapshot, _selected_revision);
}

void Application::QueueCommands(
    std::vector<Command> commands, const std::vector<std::string>& revisions, std::string warning)
{
    if (!_active_operation.empty())
        return;
    if (std::ranges::none_of(revisions, [this](const std::string& revision) { return IsLocked(revision); }))
    {
        for (Command& command : commands)
            _engine.Enqueue(std::move(command));
        return;
    }
    _pending_commands = std::move(commands);
    _locked_warning = std::move(warning);
    OpenDialog(Dialog::ConfirmLocked);
}

std::vector<std::string> Application::AbandonRevisions(
    const std::string& selected, bool include_descendants) const
{
    std::vector<std::string> result{selected};
    if (!include_descendants)
        return result;
    bool added = true;
    while (added)
    {
        added = false;
        for (const Revision& revision : _snapshot->revisions)
        {
            if (std::ranges::find(result, revision.oid) != result.end())
                continue;
            if (std::ranges::any_of(revision.parents, [&](const std::string& parent) {
                    return std::ranges::find(result, parent) != result.end();
                }))
            {
                result.push_back(revision.oid);
                added = true;
            }
        }
    }
    return result;
}

std::vector<RemoteBookmarkDelete> Application::RemoteBookmarksAt(
    const std::vector<std::string>& revisions) const
{
    std::vector<RemoteBookmarkDelete> result;
    for (const NamedRef& local : _snapshot->refs)
    {
        if (local.kind != GG_NAMED_REF_LOCAL_BOOKMARK
            || std::ranges::find(revisions, local.target) == revisions.end())
            continue;
        for (const NamedRef& remote : _snapshot->refs)
        {
            if (remote.kind != GG_NAMED_REF_REMOTE_BOOKMARK || remote.name != local.name
                || remote.target != local.target || remote.remote.empty())
                continue;
            const RemoteBookmarkDelete deletion{local.name, remote.remote};
            if (std::ranges::none_of(result, [&](const RemoteBookmarkDelete& existing) {
                    return existing.bookmark == deletion.bookmark && existing.remote == deletion.remote;
                }))
                result.push_back(deletion);
        }
    }
    return result;
}

void Application::RequestAbandon(const std::string& revision)
{
    if (!_active_operation.empty() || revision.empty())
        return;
    if (_selected_revision != revision)
        SelectRevision(revision);
    const auto selected = std::ranges::find(_snapshot->revisions, revision, &Revision::oid);
    const bool has_refs = std::ranges::any_of(
        _snapshot->refs, [&](const NamedRef& ref) { return ref.target == revision; });
    if (selected != _snapshot->revisions.end() && selected->empty && !has_refs && !selected->pushed)
        _engine.Enqueue(Abandon{{revision}, false, false, {}});
    else
    {
        OpenDialog(Dialog::Abandon);
        _input_flag = std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.target == revision;
        });
    }
}

bool Application::CanSubmitDialog() const
{
    switch (_dialog)
    {
    case Dialog::Clone: return HasText(_input_primary) && HasText(_input_secondary);
    case Dialog::Rebase: return HasText(_input_primary);
    case Dialog::Split: return HasText(_input_filesets);
    case Dialog::Bookmark:
        return HasText(_input_primary);
    case Dialog::BookmarkRename:
        return HasText(_input_primary) && _input_primary != _input_secondary && _snapshot != nullptr
            && std::ranges::none_of(_snapshot->refs, [this](const NamedRef& ref) {
                   return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == _input_primary;
               });
    case Dialog::Tag:
    case Dialog::WorkspaceAdd:
    case Dialog::WorkspaceRename: return HasText(_input_primary);
    case Dialog::RemoteAdd: return HasText(_input_primary) && HasText(_input_secondary);
    case Dialog::PushTo: return HasText(_input_primary) && HasText(_input_secondary);
    case Dialog::Credentials:
        return HasText(_input_primary) && (_input_mode == 1
            || (_input_mode == 2 ? HasText(_input_secondary) : HasText(_input_filesets)));
    case Dialog::None: return false; // GCOV_EXCL_LINE: RenderDialogs returns before validation
    default: return true;
    }
}

gg_reorder_placement Application::DropPlacement(DropAction action)
{
    return action == DropAction::ReorderAfter ? GG_REORDER_BEFORE : GG_REORDER_AFTER;
}

std::string Application::DropTooltip(DropAction action, std::string_view target)
{
    const std::string_view verb = action == DropAction::ReorderBefore ? "Move before "
        : action == DropAction::ReorderAfter                           ? "Move after "
        : action == DropAction::Squash                                 ? "Squash into "
                                                                       : "Rebase onto ";
    return std::string(verb) + std::string(target);
}

void Application::SelectFile(const std::string& path)
{
    _selected_file = path;
    _preferred_file = path;
    _pending_revision.clear();
    if (!_selected_revision.empty())
    {
        RequestDiff(false);
    }
}

std::pair<std::string, std::string> Application::AdjacentRevisions(const std::string& oid) const
{
    std::string parent;
    std::string child;
    if (_snapshot == nullptr)
        return {parent, child};
    const auto source = std::ranges::find(_snapshot->revisions, oid, &Revision::oid);
    if (source == _snapshot->revisions.end())
        return {parent, child};
    if (source->parents.size() == 1)
        parent = source->parents.front();
    int children = 0;
    for (const Revision& revision : _snapshot->revisions)
    {
        if (std::ranges::find(revision.parents, source->oid) == revision.parents.end())
            continue;
        child = revision.oid;
        ++children;
    }
    if (children != 1)
        child.clear();
    return {parent, child};
}

void Application::ResetRepositoryState()
{
    _snapshot.reset();
    _diff = {};
    _visible_revisions.clear();
    _graph_rows.clear();
    _graph_generation = 0;
    _revision_prefixes.clear();
    _operation_prefixes.clear();
    _selected_revision.clear();
    _selected_revisions.clear();
    _selected_file.clear();
    _preferred_file.clear();
    _pending_revision.clear();
    _compare_to.clear();
    _file_comparison = false;
    _open_save_patch = false;
    _open_apply_patch = false;
    _bookmark_filter.clear();
    _tag_filter.clear();
    _changes_filter.clear();
    _graph_filter.clear();
    _built_filter.clear();
    _diff_loading = false;
    _default_layout = true;
    _status_message.clear();
    _error_message.clear();
    if (_window != nullptr)
        SDL_SetWindowTitle(_window, "ggui");
}

bool Application::FileMatchesFilter(const StatusEntry& file) const
{
    const std::string status = DeltaName(file.status);
    return ContainsInsensitive(status, _changes_filter) || ContainsInsensitive(file.path, _changes_filter)
        || ContainsInsensitive(file.old_path, _changes_filter);
}

bool Application::CanNavigateChangedFile(int direction) const
{
    std::vector<const StatusEntry*> files;
    for (const StatusEntry& file : _diff.files)
        if (FileMatchesFilter(file))
            files.push_back(&file);
    if (files.empty())
        return false;
    const auto selected = std::ranges::find_if(files,
        [this](const StatusEntry* file) { return file->path == _selected_file; });
    if (selected == files.end())
        return true;
    return direction < 0 ? selected != files.begin() : std::next(selected) != files.end();
}

void Application::NavigateChangedFile(int direction)
{
    std::vector<const StatusEntry*> files;
    for (const StatusEntry& file : _diff.files)
        if (FileMatchesFilter(file))
            files.push_back(&file);
    if (files.empty())
        return;
    auto selected = std::ranges::find_if(files,
        [this](const StatusEntry* file) { return file->path == _selected_file; });
    if (selected == files.end())
        selected = direction < 0 ? std::prev(files.end()) : files.begin();
    else if (direction < 0 && selected != files.begin())
        --selected;
    else if (direction > 0 && std::next(selected) != files.end())
        ++selected;
    else
        return;
    SelectFile((*selected)->path);
}

std::optional<std::filesystem::path> Application::WorkingCopyPath(
    const std::string& root, const std::string& relative)
{
    if (root.empty() || relative.empty())
        return std::nullopt;
    const std::filesystem::path relative_path(relative);
    if (relative_path.is_absolute())
        return std::nullopt;
    std::error_code error;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error)
        return std::nullopt;
    const std::filesystem::path candidate = std::filesystem::weakly_canonical(canonical_root / relative_path, error);
    if (error)
        return std::nullopt;
    const std::filesystem::path within = candidate.lexically_relative(canonical_root);
    if (within.empty() || within.is_absolute() || *within.begin() == "..")
        return std::nullopt;
    return candidate;
}

// GCOV_EXCL_START: OS-default file handlers are platform integrations

void Application::OpenExternalPath(const std::filesystem::path& path, std::string_view description)
{
    if (!SDL_OpenURL(FileUrl(path.string()).c_str()))
        _error_message = SDL_GetError();
    else
        _status_message = std::string(description) + " opened";
}

void Application::OpenExternalDiff(const std::string& path, const std::string& compare_to)
{
    if (_snapshot == nullptr || _diff.revision.empty() || path.empty())
        return;
    const std::string revision = _diff.revision + "^!";
    std::vector<const char*> arguments{"git", "-C", _snapshot->root.c_str(), "difftool", "--no-prompt",
        compare_to.empty() ? revision.c_str() : _diff.revision.c_str()};
    if (!compare_to.empty())
        arguments.push_back(compare_to.c_str());
    arguments.insert(arguments.end(), {"--", path.c_str(), nullptr});
    SDL_Process* process = SDL_CreateProcess(arguments.data(), false); // GCOV_EXCL_LINE: external application handoff
    if (process == nullptr)
        _error_message = SDL_GetError(); // GCOV_EXCL_LINE: platform process failure
    else
    {
        SDL_DestroyProcess(process); // GCOV_EXCL_LINE: external process owns its lifetime
        _status_message = "External diff opened";
    }
}
// GCOV_EXCL_STOP

// GCOV_EXCL_START: nativefiledialog owns the platform-dependent modal interaction

void Application::PickAndOpen(bool initialize)
{
    if (!_active_operation.empty())
        return;
    const std::string path = PickFolder();
    if (!path.empty())
        _engine.Enqueue(initialize ? Command{InitRepository{path}} : Command{OpenRepository{path}});
}

std::string Application::PickFolder(const std::string& initial)
{
    if (NFD_Init() != NFD_OKAY)
    {
        _error_message = NFD_GetError() == nullptr ? "Could not initialize folder picker" : NFD_GetError();
        return {};
    }
    nfdu8char_t* path = nullptr;
    nfdpickfolderu8args_t arguments{};
    arguments.defaultPath = initial.empty() ? nullptr : initial.c_str();
    const nfdresult_t result = NFD_PickFolderU8_With(&path, &arguments);
    std::string selected;
    if (result == NFD_OKAY && path != nullptr)
        selected = path;
    else if (result == NFD_ERROR)
        _error_message = NFD_GetError() == nullptr ? "Folder picker failed" : NFD_GetError();
    if (path != nullptr)
        NFD_FreePathU8(path);
    NFD_Quit();
    return selected;
}
// GCOV_EXCL_STOP

} // namespace Ggui

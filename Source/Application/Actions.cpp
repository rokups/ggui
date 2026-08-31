// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <nfd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

namespace
{

template <typename Type, void (*Free)(Type*)>
struct GitDeleter
{
    void operator()(Type* value) const { Free(value); }
};

template <typename Type, void (*Free)(Type*)>
using GitPtr = std::unique_ptr<Type, GitDeleter<Type, Free>>;

void CheckMerge(int result, std::string_view action)
{
    if (result >= 0)
        return;
    const git_error* error = git_error_last();
    throw std::runtime_error(std::string(action) + ": "
        + (error == nullptr || error->message == nullptr ? "libgit2 error" : error->message));
}

git_index_entry ConflictIndexEntry(const gg_conflict_term& term, const std::string& path)
{
    git_index_entry entry{};
    entry.mode = term.mode;
    entry.id = term.oid;
    entry.path = path.c_str();
    return entry;
}

SDL_Process* StartBackgroundProcess(const char* const* arguments)
{
    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0)
        return nullptr;
    SDL_Process* process = nullptr;
    if (SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
            const_cast<char**>(arguments))
        && SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true))
        process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    return process;
}

} // namespace

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
    _show_history = true;
    _reveal_revision = oid;
    _graph_filter = oid;
    _history_requested_generation = 0;
    UpdateGraphBuild();
}

void Application::CancelHistorySearch()
{
    _reveal_revision.clear();
    _graph_filter.clear();
    _history_requested_generation = 0;
    UpdateGraphBuild();
}

bool Application::CanCreateChange() const
{
    return _snapshot != nullptr && !_selected_revisions.empty()
        && std::ranges::all_of(_selected_revisions, [this](const std::string& oid) {
               return std::ranges::any_of(
                   _history_revisions, [&](const Revision& revision) { return revision.oid == oid; });
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

void Application::CreateChange(const std::string& parent)
{
    if (!_active_operation.empty())
        return;
    const std::string selected_revision = parent == "@" ? CurrentCommit(*_snapshot)
        : !parent.empty()                              ? parent
        : _selected_revisions.size() == 1              ? _selected_revisions.front()
                                                        : "";
    const auto selected = std::ranges::find(_history_revisions, selected_revision, &Revision::oid);
    // Preserve the working-copy shorthand once a gg workspace exists.
    // Resolving that shorthand to its object ID needlessly sends the core
    // through alias resolution (and touches alias reflogs). Before the first
    // gg change exists, however, the toolbar's conceptual "@" is Git HEAD and
    // must be passed as the selected object ID.
    const std::vector<std::string> create_parents = parent.empty() ? SelectedParentRevisions()
        : parent == "@" && !_snapshot->working_copy.empty()        ? std::vector<std::string>{"@"}
                                                                   : std::vector{selected_revision};
    NewChange create{{}, create_parents, {}, {}, false};
    if (selected != _history_revisions.end() && selected->empty)
    {
        // An empty working-copy change is already the writable change the
        // user is asking for. Creating another one can produce the exact same
        // Git object (timestamps have one-second precision), and then trying
        // to abandon the "old" object abandons the new one as well.
        if (selected->oid == _snapshot->working_copy)
            return;
        QueueCommands({std::move(create), Abandon{{selected->oid}, true, false, {}}}, {selected->oid},
            "Creating a new change will rewrite a locked empty parent.");
        return;
    }
    _engine.Enqueue(std::move(create));
}

bool Application::IsLocked(const std::string& identifier) const
{
    if (_snapshot == nullptr || identifier.empty())
        return false;
    std::string oid = identifier == "@" ? CurrentCommit(*_snapshot) : identifier;
    const auto ref = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& candidate) {
        return candidate.name == oid;
    });
    if (ref != _snapshot->refs.end())
        oid = ref->target;
    const auto revision = std::ranges::find_if(_history_revisions, [&](const Revision& candidate) {
        return candidate.oid == oid || std::ranges::find(candidate.aliases, oid) != candidate.aliases.end();
    });
    return revision != _history_revisions.end() && revision->pushed;
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
        return IsLocked(source == nullptr ? _input_secondary : source->oid);
    }
    case Dialog::Squash:
    {
        std::string destination = _input_secondary;
        if (destination.empty())
        {
            const auto source = std::ranges::find(_history_revisions, _selected_revision, &Revision::oid);
            if (source != _history_revisions.end() && !source->parents.empty())
                destination = source->parents.front();
        }
        return IsLocked(_selected_revision) || IsLocked(destination);
    }
    case Dialog::Restore: return IsLocked(_selected_revision) || IsLocked(_input_primary);
    case Dialog::Reconcile:
    {
        const Revision* source = RebaseBranchRoot(*_snapshot, _input_tertiary, _input_filesets);
        return IsLocked(source == nullptr ? _input_tertiary : source->oid);
    }
    case Dialog::ConfirmDrop:
    {
        const Revision* source = _pending_drop.entire_branch
            ? RebaseBranchRoot(*_snapshot, _pending_drop.source, _pending_drop.target) : nullptr;
        return IsLocked(source == nullptr ? _pending_drop.source : source->oid) || IsLocked(_pending_drop.target);
    }
    case Dialog::ConfirmLocked: return true;
    default: return false;
    }
}

const Revision* Application::RebaseSource() const
{
    const auto source = std::ranges::find_if(_history_revisions, [this](const Revision& revision) {
        return revision.oid == _input_secondary
            || std::ranges::find(revision.aliases, _input_secondary) != revision.aliases.end();
    });
    return source == _history_revisions.end() ? nullptr : &*source;
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
    git_repository* raw = nullptr;
    if (_snapshot == nullptr || git_repository_open_ext(
            &raw, _snapshot->root.c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) != GIT_OK)
        return {};
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repository(raw, git_repository_free);
    git_oid selected_oid{};
    if (git_oid_fromstr(&selected_oid, selected.c_str(), git_repository_oid_type(repository.get())) != GIT_OK)
        return {};
    git_revwalk* raw_walk = nullptr;
    if (git_revwalk_new(&raw_walk, repository.get()) != GIT_OK) return {};
    std::unique_ptr<git_revwalk, decltype(&git_revwalk_free)> walk(raw_walk, git_revwalk_free);
    if (git_revwalk_push_glob(walk.get(), "refs/*") != GIT_OK) return {};
    git_oid candidate{};
    while (git_revwalk_next(&candidate, walk.get()) == GIT_OK)
    {
        if (git_oid_equal(&candidate, &selected_oid) != 0) continue;
        const int descendant = git_graph_descendant_of(repository.get(), &candidate, &selected_oid);
        if (descendant < 0) return {};
        if (descendant != 0) result.emplace_back(git_oid_tostr_s(&candidate));
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

void Application::RequestAbandon(const std::string& revision, bool include_descendants)
{
    if (!_active_operation.empty() || revision.empty())
        return;
    if (_selected_revision != revision)
        SelectRevision(revision);
    const auto selected = std::ranges::find(_history_revisions, revision, &Revision::oid);
    const bool has_refs = std::ranges::any_of(
        _snapshot->refs, [&](const NamedRef& ref) { return ref.target == revision; });
    if (!include_descendants && selected != _history_revisions.end() && selected->empty && !has_refs
        && !selected->pushed)
        _engine.Enqueue(Abandon{{revision}, false, false, {}});
    else
    {
        OpenDialog(Dialog::Abandon);
        _input_flag_tertiary = include_descendants;
        const std::vector<std::string> revisions = AbandonRevisions(revision, include_descendants);
        _input_flag = std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK
                && std::ranges::find(revisions, ref.target) != revisions.end();
        });
    }
}

bool Application::CanSubmitDialog() const
{
    switch (_dialog)
    {
    case Dialog::Clone: return HasText(_input_primary) && HasText(_input_secondary);
    case Dialog::Rebase:
        return HasText(_input_primary) && _snapshot != nullptr
            && _snapshot->generation == _dialog_snapshot_generation
            && CurrentCommit(*_snapshot) == _input_secondary;
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
    case Dialog::Reconcile:
        return _snapshot != nullptr && _snapshot->generation == _dialog_snapshot_generation
            && std::ranges::any_of(_snapshot->refs, [this](const NamedRef& ref) {
                   return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == _input_primary
                       && ref.target == _input_tertiary;
               })
            && std::ranges::any_of(_snapshot->refs, [this](const NamedRef& ref) {
                   return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.tracked
                       && ref.name == _input_primary && ref.remote == _input_secondary
                       && ref.target == _input_filesets;
               })
            && ClassifyBookmarkRelation(*_snapshot, _input_tertiary, _input_filesets)
                == BookmarkRelation::Diverged;
    case Dialog::ConfirmBookmarkMove:
        return _snapshot != nullptr && _snapshot->generation == _dialog_snapshot_generation
            && std::ranges::any_of(_snapshot->refs, [this](const NamedRef& ref) {
                   return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == _input_primary
                       && ref.target == _input_secondary;
               });
    case Dialog::ConfirmDrop:
        return _snapshot != nullptr && _snapshot->generation == _dialog_snapshot_generation
            && ResolveSnapshotRevision(*_snapshot, _pending_drop.source) != nullptr
            && ResolveSnapshotRevision(*_snapshot, _pending_drop.target) != nullptr;
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

std::string_view Application::DropTooltip(DropAction action, bool entire_branch, bool copy)
{
    return action == DropAction::ReorderBefore ? copy ? "Copy as child of" : "Move as child of"
        : action == DropAction::ReorderAfter    ? copy ? "Copy as parent of" : "Move as parent of"
        : action == DropAction::Squash
        ? entire_branch ? "Squash entire branch into" : "Squash change into"
        : entire_branch ? "Rebase entire branch onto" : "Rebase change onto";
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
    const auto source = std::ranges::find(_history_revisions, oid, &Revision::oid);
    if (source == _history_revisions.end())
        return {parent, child};
    if (source->parents.size() == 1
        && std::ranges::find(_history_revisions, source->parents.front(), &Revision::oid)
            != _history_revisions.end())
        parent = source->parents.front();
    int children = 0;
    for (const Revision& revision : _history_revisions)
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
    ClearConflictMerge();
    _snapshot.reset();
    _history_view.reset();
    _history_revisions.clear();
    _diff = {};
    _visible_revisions.clear();
    _graph_rows.clear();
    _history_hovered_track = -1;
    _history_hovered_commit_row = -1;
    _graph_generation = 0;
    _history_requested_generation = 0;
    _history_applied_request = 0;
    _history_search_scrolled_request = 0;
    _reveal_revision.clear();
    _history_scroll_target = -1.0f;
    _history_scroll_frames = 0;
    _history_anchor.clear();
    _history_anchor_offset = 0.0f;
    _history_expansion_pending.clear();
    _history_expansion_feedback_until = {};
    _revision_prefixes.clear();
    _operation_prefixes.clear();
    _history_refs_by_revision.clear();
    _selected_revision.clear();
    _selected_revisions.clear();
    _visible_bookmarks.clear();
    _visible_bookmarks_user_selected = false;
    _selected_tags.clear();
    _selected_remotes.clear();
    _selected_remotes_user_selected = false;
    _selected_file.clear();
    _preferred_file.clear();
    _pending_revision.clear();
    _pending_editor_revision.clear();
    _pending_editor_path.clear();
    _compare_to.clear();
    _file_comparison = false;
    _open_save_patch = false;
    _open_apply_patch = false;
    _bookmark_filter.clear();
    _tag_filter.clear();
    _changes_filter.clear();
    _graph_filter.clear();
    _built_filter.clear();
    _built_bookmarks.clear();
    _diff_loading = false;
    _default_layout = true;
    _status_message.clear();
    _error_message.clear();
    if (_window != nullptr)
        SDL_SetWindowTitle(_window, "ggui");
}

bool Application::FileMatchesFilter(const StatusEntry& file) const
{
    const std::string status = DeltaName(file.conflicted ? GIT_DELTA_CONFLICTED : file.status);
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

void Application::OpenFileInEditor(const std::string& path)
{
    if (_snapshot == nullptr || _diff.revision.empty() || path.empty())
        return;
    const auto current = std::ranges::find(_history_revisions, _snapshot->working_copy, &Revision::oid);
    const bool direct_parent = current != _history_revisions.end() && current->parents.size() == 1
        && current->parents.front() == _diff.revision;
    const bool changed_in_working_copy = std::ranges::any_of(_snapshot->status, [&](const StatusEntry& file) {
        return file.status != GIT_DELTA_UNMODIFIED && file.status != GIT_DELTA_IGNORED
            && (file.path == path || file.old_path == path);
    });
    if (_diff.revision != _snapshot->working_copy && (!direct_parent || changed_in_working_copy))
    {
        _pending_editor_revision = _diff.revision;
        _pending_editor_path = path;
        _engine.Enqueue(LoadFileContent{_diff.revision, path});
        return;
    }
    _pending_editor_revision.clear();
    _pending_editor_path.clear();
    const std::optional<std::filesystem::path> absolute = WorkingCopyPath(_snapshot->root, path);
    std::error_code error;
    if (!absolute.has_value() || !std::filesystem::is_regular_file(*absolute, error) || error)
    {
        _error_message = "File is unavailable in the working copy";
        return;
    }
    OpenEditorPath(*absolute);
}

void Application::OpenTemporaryFileInEditor(const FileContentReady& file)
{
    if (file.revision != _pending_editor_revision || file.path != _pending_editor_path)
        return;
    _pending_editor_revision.clear();
    _pending_editor_path.clear();
    try
    {
        if (_editor_temp_directory.empty())
        {
            _editor_temp_directory = std::filesystem::temp_directory_path()
                / ("ggui-editor-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(_editor_temp_directory);
        }
        const std::string relative = (std::filesystem::path(ShortId(file.revision)) / file.path).generic_string();
        const std::optional<std::filesystem::path> temporary =
            WorkingCopyPath(_editor_temp_directory.string(), relative);
        if (!temporary.has_value())
            throw std::runtime_error("temporary editor path is invalid");
        std::filesystem::create_directories(temporary->parent_path());
        std::ofstream output(*temporary, std::ios::binary | std::ios::trunc);
        output.write(file.contents.data(), static_cast<std::streamsize>(file.contents.size()));
        output.close();
        if (!output)
            throw std::runtime_error("could not write temporary editor file");
        OpenEditorPath(*temporary);
    }
    catch (const std::exception& error)
    {
        _error_message = error.what();
    }
}

void Application::OpenConflictInMergeTool(const std::string& path)
{
    if (_snapshot == nullptr || _selected_revision.empty() || path.empty() || !_active_operation.empty())
        return;
    if (_merge_process != nullptr || _open_merge_confirmation)
    {
        _error_message = "Finish the current conflict merge first";
        return;
    }
#ifdef IMGUI_BUILD_TESTING
    if (_test_mode)
    {
        _merge_revision = _selected_revision;
        _merge_conflict_path = path;
        _merge_exit_code = 0;
        _open_merge_confirmation = true;
        return;
    }
#endif
    // GCOV_EXCL_START: native merge-tool staging and process handoff
    try
    {
        git_repository* raw_repository = nullptr;
        CheckMerge(git_repository_open(&raw_repository, _snapshot->root.c_str()), "open merge repository");
        GitPtr<git_repository, git_repository_free> repository(raw_repository);
        gg_repository* raw_gg = nullptr;
        CheckMerge(gg_repository_attach(&raw_gg, repository.get()), "attach merge repository");
        GitPtr<gg_repository, gg_repository_free> gg(raw_gg);
        git_oid revision_oid{};
        CheckMerge(gg_repository_resolve(&revision_oid, gg.get(), _selected_revision.c_str()),
            "load conflicted revision");
        gg_conflict_array conflicts{};
        CheckMerge(gg_repository_conflicts(&conflicts, gg.get(), &revision_oid), "load conflict sides");
        const auto dispose_conflicts = [&conflicts](void*) { gg_conflict_array_dispose(&conflicts); };
        std::unique_ptr<void, decltype(dispose_conflicts)> conflict_guard(reinterpret_cast<void*>(1), dispose_conflicts);
        const gg_conflict* conflict = nullptr;
        for (size_t index = 0; index < conflicts.count; ++index)
            if (conflicts.items[index].path != nullptr && path == conflicts.items[index].path)
            {
                conflict = &conflicts.items[index];
                break;
            }
        if (conflict == nullptr)
            throw std::runtime_error("The selected file is no longer conflicted");
        if (conflict->remove_count != 1 || conflict->add_count != 2)
            throw std::runtime_error("The configured merge tool requires one base and two conflict sides");

        _merge_temp_directory = std::filesystem::temp_directory_path()
            / ("ggui-merge-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path worktree = _merge_temp_directory / "worktree";
        std::filesystem::create_directories(worktree);
        const std::optional<std::filesystem::path> merge_result = WorkingCopyPath(worktree.string(), path);
        if (!merge_result.has_value())
            throw std::runtime_error("Conflict path is outside the temporary merge worktree");
        _merge_result_path = *merge_result;
        std::filesystem::create_directories(_merge_result_path.parent_path());
        git_commit* raw_revision = nullptr;
        CheckMerge(git_commit_lookup(&raw_revision, repository.get(), &revision_oid), "load conflicted revision");
        GitPtr<git_commit, git_commit_free> revision(raw_revision);
        git_tree* raw_tree = nullptr;
        CheckMerge(git_commit_tree(&raw_tree, revision.get()), "load conflicted revision tree");
        GitPtr<git_tree, git_tree_free> tree(raw_tree);
        git_tree_entry* raw_entry = nullptr;
        const int entry_result = git_tree_entry_bypath(&raw_entry, tree.get(), path.c_str());
        if (entry_result == GIT_ENOTFOUND)
        {
            std::ofstream output(_merge_result_path, std::ios::binary);
            if (!output)
                throw std::runtime_error("Could not prepare the merge result file");
        }
        else
        {
            CheckMerge(entry_result, "load conflicted file");
            GitPtr<git_tree_entry, git_tree_entry_free> entry(raw_entry);
            if (git_tree_entry_type(entry.get()) != GIT_OBJECT_BLOB)
                throw std::runtime_error("The conflicted path is not a file");
            git_blob* raw_blob = nullptr;
            CheckMerge(git_blob_lookup(&raw_blob, repository.get(), git_tree_entry_id(entry.get())),
                "load conflicted file contents");
            GitPtr<git_blob, git_blob_free> blob(raw_blob);
            std::ofstream output(_merge_result_path, std::ios::binary | std::ios::trunc);
            if (git_blob_rawsize(blob.get()) != 0)
                output.write(static_cast<const char*>(git_blob_rawcontent(blob.get())),
                    static_cast<std::streamsize>(git_blob_rawsize(blob.get())));
            if (!output)
                throw std::runtime_error("Could not prepare the merge result file");
        }

        const std::filesystem::path index_path = _merge_temp_directory / "index";
        git_index* raw_index = nullptr;
#ifdef GIT_EXPERIMENTAL_SHA256
        git_index_options index_options = GIT_INDEX_OPTIONS_INIT;
        index_options.oid_type = git_repository_oid_type(repository.get());
        CheckMerge(git_index_open(&raw_index, index_path.string().c_str(), &index_options),
            "create temporary merge index");
#else
        CheckMerge(git_index_open(&raw_index, index_path.string().c_str()), "create temporary merge index");
#endif
        GitPtr<git_index, git_index_free> index(raw_index);
        const git_index_entry base = ConflictIndexEntry(conflict->removes[0], path);
        const git_index_entry local = ConflictIndexEntry(conflict->adds[0], path);
        const git_index_entry remote = ConflictIndexEntry(conflict->adds[1], path);
        CheckMerge(git_index_conflict_add(index.get(), conflict->removes[0].present ? &base : nullptr,
            conflict->adds[0].present ? &local : nullptr, conflict->adds[1].present ? &remote : nullptr),
            "stage conflict sides");
        CheckMerge(git_index_write(index.get()), "write temporary merge index");

        const char* arguments[]{"git", "-C", _snapshot->root.c_str(), "mergetool", "--no-prompt", "--",
            path.c_str(), nullptr};
        SDL_Environment* environment = SDL_CreateEnvironment(true);
        if (environment == nullptr
            || !SDL_SetEnvironmentVariable(environment, "GIT_INDEX_FILE", index_path.string().c_str(), true)
            || !SDL_SetEnvironmentVariable(environment, "GIT_WORK_TREE", worktree.string().c_str(), true))
        {
            if (environment != nullptr)
                SDL_DestroyEnvironment(environment);
            throw std::runtime_error(SDL_GetError());
        }
        const SDL_PropertiesID properties = SDL_CreateProperties();
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(arguments));
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
#ifdef _WIN32
        // A GUI application has no console to inherit. Prevent git.exe from
        // allocating a transient console while it waits for the merge tool.
        SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
        _merge_process = SDL_CreateProcessWithProperties(properties);
        SDL_DestroyProperties(properties);
        SDL_DestroyEnvironment(environment);
        if (_merge_process == nullptr)
            throw std::runtime_error(SDL_GetError());
        _merge_revision = _selected_revision;
        _merge_conflict_path = path;
        _status_message = "Three-way merge tool opened";
    }
    catch (const std::exception& error)
    {
        _error_message = error.what();
        ClearConflictMerge();
    }
    // GCOV_EXCL_STOP
}

void Application::PollMergeTool()
{
    if (_merge_process == nullptr || !SDL_WaitProcess(_merge_process, false, &_merge_exit_code))
        return;
    // GCOV_EXCL_START: external merge-tool process completion
    SDL_DestroyProcess(_merge_process);
    _merge_process = nullptr;
    _open_merge_confirmation = true;
    // GCOV_EXCL_STOP
}

void Application::MarkConflictResolved(const std::string& path)
{
    if (_snapshot == nullptr || _selected_revision != _snapshot->working_copy || path.empty())
        return;
    const std::optional<std::filesystem::path> working_path = WorkingCopyPath(_snapshot->root, path);
    std::error_code error;
    if (!working_path.has_value()
        || (std::filesystem::exists(*working_path, error) && !std::filesystem::is_regular_file(*working_path, error))
        || error)
    {
        _error_message = "Resolved file is unavailable in the working copy";
        return;
    }
    _engine.Enqueue(Refresh{});
    _status_message = "Conflict resolution queued";
}

void Application::FinishConflictMerge(bool resolved)
{
    if (resolved && _snapshot != nullptr)
    {
        // GCOV_EXCL_START: confirmed external merge result import
        try
        {
            ResolveConflict resolution{_merge_revision, _merge_conflict_path, {}, false};
            if (std::filesystem::is_regular_file(_merge_result_path))
            {
                std::ifstream input(_merge_result_path, std::ios::binary);
                resolution.contents.assign(
                    std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
                if (!input.eof())
                    throw std::runtime_error("Could not read the merge result file");
                resolution.present = true;
            }
            else if (std::filesystem::exists(_merge_result_path))
            {
                throw std::runtime_error("The merge result is not a file");
            }
            _engine.Enqueue(std::move(resolution));
            _status_message = "Conflict resolution queued";
        }
        catch (const std::exception& error)
        {
            _error_message = error.what();
            return;
        }
        // GCOV_EXCL_STOP
    }
    ClearConflictMerge();
}

void Application::ClearConflictMerge()
{
    if (_merge_process != nullptr)
    {
        // GCOV_EXCL_START: external merge-tool process shutdown
        SDL_KillProcess(_merge_process, true);
        SDL_WaitProcess(_merge_process, true, nullptr);
        SDL_DestroyProcess(_merge_process);
        _merge_process = nullptr;
        // GCOV_EXCL_STOP
    }
    if (!_merge_temp_directory.empty())
    {
        std::error_code error;
        std::filesystem::remove_all(_merge_temp_directory, error);
    }
    _merge_temp_directory.clear();
    _merge_result_path.clear();
    _merge_revision.clear();
    _merge_conflict_path.clear();
    _open_merge_confirmation = false;
    _merge_exit_code = 0;
}

// GCOV_EXCL_START: OS-default file handlers are platform integrations

void Application::OpenEditorPath(const std::filesystem::path& path)
{
#ifdef IMGUI_BUILD_TESTING
    if (_test_mode)
    {
        _opened_editor_path_for_test = path;
        _status_message = "File opened in editor";
        return;
    }
#endif
    std::string editor;
    try
    {
        editor = EffectiveEditor(ReadEditorValues(_snapshot == nullptr
            ? std::nullopt : std::optional<std::filesystem::path>(_snapshot->root)));
    }
    catch (const std::exception& error)
    {
        _error_message = error.what();
        return;
    }
    if (editor.empty())
    {
        _error_message = "Configure a GUI editor in Settings";
        return;
    }
    const std::string path_text = path.string();
#ifdef _WIN32
    const std::string command = editor + " \"" + path_text + "\"";
    const char* arguments[]{"cmd.exe", "/D", "/S", "/C", command.c_str(), nullptr};
#else
    const std::string command = editor + " \"$1\"";
    const char* arguments[]{"/bin/sh", "-c", command.c_str(), "ggui-editor", path_text.c_str(), nullptr};
#endif
    SDL_Process* process = StartBackgroundProcess(arguments); // GCOV_EXCL_LINE: external application handoff
    if (process == nullptr)
        _error_message = SDL_GetError(); // GCOV_EXCL_LINE: platform process failure
    else
    {
        SDL_DestroyProcess(process); // GCOV_EXCL_LINE: external process owns its lifetime
        _status_message = "File opened in editor";
    }
}

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
    const bool conflicted = std::ranges::any_of(_diff.files, [&](const StatusEntry& file) {
        return file.conflicted && (file.path == path || file.old_path == path);
    });
    if (conflicted)
    {
        OpenConflictInMergeTool(path);
        return;
    }
    const std::string revision = _diff.revision
        + (compare_to.empty() && _diff.revision == _snapshot->working_copy ? "^" : "^!");
    std::vector<const char*> arguments{"git", "-C", _snapshot->root.c_str(), "difftool", "--no-prompt",
        compare_to.empty() ? revision.c_str() : _diff.revision.c_str()};
    if (!compare_to.empty())
        arguments.push_back(compare_to.c_str());
    arguments.insert(arguments.end(), {"--", path.c_str(), nullptr});
    SDL_Process* process = StartBackgroundProcess(arguments.data()); // GCOV_EXCL_LINE: external application handoff
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

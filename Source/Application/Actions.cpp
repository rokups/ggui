// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <nfd.h>

#include <git2/merge.h>
#include <git2/sys/errors.h>

#include <array>
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
#include <unordered_set>
#include <utility>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

bool Application::EnqueueAction(Command command)
{
    if (!_active_operation.empty())
        return false;
    const std::string name = CommandName(command);
    // Lock actions immediately, but leave repository-opening state changes to
    // the engine event at the start of the next frame. Repository switches can
    // be initiated from the toolbar itself; clearing the snapshot here would
    // invalidate the rest of the frame while it is still rendering it.
    _active_operation = name;
    _error_message.clear();
    _status_message.clear();
    _progress_phase.clear();
    _progress_completed = 0;
    _progress_total = 0;
    if (_engine.Enqueue(std::move(command)))
        return true;
    // Synthetic UI snapshots suppress engine commands. Do not leave their
    // controls permanently locked when a test intentionally exercises them.
    ApplyEvent(OperationFinished{name});
    return false;
}

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

bool IsGgConflictFile(std::string_view contents)
{
    bool opening = false;
    bool side = false;
    bool base = false;
    bool closing = false;
    while (!contents.empty())
    {
        const std::size_t line_end = contents.find('\n');
        std::string_view line = contents.substr(0, line_end);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        const char marker = line.empty() ? '\0' : line.front();
        const std::size_t marker_end = line.find_first_not_of(marker);
        const std::size_t marker_length = marker_end == std::string_view::npos ? line.size() : marker_end;
        const std::string_view label = marker_length >= 7 ? line.substr(marker_length) : std::string_view{};
        opening |= marker == '<' && marker_length >= 7 && label == " Conflict";
        side |= marker == '+' && marker_length >= 7 && label.starts_with(" Side #");
        base |= marker == '-' && marker_length >= 7 && label.starts_with(" Base #");
        closing |= marker == '>' && marker_length >= 7 && label == " Conflict ends";
        if (line_end == std::string_view::npos)
            break;
        contents.remove_prefix(line_end + 1);
    }
    return opening && (side || base) && closing;
}

std::optional<std::string> TreeBlobContents(git_repository* repository, git_tree* tree, const char* path)
{
    git_tree_entry* raw_entry = nullptr;
    const int entry_result = git_tree_entry_bypath(&raw_entry, tree, path);
    if (entry_result == GIT_ENOTFOUND)
    {
        git_error_clear();
        return std::nullopt;
    }
    CheckMerge(entry_result, "load conflicted file");
    GitPtr<git_tree_entry, git_tree_entry_free> entry(raw_entry);
    if (git_tree_entry_type(entry.get()) != GIT_OBJECT_BLOB)
        throw std::runtime_error("The conflicted path is not a file");
    git_blob* raw_blob = nullptr;
    CheckMerge(git_blob_lookup(&raw_blob, repository, git_tree_entry_id(entry.get())),
        "load conflicted file contents");
    GitPtr<git_blob, git_blob_free> blob(raw_blob);
    return std::string(static_cast<const char*>(git_blob_rawcontent(blob.get())),
        static_cast<std::size_t>(git_blob_rawsize(blob.get())));
}

std::string StandardMergeContents(
    git_repository* repository, const gg_conflict& conflict, const std::string& path)
{
    struct MergeInput
    {
        GitPtr<git_blob, git_blob_free> blob{nullptr};
        git_merge_file_input input = GIT_MERGE_FILE_INPUT_INIT;
    };
    std::array<MergeInput, 3> inputs;
    const gg_conflict_term* terms[]{
        conflict.remove_count == 0 ? nullptr : conflict.removes,
        conflict.add_count == 0 ? nullptr : conflict.adds,
        conflict.add_count < 2 ? nullptr : conflict.adds + 1};
    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        const gg_conflict_term* term = terms[index];
        if (term == nullptr || !term->present)
            continue;
        git_blob* raw_blob = nullptr;
        CheckMerge(git_blob_lookup(&raw_blob, repository, &term->oid), "load merge input");
        inputs[index].blob.reset(raw_blob);
        inputs[index].input.ptr = static_cast<const char*>(git_blob_rawcontent(inputs[index].blob.get()));
        inputs[index].input.size = static_cast<std::size_t>(git_blob_rawsize(inputs[index].blob.get()));
        inputs[index].input.path = path.c_str();
        inputs[index].input.mode = term->mode;
    }
    git_merge_file_options options = GIT_MERGE_FILE_OPTIONS_INIT;
    options.flags = GIT_MERGE_FILE_STYLE_MERGE;
    options.ancestor_label = "BASE";
    options.our_label = "LOCAL";
    options.their_label = "REMOTE";
    git_merge_file_result result{};
    CheckMerge(git_merge_file(&result, &inputs[0].input, &inputs[1].input, &inputs[2].input, &options),
        "create standard merge file");
    struct ResultGuard
    {
        git_merge_file_result& value;
        ~ResultGuard() { git_merge_file_result_free(&value); }
    } result_guard{result};
    if (result.path == nullptr)
        throw std::runtime_error("The conflict sides have incompatible paths");
    return std::string(result.ptr == nullptr ? "" : result.ptr, result.len);
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

std::vector<std::string> FindAbandonRevisions(
    const std::string& root, const std::string& selected)
{
    git_repository* raw = nullptr;
    if (git_repository_open_ext(&raw, root.c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) != GIT_OK)
        return {};
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repository(raw, git_repository_free);
    gg_repository* raw_gg = nullptr;
    if (gg_repository_attach(&raw_gg, repository.get()) != GIT_OK)
        return {};
    std::unique_ptr<gg_repository, decltype(&gg_repository_free)> gg(raw_gg, gg_repository_free);
    // Match the core's live rewrite graph. Walking refs/* also traverses
    // operation keepalive commits and superseded user commits retained for undo.
    const std::string expression = "descendants(" + selected + ")";
    gg_oid_array descendants{};
    if (gg_repository_resolve_set(&descendants, gg.get(), expression.c_str()) != GIT_OK)
    {
        gg_oid_array_dispose(&descendants);
        return {};
    }
    std::vector<std::string> result;
    result.reserve(descendants.count);
    for (std::size_t index = 0; index < descendants.count; ++index)
        result.emplace_back(git_oid_tostr_s(&descendants.ids[index]));
    gg_oid_array_dispose(&descendants);
    return result;
}

std::string SquashDescription(
    const std::vector<Revision>& revisions, const std::string& selected_id, bool descendants)
{
    const auto selected = std::ranges::find(revisions, selected_id, &Revision::oid);
    if (selected == revisions.end()) return {};

    std::vector<const Revision*> combined;
    if (selected->parents.size() == 1)
    {
        const auto parent = std::ranges::find(revisions, selected->parents.front(), &Revision::oid);
        if (parent != revisions.end()) combined.push_back(&*parent);
    }

    std::unordered_set<std::string> included{selected->oid};
    if (descendants)
    {
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (const Revision& revision : revisions)
            {
                if (included.contains(revision.oid)
                    || std::ranges::none_of(revision.parents,
                        [&](const std::string& parent) { return included.contains(parent); }))
                    continue;
                included.insert(revision.oid);
                changed = true;
            }
        }
    }
    for (const Revision& revision : revisions | std::views::reverse)
        if (included.contains(revision.oid)) combined.push_back(&revision);

    std::string result;
    for (const Revision* revision : combined)
    {
        if (revision->description.empty()) continue;
        if (!result.empty()) result += "\n\n";
        result += revision->description;
    }
    return result;
}

} // namespace

void Application::SelectRevision(const std::string& oid, bool additive)
{
    if (IsWorkingTreeRevision(oid)) additive = false;
    else if (additive && IsWorkingTreeRevision(_selected_revision)) _selected_revisions.clear();
    if (_snapshot != nullptr && (oid == CurrentCommit(*_snapshot) || IsWorkingTreeRevision(oid)))
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
    _working_tree_diff_outdated = false;
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

void Application::RequestBlame(const std::string& revision, const std::string& path)
{
    if (_snapshot == nullptr || revision.empty() || path.empty())
        return;
    if (!_blame_history.empty() && SameDiffRevision(_blame_revision, revision) && _blame_path == path)
    {
        _show_blame = true;
        ReloadBlame();
        return;
    }
    if (!_blame_history.empty())
    {
        _blame_history[_blame_history_index].scroll_y = _blame_scroll_y;
        _blame_history.resize(_blame_history_index + 1);
    }
    _blame_history.push_back({revision, path});
    _blame_history_index = _blame_history.size() - 1;
    _blame_restore_scroll = 0.0f;
    LoadBlameView(revision, path, false);
}

void Application::ReloadBlame()
{
    if (_blame_revision.empty() || _blame_path.empty())
        return;
    _blame_restore_scroll = -1.0f;
    LoadBlameView(_blame_revision, _blame_path, true);
}

bool Application::CanNavigateBlame(int direction) const
{
    return direction < 0 ? _blame_history_index > 0
        : _blame_history_index + 1 < _blame_history.size();
}

void Application::NavigateBlame(int direction)
{
    if (direction == 0 || !CanNavigateBlame(direction))
        return;
    _blame_history[_blame_history_index].scroll_y = _blame_scroll_y;
    _blame_history_index = direction < 0 ? _blame_history_index - 1 : _blame_history_index + 1;
    const BlameLocation& location = _blame_history[_blame_history_index];
    _blame_restore_scroll = location.scroll_y;
    LoadBlameView(location.revision, location.path, false);
}

void Application::LoadBlameView(const std::string& revision, const std::string& path, bool keep_content)
{
    if (_snapshot == nullptr)
        return;
    _blame_revision = revision;
    _blame_path = path;
    if (!keep_content)
        _blame = {};
    _blame_loading = true;
    _show_blame = true;
    if (!_engine.Enqueue(LoadBlame{revision, path}))
        _blame_loading = false;
}

void Application::ClearBlame()
{
    _blame = {};
    _blame_revision.clear();
    _blame_path.clear();
    _blame_filter.clear();
    _blame_loading = false;
    _blame_history.clear();
    _blame_history_index = 0;
    _blame_scroll_y = 0.0f;
    _blame_restore_scroll = -1.0f;
}

void Application::ToggleComparison(bool file_comparison)
{
    if (!_compare_to.empty() && _file_comparison == file_comparison)
    {
        _compare_to.clear();
        _file_comparison = false;
    }
    else if (_snapshot != nullptr && !_selected_revision.empty() && !IsWorkingTreeRevision(_selected_revision)
        && !CurrentCommit(*_snapshot).empty()
        && _selected_revision != CurrentCommit(*_snapshot))
    {
        _compare_to = CurrentCommit(*_snapshot);
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

void Application::MoveBranch(const NamedRef& branch, const std::string& revision)
{
    if (branch.kind != GG_NAMED_REF_LOCAL_BRANCH || branch.target == revision)
        return;
    const BranchRelation relation = ClassifyBranchRelation(*_snapshot, branch.target, revision);
    if (relation == BranchRelation::LocalAhead || relation == BranchRelation::Diverged)
    {
        OpenDialog(Dialog::ConfirmBranchMove);
        _input_primary = branch.name;
        _input_secondary = branch.target;
        _input_tertiary = revision;
        _dialog_snapshot_generation = _snapshot->generation;
    }
    else
        _engine.Enqueue(Branch{GG_BRANCH_MOVE, {branch.name}, revision, {}});
}

void Application::RequestBranchDelete(const std::string& name, bool local, std::vector<std::string> remotes)
{
    OpenDialog(Dialog::ConfirmBranchDelete);
    if (_dialog == Dialog::ConfirmBranchDelete)
        _pending_branch_delete = {name, local, std::move(remotes)};
}

void Application::RequestWorkingFiles(std::vector<StatusEntry> files, bool remove)
{
    OpenDialog(Dialog::ConfirmWorkingFiles);
    if (_dialog != Dialog::ConfirmWorkingFiles)
        return;
    _pending_working_revision = _diff.revision;
    _pending_working_files = std::move(files);
    _pending_working_delete = remove;
}

void Application::CreateChange(const std::string& parent, bool detach)
{
    if (!_active_operation.empty() || _snapshot == nullptr || IsWorkingTreeRevision(parent)
        || (parent.empty() && !CanCreateChange()))
        return;
    const std::string selected_revision = parent == "@" ? CurrentCommit(*_snapshot)
        : !parent.empty()                              ? parent
        : _selected_revisions.size() == 1              ? _selected_revisions.front()
                                                        : "";
    const Revision* selected = ResolveSnapshotRevision(*_snapshot, selected_revision, _history_revisions);
    const std::string selected_oid = selected == nullptr ? selected_revision : selected->oid;
    // Preserve the working-copy shorthand once a gg workspace exists.
    // Resolving that shorthand to its object ID needlessly sends the core
    // through alias resolution (and touches alias reflogs). Before the first
    // gg change exists, however, the toolbar's conceptual "@" is Git HEAD and
    // must be passed as the selected object ID.
    const bool on_working_copy = !_snapshot->working_copy.empty()
        && (parent == "@" || selected_oid == _snapshot->working_copy);
    // Git-like New: a change on @ continues the checked-out branch, and a
    // change on the tip of one free local branch checks that branch out and
    // continues it. Anything else, or Alt+N, forks a detached unnamed head.
    std::string continued_branch;
    bool named_parent = false;
    for (const NamedRef& ref : _snapshot->refs)
    {
        if (ref.kind != GG_NAMED_REF_LOCAL_BRANCH || ref.target != selected_oid)
            continue;
        named_parent = true;
        if (!ref.workspace.empty())
            continue;
        continued_branch = continued_branch.empty() ? ref.name : std::string{};
        if (ref.current)
        {
            continued_branch = ref.name;
            break;
        }
    }
    std::vector<std::string> create_parents;
    if (parent.empty() && _selected_revisions.size() != 1)
        create_parents = SelectedParentRevisions();
    else if (on_working_copy && (detach || _snapshot->head_branch.empty() || continued_branch.empty()
                                    || continued_branch == _snapshot->head_branch))
        create_parents = {"@"};
    else if (!detach && !continued_branch.empty())
        create_parents = {continued_branch};
    else
        create_parents = {selected_oid};
    NewChange create{{}, std::move(create_parents), {}, {}, false, detach};

    std::vector<Command> commands;
    commands.emplace_back(std::move(create));

    // A fresh, undescribed empty change does not add a useful boundary. Once
    // its child exists, splice that placeholder out so repeated New actions
    // continue the same change instead of building an empty stack. Pushed
    // history (including pushed descendants) and other workspaces must stay
    // intact because abandoning it would rewrite protected history, and a
    // detached New must leave the branches on its parent where they are.
    const bool another_workspace = std::ranges::any_of(_snapshot->workspaces,
        [&](const Workspace& workspace) {
            return !workspace.current && workspace.working_copy == selected_oid;
        });
    if (selected != nullptr && !selected->parents.empty() && selected->empty
        && selected->description.empty() && !selected->pushed && !another_workspace
        && !(detach && named_parent) && !RewritesLockedCommit(selected_oid))
        commands.emplace_back(Abandon{{selected_oid}, true, false, {}});
    QueueCommands(std::move(commands), {}, {});
}

std::string Application::CheckoutTarget(const std::string& revision) const
{
    if (_snapshot == nullptr)
        return revision;
    const Revision* resolved = ResolveSnapshotRevision(*_snapshot, revision, _history_revisions);
    const std::string& oid = resolved == nullptr ? revision : resolved->oid;
    const NamedRef* branch = nullptr;
    for (const NamedRef& ref : _snapshot->refs)
    {
        if (ref.kind != GG_NAMED_REF_LOCAL_BRANCH || ref.target != oid || !ref.workspace.empty())
            continue;
        if (branch != nullptr)
            return revision;
        branch = &ref;
    }
    return branch == nullptr ? revision : branch->name;
}

bool Application::IsLocked(const std::string& identifier) const
{
    if (_snapshot == nullptr)
        return false;
    const Revision* revision = ResolveSnapshotRevision(*_snapshot, identifier, _history_revisions);
    return revision != nullptr && revision->pushed;
}

bool Application::RewritesLockedCommit(const std::string& identifier) const
{
    if (_snapshot == nullptr)
        return false;
    const Revision* source = ResolveSnapshotRevision(*_snapshot, identifier, _history_revisions);
    if (source == nullptr)
        return false;
    std::unordered_set<std::string> affected{source->oid};
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const Revision& revision : _history_revisions)
        {
            if (affected.contains(revision.oid))
            {
                if (revision.pushed) return true;
                continue;
            }
            if (std::ranges::any_of(revision.parents,
                    [&](const std::string& parent) { return affected.contains(parent); }))
            {
                if (revision.pushed) return true;
                affected.insert(revision.oid);
                changed = true;
            }
        }
    }
    return false;
}

std::vector<std::string> Application::DropRewriteRoots() const
{
    if (_snapshot == nullptr)
        return {};
    const Revision* source = ResolveSnapshotRevision(*_snapshot, _pending_drop.source, _history_revisions);
    const Revision* target = ResolveSnapshotRevision(*_snapshot, _pending_drop.target, _history_revisions);
    if (source == nullptr || target == nullptr || source == target)
        return {};
    if (_pending_drop.action == DropAction::Rebase)
    {
        if (_pending_drop.entire_branch)
            source = RebaseBranchRoot(*_snapshot, source->oid, target->oid, _history_revisions);
        if (source == nullptr || (source->parents.size() == 1 && source->parents.front() == target->oid))
            return {};
        return {source->oid};
    }
    if (_pending_drop.action == DropAction::Squash)
    {
        if (_pending_drop.entire_branch)
        {
            const Revision* root = RebaseBranchRoot(*_snapshot, source->oid, target->oid, _history_revisions);
            if (root != nullptr)
                source = root;
        }
        return {source->oid, target->oid};
    }

    // Compare the original and requested stack order. The unchanged prefix,
    // including a destination that remains the base, is only read by reorder.
    const auto path = [this](const Revision* tip, const Revision* base) {
        std::vector<std::string> segment;
        while (tip != nullptr)
        {
            segment.push_back(tip->oid);
            if (tip == base)
            {
                std::ranges::reverse(segment);
                return segment;
            }
            tip = tip->parents.size() == 1
                ? ResolveSnapshotRevision(*_snapshot, tip->parents.front(), _history_revisions) : nullptr;
        }
        return std::vector<std::string>{};
    };
    auto segment = path(source, target);
    if (segment.empty())
        segment = path(target, source);
    if (segment.empty())
        return {};
    auto reordered = segment;
    reordered.erase(std::ranges::find(reordered, source->oid));
    auto position = std::ranges::find(reordered, target->oid);
    if (DropPlacement(_pending_drop.action) == GG_REORDER_AFTER)
        ++position;
    reordered.insert(position, source->oid);
    for (std::size_t index = 0; index < segment.size(); ++index)
        if (segment[index] != reordered[index])
            return {segment[index]};
    return {};
}

bool Application::DialogModifiesLockedCommit() const
{
    if (_snapshot == nullptr)
        return false;
    switch (_dialog)
    {
    case Dialog::Commit: return _input_mode == 1 && RewritesLockedCommit(_dialog_revision);
    case Dialog::Metaedit:
    {
        const Revision* revision = ResolveSnapshotRevision(*_snapshot, _dialog_revision, _history_revisions);
        if (revision == nullptr)
            return false;
        const std::string author = revision->author + (revision->author_email.empty()
            ? "" : " <" + revision->author_email + ">");
        return (_input_primary != revision->description || _input_secondary != author)
            && RewritesLockedCommit(_dialog_revision);
    }
    case Dialog::Split: return RewritesLockedCommit(_dialog_revision);
    case Dialog::Abandon: return RewritesLockedCommit(_dialog_revision) || _abandon_modifies_locked;
    case Dialog::Rebase:
    {
        const Revision* source = RebaseSource();
        const Revision* destination = ResolveSnapshotRevision(*_snapshot, _input_primary, _history_revisions);
        if (source != nullptr && destination != nullptr && source->parents.size() == 1
            && source->parents.front() == destination->oid)
            return false;
        return RewritesLockedCommit(source == nullptr ? _input_secondary : source->oid);
    }
    case Dialog::Squash:
    {
        std::string destination = _input_secondary;
        if (destination.empty())
        {
            const auto source = std::ranges::find(_history_revisions, _dialog_revision, &Revision::oid);
            if (source != _history_revisions.end() && !source->parents.empty())
                destination = source->parents.front();
        }
        return RewritesLockedCommit(_dialog_revision) || RewritesLockedCommit(destination);
    }
    case Dialog::Restore:
        if (!_input_primary.empty()
            && ResolveSnapshotRevision(*_snapshot, _input_primary, _history_revisions)
                == ResolveSnapshotRevision(*_snapshot, _dialog_revision, _history_revisions))
            return false;
        return RewritesLockedCommit(_dialog_revision);
    case Dialog::Reconcile:
    {
        const Revision* source = RebaseBranchRoot(*_snapshot, _input_tertiary, _input_filesets, _history_revisions);
        return RewritesLockedCommit(source == nullptr ? _input_tertiary : source->oid);
    }
    case Dialog::ConfirmDrop:
    {
        return std::ranges::any_of(DropRewriteRoots(),
            [this](const std::string& oid) { return RewritesLockedCommit(oid); });
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
    std::erase_if(commands, [this](const Command& command) {
        const auto same_revision = [this](const auto& move) {
            if (_snapshot == nullptr)
                return false;
            const Revision* source = ResolveSnapshotRevision(*_snapshot, move.source, _history_revisions);
            return source != nullptr && source == ResolveSnapshotRevision(*_snapshot, move.destination, _history_revisions);
        };
        if (const auto* move = std::get_if<MoveFiles>(&command))
            return same_revision(*move);
        if (const auto* move = std::get_if<MoveDiffLines>(&command))
            return same_revision(*move);
        return false;
    });
    if (commands.empty())
        return;
    if (std::ranges::none_of(revisions, [this](const std::string& revision) { return RewritesLockedCommit(revision); }))
    {
        if (!commands.empty() && EnqueueAction(std::move(commands.front())))
            for (Command& command : commands | std::views::drop(1))
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
    return _snapshot == nullptr ? std::vector<std::string>{}
                                : FindAbandonRevisions(_snapshot->root, selected);
}

void Application::RequestAbandonRevisions(const std::string& revision)
{
    _abandon_revisions_requested = revision;
#ifdef IMGUI_BUILD_TESTING
    if (_test_snapshot_mode)
    {
        _abandon_revisions = {revision};
        for (std::size_t index = 0; index < _abandon_revisions.size(); ++index)
            for (const Revision& candidate : _history_revisions)
                if (std::ranges::find(candidate.parents, _abandon_revisions[index]) != candidate.parents.end()
                    && std::ranges::find(_abandon_revisions, candidate.oid) == _abandon_revisions.end())
                    _abandon_revisions.push_back(candidate.oid);
        _abandon_revisions_revision = revision;
        _abandon_revisions_complete = true;
        _abandon_remote_branches = RemoteBranchesAt(_abandon_revisions);
        _abandon_modifies_locked = std::ranges::any_of(
            _abandon_revisions, [this](const std::string& oid) { return IsLocked(oid); });
        return;
    }
#endif
    if ((_abandon_revisions_complete && _abandon_revisions_revision == revision)
        || _abandon_revisions_future.valid())
        return;
    _abandon_revisions_in_flight = revision;
    const std::string root = _snapshot == nullptr ? "" : _snapshot->root;
    _abandon_revisions_future = std::async(
        std::launch::async, [root, revision] { return FindAbandonRevisions(root, revision); });
}

void Application::PollAbandonRevisions()
{
    using namespace std::chrono_literals;
    if (!_abandon_revisions_future.valid()
        || _abandon_revisions_future.wait_for(0ms) != std::future_status::ready)
        return;
    std::vector<std::string> revisions = _abandon_revisions_future.get();
    const std::string completed = std::move(_abandon_revisions_in_flight);
    if (_abandon_revisions_requested == completed)
    {
        if (revisions.empty())
        {
            _error_message = "Could not determine the changes in this branch.";
            _input_flag_tertiary = false;
            _abandon_revisions = {_dialog_revision};
            _abandon_revisions_revision = _dialog_revision;
            _abandon_revisions_requested.clear();
            _abandon_revisions_complete = false;
            return;
        }
        _abandon_revisions = std::move(revisions);
        _abandon_revisions_revision = completed;
        _abandon_revisions_complete = true;
        _abandon_remote_branches = RemoteBranchesAt(_abandon_revisions);
        const std::unordered_set<std::string> abandoned(
            _abandon_revisions.begin(), _abandon_revisions.end());
        _abandon_modifies_locked = std::ranges::any_of(_history_revisions, [&](const Revision& revision) {
            return revision.pushed && (abandoned.contains(revision.oid)
                || std::ranges::any_of(revision.aliases,
                    [&](const std::string& alias) { return abandoned.contains(alias); }));
        });
        _input_flag = _input_flag || std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BRANCH
                && std::ranges::find(_abandon_revisions, ref.target) != _abandon_revisions.end();
        });
    }
    else if (!_abandon_revisions_requested.empty())
        RequestAbandonRevisions(_abandon_revisions_requested);
}

std::vector<RemoteBranchDelete> Application::RemoteBranchesAt(
    const std::vector<std::string>& revisions) const
{
    std::vector<RemoteBranchDelete> result;
    const std::unordered_set<std::string> revision_set(revisions.begin(), revisions.end());
    for (const NamedRef& local : _snapshot->refs)
    {
        if (local.kind != GG_NAMED_REF_LOCAL_BRANCH
            || !revision_set.contains(local.target))
            continue;
        for (const NamedRef& remote : _snapshot->refs)
        {
            if (remote.kind != GG_NAMED_REF_REMOTE_BRANCH || remote.name != local.name
                || remote.target != local.target || remote.remote.empty())
                continue;
            const RemoteBranchDelete deletion{local.name, remote.remote};
            if (std::ranges::none_of(result, [&](const RemoteBranchDelete& existing) {
                    return existing.branch == deletion.branch && existing.remote == deletion.remote;
                }))
                result.push_back(deletion);
        }
    }
    return result;
}

void Application::RequestAbandon(const std::string& revision, bool include_descendants)
{
    if (!_active_operation.empty() || revision.empty() || IsWorkingTreeRevision(revision))
        return;
    if (_selected_revision != revision)
        SelectRevision(revision);
    const auto selected = std::ranges::find(_history_revisions, revision, &Revision::oid);
    const bool has_refs = std::ranges::any_of(
        _snapshot->refs, [&](const NamedRef& ref) { return ref.target == revision; });
    if (!include_descendants && selected != _history_revisions.end() && selected->empty && !has_refs
        && !RewritesLockedCommit(revision))
        EnqueueAction(Abandon{{revision}, false, false, {}});
    else
    {
        OpenDialog(Dialog::Abandon);
        _input_flag_tertiary = include_descendants;
        if (include_descendants)
            RequestAbandonRevisions(revision);
        const std::vector<std::string>& revisions = _abandon_revisions;
        _input_flag = std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BRANCH
                && std::ranges::find(revisions, ref.target) != revisions.end();
        });
    }
}

void Application::RequestSquash(const std::string& revision, bool include_descendants)
{
    if (!_active_operation.empty() || revision.empty() || IsWorkingTreeRevision(revision))
        return;
    if (_selected_revision != revision)
        SelectRevision(revision);
    OpenDialog(Dialog::Squash);
    _input_flag_tertiary = include_descendants;
    _input_primary = SquashDescription(_history_revisions, revision, include_descendants);
    if (include_descendants)
    {
        const auto selected = std::ranges::find(_history_revisions, revision, &Revision::oid);
        if (selected != _history_revisions.end() && selected->parents.size() == 1)
            _input_secondary = selected->parents.front();
    }
}

bool Application::CanSubmitDialog() const
{
    if (_dialog == Dialog::Commit)
    {
        if (_snapshot == nullptr || !_snapshot->has_worktree
            || _snapshot->generation != _dialog_snapshot_generation)
            return false;
        if (_input_mode == 0)
            return _dialog_revision == MakeWorkingTreeHistoryItem(
                _snapshot->repository_generation, CurrentCommit(*_snapshot)).id;
        return _input_mode == 1 && !_dialog_revision.empty()
            && _dialog_revision == CurrentCommit(*_snapshot);
    }
    switch (_dialog)
    {
    case Dialog::Metaedit:
    case Dialog::Squash:
    case Dialog::Split:
    case Dialog::Abandon:
    case Dialog::Restore:
    case Dialog::ConfirmLocked:
        if (_snapshot == nullptr || _snapshot->generation != _dialog_snapshot_generation)
            return false;
        if (_dialog != Dialog::ConfirmLocked
            && ResolveSnapshotRevision(*_snapshot, _dialog_revision, _history_revisions) == nullptr)
            return false;
        break;
    default: break;
    }
    switch (_dialog)
    {
    case Dialog::Clone: return HasText(_input_primary) && HasText(_input_secondary);
    case Dialog::ConfirmBranchDelete:
        return _snapshot != nullptr && (_pending_branch_delete.local || !_pending_branch_delete.remotes.empty());
    case Dialog::ConfirmWorkingFiles:
        return _snapshot != nullptr && !_pending_working_files.empty();
    case Dialog::Rebase:
        return HasText(_input_primary) && _snapshot != nullptr
            && _snapshot->generation == _dialog_snapshot_generation
            && CurrentCommit(*_snapshot) == _input_secondary;
    case Dialog::Squash:
        return (!_input_flag_tertiary || !_input_secondary.empty())
            && (_input_secondary.empty()
                || ResolveSnapshotRevision(*_snapshot, _input_secondary, _history_revisions) != nullptr);
    case Dialog::Split: return HasText(_input_filesets);
    case Dialog::Abandon:
        return !_input_flag_tertiary
            || (_abandon_revisions_complete && _abandon_revisions_revision == _dialog_revision);
    case Dialog::Branch:
    case Dialog::Tag:
        // Creating a ref only needs its target to exist. Background refreshes
        // must not invalidate the dialog as they do for rewriting operations.
        return HasText(_input_primary) && _snapshot != nullptr
            && (HasText(_input_secondary)
                || ResolveSnapshotRevision(*_snapshot, _dialog_revision, _history_revisions) != nullptr);
    case Dialog::BranchRename:
        return HasText(_input_primary) && _input_primary != _input_secondary && _snapshot != nullptr
            && std::ranges::none_of(_snapshot->refs, [this](const NamedRef& ref) {
                   return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == _input_primary;
               });
    case Dialog::WorkspaceAdd:
    case Dialog::WorkspaceRename:
    case Dialog::WorkspaceRemove: return HasText(_input_primary);
    case Dialog::RemoteAdd: return HasText(_input_primary) && HasText(_input_secondary);
    case Dialog::PushTo: return HasText(_input_primary) && HasText(_input_secondary);
    case Dialog::Reconcile:
        return _snapshot != nullptr && _snapshot->generation == _dialog_snapshot_generation
            && std::ranges::any_of(_snapshot->refs, [this](const NamedRef& ref) {
                   return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == _input_primary
                       && ref.target == _input_tertiary;
               })
            && std::ranges::any_of(_snapshot->refs, [this](const NamedRef& ref) {
                   return ref.kind == GG_NAMED_REF_REMOTE_BRANCH && ref.tracked
                       && ref.name == _input_primary && ref.remote == _input_secondary
                       && ref.target == _input_filesets;
               })
            && ClassifyBranchRelation(*_snapshot, _input_tertiary, _input_filesets)
                == BranchRelation::Diverged;
    case Dialog::ConfirmBranchMove:
        return _snapshot != nullptr && _snapshot->generation == _dialog_snapshot_generation
            && std::ranges::any_of(_snapshot->refs, [this](const NamedRef& ref) {
                   return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == _input_primary
                       && ref.target == _input_secondary;
               });
    case Dialog::ConfirmDrop:
        return _snapshot != nullptr && _snapshot->generation == _dialog_snapshot_generation
            && ResolveSnapshotRevision(*_snapshot, _pending_drop.source, _history_revisions) != nullptr
            && ResolveSnapshotRevision(*_snapshot, _pending_drop.target, _history_revisions) != nullptr;
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
    _selected_files = {path};
    _selected_files_revision = _diff.revision;
    _file_selection_anchor = path;
    FocusFile(path);
}

void Application::FocusFile(const std::string& path)
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
    _working_tree_diff_outdated = false;
    ClearBlame();
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
    _revision_prefixes.clear();
    _operation_prefixes.clear();
    _history_refs_by_revision.clear();
    _selected_revision.clear();
    _selected_revisions.clear();
    _visible_branches.clear();
    _visible_branches_user_selected = false;
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
    _branch_filter.clear();
    _tag_filter.clear();
    _reflog_filter.clear();
    _changes_filter.clear();
    _graph_filter.clear();
    _built_filter.clear();
    _built_branches.clear();
    _diff_loading = false;
    _blame_loading = false;
    _background_activities.clear();
    _default_layout = true;
    _status_message.clear();
    _error_message.clear();
    _pending_created_branch.clear();
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

std::vector<const StatusEntry*> Application::VisibleChangedFiles() const
{
    std::vector<const StatusEntry*> files;
    for (const StatusEntry& file : _diff.files)
        if (FileMatchesFilter(file))
            files.push_back(&file);
    return files;
}

bool Application::IsChangedFileSelected(const std::string& path) const
{
    const bool multiple = SameDiffRevision(_selected_files_revision, _diff.revision)
        && std::ranges::contains(_selected_files, _selected_file);
    return multiple ? std::ranges::contains(_selected_files, path) : path == _selected_file;
}

std::vector<const StatusEntry*> Application::SelectedChangedFiles() const
{
    std::vector<const StatusEntry*> result;
    for (const StatusEntry& file : _diff.files)
        if (IsChangedFileSelected(file.path))
            result.push_back(&file);
    return result;
}

void Application::ClickChangedFile(const std::string& path, bool toggle, bool range)
{
    const std::vector<const StatusEntry*> visible = VisibleChangedFiles();
    const auto position = [&](const std::string& value) {
        return std::ranges::find(visible, value, &StatusEntry::path);
    };
    std::vector<std::string> selection;
    for (const StatusEntry* file : SelectedChangedFiles())
        selection.push_back(file->path);
    if (range && position(_file_selection_anchor) != visible.end() && position(path) != visible.end())
    {
        auto first = position(_file_selection_anchor);
        auto last = position(path);
        if (last < first)
            std::swap(first, last);
        if (!toggle)
            selection.clear();
        for (auto file = first; file <= last; ++file)
            if (!std::ranges::contains(selection, (*file)->path))
                selection.push_back((*file)->path);
    }
    else if (toggle)
    {
        _file_selection_anchor = path;
        if (const auto selected = std::ranges::find(selection, path); selected == selection.end())
            selection.push_back(path);
        else if (selection.size() > 1)
        {
            selection.erase(selected);
            _selected_files = selection;
            _selected_files_revision = _diff.revision;
            // Removing the focused file shows another selected one.
            if (path == _selected_file)
                FocusFile(selection.back());
            return;
        }
    }
    else
    {
        SelectFile(path);
        return;
    }
    _selected_files = std::move(selection);
    _selected_files_revision = _diff.revision;
    if (path != _selected_file)
        FocusFile(path);
}

void Application::SelectAllChangedFiles()
{
    const std::vector<const StatusEntry*> visible = VisibleChangedFiles();
    if (visible.empty())
        return;
    _selected_files.clear();
    for (const StatusEntry* file : visible)
        _selected_files.push_back(file->path);
    _selected_files_revision = _diff.revision;
    if (!std::ranges::contains(_selected_files, _selected_file))
        FocusFile(_selected_files.front());
}

void Application::NavigateChangedFile(int direction, bool extend)
{
    const std::vector<const StatusEntry*> files = VisibleChangedFiles();
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
    if (extend)
        ClickChangedFile((*selected)->path, false, true);
    else
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
    const auto current = std::ranges::find(_history_revisions, CurrentCommit(*_snapshot), &Revision::oid);
    const bool direct_parent = current != _history_revisions.end() && current->parents.size() == 1
        && current->parents.front() == _diff.revision;
    const bool changed_in_working_copy = std::ranges::any_of(_snapshot->status, [&](const StatusEntry& file) {
        return file.status != GIT_DELTA_UNMODIFIED && file.status != GIT_DELTA_IGNORED
            && (file.path == path || file.old_path == path);
    });
    if (!IsWorkingTreeRevision(_diff.revision) && _diff.revision != CurrentCommit(*_snapshot)
        && (!direct_parent || changed_in_working_copy))
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
        _error_message = "File is unavailable in the working tree";
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
    if (_snapshot == nullptr || _selected_revision.empty() || IsWorkingTreeRevision(_selected_revision)
        || path.empty() || !_active_operation.empty())
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
        const std::optional<std::string> current = TreeBlobContents(repository.get(), tree.get(), path.c_str());
        const std::string initial = current.has_value() && !IsGgConflictFile(*current)
            ? *current
            : StandardMergeContents(repository.get(), *conflict, path);
        std::ofstream output(_merge_result_path, std::ios::binary | std::ios::trunc);
        if (!initial.empty())
            output.write(initial.data(), static_cast<std::streamsize>(initial.size()));
        if (!output)
            throw std::runtime_error("Could not prepare the merge result file");

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

        const char* raw_git_directory = git_repository_path(repository.get());
        if (raw_git_directory == nullptr)
            throw std::runtime_error("The repository has no git directory");
        const std::string git_directory(raw_git_directory);
        const std::string worktree_path = worktree.string();
        const char* arguments[]{"git", "-C", worktree_path.c_str(), "--git-dir", git_directory.c_str(), "mergetool",
            "--no-prompt", "--", path.c_str(), nullptr};
        SDL_Environment* environment = SDL_CreateEnvironment(true);
        if (environment == nullptr
            || !SDL_SetEnvironmentVariable(environment, "GIT_INDEX_FILE", index_path.string().c_str(), true))
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
    if (_snapshot == nullptr
        || (_selected_revision != CurrentCommit(*_snapshot) && !IsWorkingTreeRevision(_selected_revision))
        || path.empty())
        return;
    const std::optional<std::filesystem::path> working_path = WorkingCopyPath(_snapshot->root, path);
    std::error_code error;
    if (!working_path.has_value()
        || (std::filesystem::exists(*working_path, error) && !std::filesystem::is_regular_file(*working_path, error))
        || error)
    {
        _error_message = "Resolved file is unavailable in the working tree";
        return;
    }
    EnqueueAction(Refresh{true, {}, true});
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
                if (!input)
                    throw std::runtime_error("Could not open the merge result file");
                resolution.contents.assign(
                    std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
                if (input.bad())
                    throw std::runtime_error("Could not read the merge result file");
                resolution.present = true;
            }
            else if (std::filesystem::exists(_merge_result_path))
            {
                throw std::runtime_error("The merge result is not a file");
            }
            EnqueueAction(std::move(resolution));
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

void Application::OpenWorkspaceInNewWindow(const std::string& path)
{
    std::filesystem::path executable_path = _executable_path;
    if (!std::filesystem::exists(executable_path))
    {
        if (const char* base = SDL_GetBasePath(); base != nullptr)
            executable_path = std::filesystem::path(base) / _executable_path.filename();
    }
    if (executable_path.empty() || !std::filesystem::exists(executable_path))
    {
        _error_message = "Cannot determine the ggui executable path";
        return;
    }
    const std::string executable = executable_path.string();
    const char* arguments[]{executable.c_str(), path.c_str(), nullptr};
    SDL_Process* process = StartBackgroundProcess(arguments); // GCOV_EXCL_LINE: external application handoff
    if (process == nullptr)
        _error_message = SDL_GetError(); // GCOV_EXCL_LINE: platform process failure
    else
    {
        SDL_DestroyProcess(process); // GCOV_EXCL_LINE: external process owns its lifetime
        _status_message = "Workspace opened in a new window";
    }
}

void Application::OpenExternalDiff(const std::string& path, const std::string& compare_to)
{
    if (_snapshot == nullptr || _diff.revision.empty() || IsWorkingTreeRevision(_diff.revision)
        || path.empty())
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
        + (compare_to.empty() && _diff.revision == CurrentCommit(*_snapshot) ? "^" : "^!");
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
        EnqueueAction(initialize ? Command{InitRepository{path}} : Command{OpenRepository{path}});
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

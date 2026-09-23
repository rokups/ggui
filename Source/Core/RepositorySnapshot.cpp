// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <git2/reflog.h>
#if __has_include(<git2-experimental/sys/errors.h>)
#include <git2-experimental/sys/errors.h>
#else
#include <git2/sys/errors.h>
#endif

#include <algorithm>
#include <iterator>
#include <memory>
#include <tuple>
#include <utility>

namespace Ggui
{
using namespace RepositoryInternal;

namespace
{

struct ReflogDeleter
{
    void operator()(git_reflog* value) const { git_reflog_free(value); }
};
using ReflogPtr = std::unique_ptr<git_reflog, ReflogDeleter>;

std::string ReflogOid(const git_oid* oid)
{
    return oid == nullptr || git_oid_is_zero(oid) != 0 ? std::string{} : OidString(*oid);
}

bool CommitAvailable(git_repository* repository, const git_oid* oid)
{
    if (oid == nullptr || git_oid_is_zero(oid) != 0)
        return false;
    git_commit* raw = nullptr;
    const int result = git_commit_lookup(&raw, repository, oid);
    if (result == GIT_OK)
    {
        git_commit_free(raw);
        return true;
    }
    // Reflogs can retain entries after their objects have been pruned. A
    // missing object is expected and should not make an otherwise usable
    // snapshot fail.
    if (result == GIT_ENOTFOUND)
    {
        git_error_clear();
        return false;
    }
    Check(result, "check reflog commit");
    return false; // Check throws for every non-success result.
}

void ReadHeadReflog(RepoSnapshot& destination, git_repository* repository)
{
    git_reflog* raw = nullptr;
    const int result = git_reflog_read(&raw, repository, "HEAD");
    if (result == GIT_ENOTFOUND || result == GIT_EUNBORNBRANCH)
    {
        git_error_clear();
        return;
    }
    Check(result, "load HEAD reflog");
    ReflogPtr reflog(raw);
    const std::size_t count = git_reflog_entrycount(reflog.get());
    destination.reflog.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const git_reflog_entry* source = git_reflog_entry_byindex(reflog.get(), index);
        if (source == nullptr)
            continue;
        const git_oid* old_oid = git_reflog_entry_id_old(source);
        const git_oid* new_oid = git_reflog_entry_id_new(source);
        ReflogEntry entry;
        entry.index = index;
        entry.old_hash = ReflogOid(old_oid);
        entry.new_hash = ReflogOid(new_oid);
        entry.old_commit_available = CommitAvailable(repository, old_oid);
        entry.new_commit_available = CommitAvailable(repository, new_oid);
        if (const git_signature* committer = git_reflog_entry_committer(source); committer != nullptr)
        {
            entry.author = committer->name == nullptr ? "" : committer->name;
            entry.author_email = committer->email == nullptr ? "" : committer->email;
            entry.timestamp = committer->when.time;
        }
        const char* message = git_reflog_entry_message(source);
        entry.message = message == nullptr ? "" : message;
        destination.reflog.push_back(std::move(entry));
    }
}

} // namespace

void RepositoryEngine::Impl::Sync(bool report_progress)
{
    if (gg == nullptr)
        return;
    gg_operation_options options = OperationOptions(report_progress);
    Check(gg_repository_adopt_git_history_ex(gg, false, &options), "adopt external Git history");
}

std::shared_ptr<RepoSnapshot> RepositoryEngine::Impl::ReadSnapshot(
    bool include_worktree, bool history_changed, const std::vector<std::string>& status_paths)
{
    auto result = std::make_shared<RepoSnapshot>();
    result->generation = ++generation;
    const char* workdir = git_repository_workdir(git.get());
    result->has_worktree = workdir != nullptr;
    result->root = workdir == nullptr ? git_repository_path(git.get()) : workdir;

    git_oid working{};
    if (gg_repository_working_copy(&working, gg) == GIT_OK)
        result->working_copy = OidString(working);

    git_reference* raw_head = nullptr;
    if (git_repository_head(&raw_head, git.get()) == GIT_OK)
    {
        std::unique_ptr<git_reference, decltype(&git_reference_free)> head(raw_head, git_reference_free);
        if (const git_oid* target = git_reference_target(head.get()); target != nullptr)
            result->head = OidString(*target);
    }

    NamedRefs refs;
    Check(gg_repository_named_refs(&refs.value, gg), "load refs");
    result->refs.reserve(refs.value.count);
    for (size_t index = 0; index < refs.value.count; ++index)
    {
        const gg_named_ref& source = refs.value.items[index];
        result->refs.push_back({source.name == nullptr ? "" : source.name,
            source.remote == nullptr ? "" : source.remote, OidString(source.target), source.kind,
            source.tracked != 0, source.conflicted != 0});
    }
    for (NamedRef& remote : result->refs)
    {
        if (remote.kind != GG_NAMED_REF_REMOTE_BOOKMARK)
            continue;
        const auto local = std::ranges::find_if(result->refs, [&](const NamedRef& candidate) {
            return candidate.kind == GG_NAMED_REF_LOCAL_BOOKMARK && candidate.name == remote.name;
        });
        if (local == result->refs.end())
            continue;
        git_oid local_oid{};
        git_oid remote_oid{};
        if (git_oid_fromstr(&local_oid, local->target.c_str(), git_repository_oid_type(git.get())) != GIT_OK
            || git_oid_fromstr(&remote_oid, remote.target.c_str(), git_repository_oid_type(git.get())) != GIT_OK)
            continue;
        remote.desync_known = git_graph_ahead_behind(
            &remote.local_commits, &remote.remote_commits, git.get(), &local_oid, &remote_oid) == GIT_OK;
    }
    result->worktree_state = !worktree_ready ? RepoSnapshot::WorktreeState::Unscanned
        : worktree_status_stale ? RepoSnapshot::WorktreeState::Stale
                                : RepoSnapshot::WorktreeState::Ready;
    if (include_worktree)
    {
        gg_status_options status_options = GG_STATUS_OPTIONS_INIT;
        const bool incremental_status = worktree_ready && !worktree_status_stale && !status_paths.empty();
        const bool defer_partial_status = !status_paths.empty() && !incremental_status;
        StringArray selected_status_paths(status_paths);
        if (incremental_status)
            status_options.filesets = selected_status_paths.Get();
        if (!defer_partial_status)
        {
            Status status;
            Check(gg_repository_worktree_status(&status.value, gg, &status_options), "load working-tree status");
            std::vector<StatusEntry> updated;
            updated.reserve(status.value.entry_count);
            for (size_t index = 0; index < status.value.entry_count; ++index)
            {
                const gg_status_entry& source = status.value.entries[index];
                updated.push_back({source.old_path == nullptr ? "" : source.old_path,
                    source.new_path == nullptr ? "" : source.new_path, source.status, source.conflicted != 0});
            }
            if (!incremental_status)
                cached_status = std::move(updated);
            else
            {
                // Watcher paths are individual files. Replace only the cached
                // entries touching those paths, including both sides of a rename.
                const auto touched = [&](const StatusEntry& entry) {
                    const auto selected = [&](const std::string& candidate, const std::string& path) {
                        return candidate == path
                            || (candidate.size() > path.size()
                                && candidate.compare(0, path.size(), path) == 0
                                && candidate[path.size()] == '/');
                    };
                    return std::ranges::any_of(status_paths, [&](const std::string& path) {
                        return selected(entry.path, path) || selected(entry.old_path, path);
                    });
                };
                std::erase_if(cached_status, touched);
                cached_status.insert(cached_status.end(),
                    std::make_move_iterator(updated.begin()), std::make_move_iterator(updated.end()));
            }
            worktree_ready = true;
            worktree_status_stale = false;
        }
        result->status = cached_status;
        result->worktree_state = !worktree_ready ? RepoSnapshot::WorktreeState::Unscanned
            : worktree_status_stale ? RepoSnapshot::WorktreeState::Stale
                                    : RepoSnapshot::WorktreeState::Ready;
    }
    else if (worktree_ready)
        result->status = cached_status;

    Operations operations;
    Check(gg_repository_operations(&operations.value, gg, 200), "load operations");
    result->operations.reserve(operations.value.count);
    for (size_t index = 0; index < operations.value.count; ++index)
    {
        const gg_operation& source = operations.value.items[index];
        result->operations.push_back(
            {OidString(source.oid), source.description == nullptr ? "" : source.description, source.time});
    }

    // The HEAD reflog is deliberately loaded independently of gg's operation
    // journal. It includes moves made by external Git clients and therefore is
    // the reliable recovery trail shown by the Reflog panel.
    ReadHeadReflog(*result, git.get());

    gg_operation_capabilities capabilities{};
    Check(gg_repository_operation_capabilities(&capabilities, gg), "load operation capabilities");
    result->can_undo = capabilities.can_undo != 0;
    result->can_redo = capabilities.can_redo != 0;

    Workspaces workspaces;
    Check(gg_repository_workspaces(&workspaces.value, gg), "load workspaces");
    result->workspaces.reserve(workspaces.value.count);
    for (size_t index = 0; index < workspaces.value.count; ++index)
    {
        const gg_workspace& source = workspaces.value.items[index];
        result->workspaces.push_back({source.name == nullptr ? "" : source.name,
            source.root == nullptr ? "" : source.root,
            source.has_working_copy != 0 ? OidString(source.working_copy) : "",
            source.stale != 0, source.managed != 0, source.current != 0, source.primary != 0});
    }

    GitStringArray remote_names;
    Check(git_remote_list(&remote_names.value, git.get()), "load remotes");
    result->remotes.reserve(remote_names.value.count);
    for (size_t index = 0; index < remote_names.value.count; ++index)
    {
        git_remote* raw_remote = nullptr;
        Check(git_remote_lookup(&raw_remote, git.get(), remote_names.value.strings[index]), "load remote");
        std::unique_ptr<git_remote, decltype(&git_remote_free)> remote(raw_remote, git_remote_free);
        const char* fetch = git_remote_url(remote.get());
        const char* push = git_remote_pushurl(remote.get());
        result->remotes.push_back(
            {remote_names.value.strings[index], fetch == nullptr ? "" : fetch, push == nullptr ? "" : push});
    }

    if (!result->working_copy.empty())
    {
        Conflicts conflicts;
        Check(gg_repository_conflicts(&conflicts.value, gg, &working), "load conflicts");
        AppendConflicts(result->conflicts, conflicts.value);
    }
    {
        std::lock_guard lock(snapshot_mutex);
        const bool same_topology = !history_changed && latest_snapshot != nullptr
            && latest_snapshot->root == result->root
            && latest_snapshot->working_copy == result->working_copy
            && latest_snapshot->head == result->head
            && latest_snapshot->refs.size() == result->refs.size()
            && std::ranges::equal(latest_snapshot->refs, result->refs, {},
                [](const NamedRef& ref) { return std::tie(ref.name, ref.remote, ref.target, ref.kind); },
                [](const NamedRef& ref) { return std::tie(ref.name, ref.remote, ref.target, ref.kind); });
        result->repository_generation = same_topology && latest_snapshot != nullptr
            ? latest_snapshot->repository_generation : ++topology_generation;
        latest_snapshot = result;
    }
    return result;
}

void RepositoryEngine::Impl::PublishSnapshot(
    bool include_worktree, bool history_changed, const std::vector<std::string>& status_paths)
{
    if (gg != nullptr)
    {
        std::shared_ptr<RepoSnapshot> snapshot = ReadSnapshot(include_worktree, history_changed, status_paths);
        // History is request-versioned separately. The UI rebuilds it with
        // its persisted head selections after observing this generation.
        Post(SnapshotReady{std::move(snapshot)});
    }
}

void RepositoryEngine::Impl::InvalidateWorktreeStatus()
{
    cached_status.clear();
    worktree_ready = false;
    worktree_status_stale = false;
}

void RepositoryEngine::Impl::MarkWorktreeStatusStale()
{
    if (worktree_ready)
        worktree_status_stale = true;
}

void RepositoryEngine::Impl::PublishWorktreeChanges(
    const std::vector<std::string>& paths, bool history_changed)
{
    if (!paths.empty() && worktree_ready && !worktree_status_stale)
        PublishSnapshot(true, history_changed, paths);
    else
        PublishSnapshot(false, history_changed);
}

} // namespace Ggui

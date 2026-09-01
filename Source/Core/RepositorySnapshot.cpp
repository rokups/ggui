// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <algorithm>
#include <memory>
#include <tuple>
#include <utility>

namespace Ggui
{
using namespace RepositoryInternal;

bool RepositoryEngine::Impl::Sync(bool report_progress, const std::vector<std::string>& paths)
{
    if (gg == nullptr)
        return false;
    gg_operation_options options = OperationOptions(report_progress);
    Check(gg_repository_adopt_git_history_ex(gg, false, &options), "adopt external Git history");
    int changed = 0;
    if (paths.empty())
        Check(gg_repository_snapshot_working_copy(&changed, gg, &options), "snapshot working copy");
    else
    {
        StringArray selected(paths);
        Check(gg_repository_snapshot_working_copy_paths(&changed, gg, selected.Get(), &options),
            "snapshot working copy paths");
    }
    return changed != 0;
}

std::shared_ptr<RepoSnapshot> RepositoryEngine::Impl::ReadSnapshot(bool include_worktree, bool history_changed)
{
    auto result = std::make_shared<RepoSnapshot>();
    result->generation = ++generation;
    const char* workdir = git_repository_workdir(git.get());
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
    result->worktree_state = include_worktree || worktree_ready
        ? RepoSnapshot::WorktreeState::Ready
        : RepoSnapshot::WorktreeState::Unscanned;
    if (include_worktree)
    {
        gg_status_options status_options = GG_STATUS_OPTIONS_INIT;
        Status status;
        Check(gg_repository_status(&status.value, gg, &status_options), "load status");
        result->status.reserve(status.value.entry_count);
        for (size_t index = 0; index < status.value.entry_count; ++index)
        {
            const gg_status_entry& source = status.value.entries[index];
            result->status.push_back({source.old_path == nullptr ? "" : source.old_path,
                source.new_path == nullptr ? "" : source.new_path, source.status, source.conflicted != 0});
        }
        cached_status = result->status;
        worktree_ready = true;
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
            source.stale != 0, source.managed != 0});
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

void RepositoryEngine::Impl::PublishSnapshot(bool include_worktree, bool history_changed)
{
    if (gg != nullptr)
    {
        std::shared_ptr<RepoSnapshot> snapshot = ReadSnapshot(include_worktree, history_changed);
        RequestClosestBookmark(snapshot);
        // History is request-versioned separately. The UI rebuilds it with
        // its persisted head selections after observing this generation.
        Post(SnapshotReady{std::move(snapshot)});
    }
}

} // namespace Ggui

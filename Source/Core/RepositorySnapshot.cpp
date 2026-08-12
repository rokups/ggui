// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <memory>
#include <utility>

namespace Ggui
{
using namespace RepositoryInternal;

void RepositoryEngine::Impl::Sync(bool report_progress)
{
    if (gg == nullptr)
        return;
    gg_operation_options options = OperationOptions(report_progress);
    Check(gg_repository_adopt_git_history(gg, &options), "adopt external Git history");
    int changed = 0;
    Check(gg_repository_snapshot_working_copy(&changed, gg, &options), "snapshot working copy");
}

std::shared_ptr<RepoSnapshot> RepositoryEngine::Impl::ReadSnapshot()
{
    auto result = std::make_shared<RepoSnapshot>();
    result->generation = ++generation;
    const char* workdir = git_repository_workdir(git.get());
    result->root = workdir == nullptr ? git_repository_path(git.get()) : workdir;

    git_oid working{};
    if (gg_repository_working_copy(&working, gg) == GIT_OK)
        result->working_copy = OidString(working);

    gg_revision_query_options query = GG_REVISION_QUERY_OPTIONS_INIT;
    query.revisions = "all()";
    Revisions revisions;
    Check(gg_repository_revisions(&revisions.value, gg, &query), "load revisions");
    result->revisions.reserve(revisions.value.count);
    for (size_t index = 0; index < revisions.value.count; ++index)
    {
        const gg_revision& source = revisions.value.items[index];
        Revision value;
        value.oid = OidString(source.oid);
        for (size_t parent = 0; parent < source.parents.count; ++parent)
            value.parents.push_back(OidString(source.parents.ids[parent]));
        value.aliases.reserve(source.aliases.count);
        for (size_t alias = 0; alias < source.aliases.count; ++alias)
            value.aliases.push_back(OidString(source.aliases.ids[alias]));
        value.description = source.description == nullptr ? "" : source.description;
        value.author = source.author == nullptr || source.author->name == nullptr ? "" : source.author->name;
        value.author_email = source.author == nullptr || source.author->email == nullptr ? "" : source.author->email;
        value.timestamp = source.committer == nullptr ? 0 : source.committer->when.time;
        value.working_copy = value.oid == result->working_copy;
        value.conflicted = source.has_conflicts != 0;
        value.empty = CommitIsEmpty(git.get(), source.oid);
        result->revisions.push_back(std::move(value));
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
    MarkPushedRevisions(result->revisions, result->refs, git.get());

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
            source.root == nullptr ? "" : source.root, OidString(source.working_copy), source.stale != 0});
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
    return result;
}

void RepositoryEngine::Impl::PublishSnapshot()
{
    if (gg != nullptr)
        Post(SnapshotReady{ReadSnapshot()});
}

} // namespace Ggui

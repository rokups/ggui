// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace Ggui
{
using namespace RepositoryInternal;

void RepositoryEngine::Impl::DispatchMutation(const Command& command)
{
    std::visit(
        Overloaded{
            [&](const NewChange& value) {
                gg_new_options options = GG_NEW_OPTIONS_INIT;
                const StringArray parents(value.parents), before(value.insert_before), after(value.insert_after);
                options.message = value.message.c_str();
                options.parents = parents.Get();
                options.insert_before = before.Get();
                options.insert_after = after.Get();
                options.no_edit = value.no_edit;
                options.detach = value.detach;
                Mutate("create change", [&](auto* out, auto* operation) {
                    return gg_repository_new_change(out, gg, &options, operation);
                });
            },
            [&](const Describe& value) {
                gg_describe_options options = GG_DESCRIBE_OPTIONS_INIT;
                const std::vector<std::string> revisions{value.revision};
                const StringArray array(revisions);
                options.revisions = array.Get();
                options.message = value.message.c_str();
                options.message_provided = 1;
                Mutate("describe change", [&](auto* out, auto* operation) {
                    return gg_repository_describe(out, gg, &options, operation);
                });
            },
            [&](const Metaedit& value) {
                gg_metaedit_options options = GG_METAEDIT_OPTIONS_INIT;
                const std::vector<std::string> revisions{value.revision};
                const StringArray array(revisions);
                options.revisions = array.Get();
                options.message = value.message ? value.message->c_str() : nullptr;
                options.author = value.author.c_str();
                options.message_provided = value.message.has_value();
                options.author_provided = !value.author.empty();
                Mutate("edit metadata", [&](auto* out, auto* operation) {
                    return gg_repository_metaedit(out, gg, &options, operation);
                });
            },
            [&](const Edit& value) {
                Mutate("edit change", [&](auto* out, auto* operation) {
                    return gg_repository_edit(out, gg, value.revision.c_str(), operation);
                });
            },
            [&](const MoveChange& value) {
                gg_move_options options = GG_MOVE_OPTIONS_INIT;
                options.direction = value.direction;
                options.offset = value.offset;
                options.conflict = value.conflict;
                Mutate("move working copy", [&](auto* out, auto* operation) {
                    return gg_repository_move(out, gg, &options, operation);
                });
            },
            [&](const Commit& value) {
                gg_commit_options options = GG_COMMIT_OPTIONS_INIT;
                options.message = value.message.c_str();
                options.message_provided = 1;
                CaptureWorktree("commit working tree", [&](auto* out, auto* operation) {
                    return gg_repository_commit(out, gg, &options, operation);
                });
            },
            [&](const Amend& value) {
                CaptureWorktree("amend commit", [&](auto* out, auto* operation) {
                    return gg_repository_amend(out, gg, value.revision.c_str(),
                        value.message.empty() ? nullptr : value.message.c_str(), operation);
                });
            },
            [&](const Rebase& value) {
                Mutate("rebase change", [&](auto* out, auto* operation) {
                    std::string source = value.source;
                    if (value.entire_branch)
                    {
                        git_oid source_oid{}, destination_oid{}, branch_oid{};
                        Check(gg_repository_resolve(&source_oid, gg, value.source.c_str()),
                            "resolve rebase source");
                        Check(gg_repository_resolve(&destination_oid, gg, value.destination.c_str()),
                            "resolve rebase destination");
                        const std::string branch = "roots(ancestors(" + OidString(source_oid)
                            + ") ~ ancestors(" + OidString(destination_oid) + "))";
                        Check(gg_repository_resolve(&branch_oid, gg, branch.c_str()),
                            "find rebase branch divergence");
                        source = OidString(branch_oid);
                    }
                    gg_rebase_options options = GG_REBASE_OPTIONS_INIT;
                    options.source = source.c_str();
                    options.destination = value.destination.c_str();
                    return gg_repository_rebase(out, gg, &options, operation);
                });
            },
            [&](const Duplicate& value) {
                gg_duplicate_options options = GG_DUPLICATE_OPTIONS_INIT;
                options.revision = value.revision.c_str();
                options.descendants = value.descendants;
                Mutate("duplicate change", [&](auto* out, auto* operation) {
                    return gg_repository_duplicate(out, gg, &options, operation);
                });
            },
            [&](const Reorder& value) {
                gg_reorder_options options = GG_REORDER_OPTIONS_INIT;
                options.source = value.source.c_str();
                options.target = value.target.c_str();
                options.placement = value.placement;
                options.copy = value.copy;
                Mutate("reorder change", [&](auto* out, auto* operation) {
                    return gg_repository_reorder(out, gg, &options, operation);
                });
            },
            [&](const Split& value) {
                gg_split_options options = GG_SPLIT_OPTIONS_INIT;
                const StringArray filesets(value.filesets);
                options.revision = value.revision.c_str();
                options.message = value.message.c_str();
                options.filesets = filesets.Get();
                Mutate("split change", [&](auto* out, auto* operation) {
                    return gg_repository_split(out, gg, &options, operation);
                });
            },
            [&](const Squash& value) {
                if (value.descendants)
                {
                    Sync();
                    git_oid selected{};
                    Check(gg_repository_resolve(&selected, gg, value.source.c_str()), "resolve squash source");
                    git_commit* raw_selected = nullptr;
                    Check(git_commit_lookup(&raw_selected, git.get(), &selected), "load squash source");
                    std::unique_ptr<git_commit, decltype(&git_commit_free)>
                        selected_commit(raw_selected, git_commit_free);
                    if (git_commit_parentcount(selected_commit.get()) != 1)
                        throw std::runtime_error("squash source must have exactly one parent");
                    const std::string destination = OidString(*git_commit_parent_id(selected_commit.get(), 0));

                    git_revwalk* raw_walk = nullptr;
                    Check(git_revwalk_new(&raw_walk, git.get()), "walk squash descendants");
                    std::unique_ptr<git_revwalk, decltype(&git_revwalk_free)> walk(raw_walk, git_revwalk_free);
                    const auto push_glob = [&](const char* pattern) {
                        const int result = git_revwalk_push_glob(walk.get(), pattern);
                        if (result != GIT_OK && result != GIT_ENOTFOUND)
                            Check(result, "find squash descendants");
                    };
                    push_glob("refs/*");
                    git_oid working{};
                    if (gg_repository_working_copy(&working, gg) == GIT_OK)
                        Check(git_revwalk_push(walk.get(), &working), "find working-copy descendants");

                    std::unordered_map<std::string, std::vector<git_oid>> children;
                    git_oid candidate{};
                    while (git_revwalk_next(&candidate, walk.get()) == GIT_OK)
                    {
                        git_oid current = candidate;
                        const std::string candidate_id = OidString(candidate);
                        if (gg_repository_resolve(&current, gg, candidate_id.c_str()) != GIT_OK)
                            continue;
                        git_commit* raw_commit = nullptr;
                        Check(git_commit_lookup(&raw_commit, git.get(), &current), "load squash descendant");
                        std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
                        // A merge with an outside branch is a convergence
                        // boundary, not part of either branch being squashed.
                        if (git_commit_parentcount(commit.get()) == 1)
                            children[OidString(*git_commit_parent_id(commit.get(), 0))].push_back(current);
                    }
                    std::vector<git_oid> revisions;
                    std::deque<git_oid> pending{selected};
                    std::unordered_set<std::string> revision_ids;
                    while (!pending.empty())
                    {
                        const git_oid revision = pending.front();
                        pending.pop_front();
                        const std::string id = OidString(revision);
                        if (!revision_ids.insert(id).second) continue;
                        revisions.push_back(revision);
                        if (const auto found = children.find(id); found != children.end())
                            pending.insert(pending.end(), found->second.begin(), found->second.end());
                    }
                    std::vector<std::string> tips;
                    for (const git_oid& revision : revisions)
                    {
                        const std::string id = OidString(revision);
                        const auto found = children.find(id);
                        if (found == children.end()
                            || std::ranges::none_of(found->second, [&](const git_oid& child) {
                                   return revision_ids.contains(OidString(child));
                               }))
                            tips.push_back(id);
                    }
                    std::ranges::sort(tips);
                    try
                    {
                        for (const std::string& tip : tips)
                        {
                            git_oid current_tip{}, current_destination{};
                            Check(gg_repository_resolve(&current_tip, gg, tip.c_str()),
                                "resolve squash descendant");
                            Check(gg_repository_resolve(
                                      &current_destination, gg, destination.c_str()),
                                "resolve squash destination");
                            if (git_oid_equal(&current_tip, &current_destination))
                                continue;
                            gg_squash_options options = GG_SQUASH_OPTIONS_INIT;
                            const std::string current_tip_id = OidString(current_tip);
                            const std::string current_destination_id = OidString(current_destination);
                            options.source = current_tip_id.c_str();
                            options.destination = current_destination_id.c_str();
                            options.message = value.message_provided || !value.message.empty()
                                ? value.message.c_str() : nullptr;
                            Mutation mutation;
                            gg_operation_options operation = OperationOptions();
                            Check(gg_repository_squash_ex(&mutation.value, gg, &options, true, &operation),
                                "squash descendants");
                            InvalidateWorktreeStatus();
                            PublishSnapshot(false, true);
                        }
                    }
                    catch (...)
                    {
                        InvalidateWorktreeStatus();
                        PublishSnapshot(false, true);
                        throw;
                    }
                    return;
                }
                gg_squash_options options = GG_SQUASH_OPTIONS_INIT;
                options.source = value.source.c_str();
                options.destination = value.destination.c_str();
                options.message = value.message_provided || !value.message.empty()
                    ? value.message.c_str() : nullptr;
                Mutate("squash change", [&](auto* out, auto* operation) {
                    return gg_repository_squash_ex(out, gg, &options, value.entire_branch, operation);
                });
            },
            [&](const Abandon& value) {
                gg_abandon_options options = GG_ABANDON_OPTIONS_INIT;
                const StringArray revisions(value.revisions);
                options.revisions = revisions.Get();
                options.retain_branches = value.retain_branches;
                options.restore_descendants = value.restore_descendants;
                Mutate("abandon change", [&](auto* out, auto* operation) {
                    return gg_repository_abandon(out, gg, &options, operation);
                });
                try
                {
                    for (const RemoteBranchDelete& branch : value.remote_branches)
                        RemoveRemoteBranch(branch, false);
                }
                catch (const std::exception& error)
                {
                    PublishSnapshot();
                    throw std::runtime_error(std::string("Changes abandoned locally; remote branch deletion failed: ")
                        + error.what() + ". Local history can be undone; remote deletions cannot.");
                }
                if (!value.remote_branches.empty())
                    PublishSnapshot();
            },
            [&](const RemoteBranchDelete& value) { RemoveRemoteBranch(value, true); },
            [&](const AddRemote& value) {
                git_remote* raw_remote = nullptr;
                Check(git_remote_create(&raw_remote, git.get(), value.name.c_str(), value.url.c_str()), "add remote");
                std::unique_ptr<git_remote, decltype(&git_remote_free)> remote(raw_remote, git_remote_free);
                PublishSnapshot();
            },
            [&](const DeleteRemote& value) {
                Check(git_remote_delete(git.get(), value.name.c_str()), "delete remote");
                PublishSnapshot();
            },
            [&](const Restore& value) {
                gg_restore_options options = GG_RESTORE_OPTIONS_INIT;
                const StringArray filesets(value.filesets);
                options.filesets = filesets.Get();
                if (value.from.empty())
                    options.changes_in = value.into.empty() ? "@" : value.into.c_str();
                else
                {
                    options.from = value.from.c_str();
                    options.into = value.into.empty() ? "@" : value.into.c_str();
                }
                Mutate("restore files", [&](auto* out, auto* operation) {
                    return gg_repository_restore(out, gg, &options, operation);
                });
            },
            [&](const MoveFiles& value) {
                if (std::string_view(value.source).starts_with("working-tree:")
                    || std::string_view(value.destination).starts_with("working-tree:"))
                {
                    MoveWorkingTreeFile(value);
                    return;
                }
                gg_move_files_options options = GG_MOVE_FILES_OPTIONS_INIT;
                const StringArray filesets(value.filesets);
                options.source = value.source.c_str();
                options.destination = value.destination.c_str();
                options.filesets = filesets.Get();
                Mutate("move files", [&](auto* out, auto* operation) {
                    return gg_repository_move_files(out, gg, &options, operation);
                });
            },
            [&](const MoveDiffLines& value) { MoveDiffSelection(value); },
            [&](const RevertDiffLines& value) {
                MoveDiffSelection(MoveDiffLines{value.source, {}, value.path, value.lines}, true);
            },
            [&](const SimplifyParents& value) {
                gg_simplify_parents_options options = GG_SIMPLIFY_PARENTS_OPTIONS_INIT;
                const StringArray revisions(value.revisions);
                options.revisions = revisions.Get();
                Mutate("simplify parents", [&](auto* out, auto* operation) {
                    return gg_repository_simplify_parents(out, gg, &options, operation);
                });
            },
            [&](const Branch& value) {
                gg_branch_options options = GG_BRANCH_OPTIONS_INIT;
                std::vector<std::string> names = value.names;
                if (!value.rename_to.empty())
                    names.push_back(value.rename_to);
                const StringArray array(names);
                options.action = value.action;
                options.names = array.Get();
                options.revision = value.revision.c_str();
                options.allow_backwards = value.allow_backwards;
                Mutate("update branch", [&](auto* out, auto* operation) {
                    return gg_repository_branch(out, gg, &options, operation);
                });
            },
            [&](const Tag& value) {
                gg_tag_options options = GG_TAG_OPTIONS_INIT;
                const StringArray names(value.names);
                options.action = value.action;
                options.names = names.Get();
                options.revision = value.revision.c_str();
                options.allow_move = value.allow_move;
                Mutate("update tag", [&](auto* out, auto* operation) {
                    return gg_repository_tag(out, gg, &options, operation);
                });
            },
            [&](const Undo&) {
                Mutate("undo", [&](auto* out, auto* operation) {
                    return gg_repository_undo(out, gg, operation);
                });
            },
            [&](const Redo&) {
                Mutate("redo", [&](auto* out, auto* operation) {
                    return gg_repository_redo(out, gg, operation);
                });
            },
            [&](const RestoreOperation& value) {
                Mutate("restore operation", [&](auto* out, auto* operation) {
                    return gg_repository_restore_operation(out, gg, value.operation.c_str(), GG_RESTORE_ALL, operation);
                });
            },
            [&](const WorkspaceAdd& value) {
                gg_workspace_add_options options = GG_WORKSPACE_ADD_OPTIONS_INIT;
                options.destination = value.destination.c_str();
                options.name = value.name.c_str();
                options.revision = value.revision.c_str();
                options.message = value.message.c_str();
                options.sparse_patterns = "full";
                Mutate("add workspace", [&](auto* out, auto* operation) {
                    return gg_repository_workspace_add(out, gg, &options, operation);
                });
            },
            [&](const WorkspaceForget& value) {
                const StringArray names(value.names);
                Mutate("forget workspace", [&](auto* out, auto* operation) {
                    return gg_repository_workspace_forget(out, gg, names.Get(), operation);
                });
            },
            [&](const WorkspaceRename& value) {
                Mutate("rename workspace", [&](auto* out, auto* operation) {
                    return value.old_name.empty()
                        ? gg_repository_workspace_rename(out, gg, value.name.c_str(), operation)
                        : gg_repository_workspace_rename_named(
                            out, gg, value.old_name.c_str(), value.name.c_str(), operation);
                });
            },
            [&](const WorkspaceRemove& value) {
                const auto remove = [&] {
                    Mutation mutation;
                    gg_operation_options operation = OperationOptions();
                    Check(gg_repository_workspace_remove(
                              &mutation.value, gg, value.name.c_str(), &operation),
                        "remove workspace");
                };
                if (!value.current)
                {
                    remove();
                    PublishSnapshot();
                    Post(WorkspaceRemoved{value.root, false});
                    return;
                }
                Close();
                try
                {
                    OpenPath(value.controller);
                    remove();
                    Close();
                    {
                        std::lock_guard lock(queue_mutex);
                        std::erase_if(commands, [](const QueuedCommand& queued) {
                            const bool remove = std::holds_alternative<Refresh>(queued.command)
                                || std::holds_alternative<RebuildHistory>(queued.command)
                                || std::holds_alternative<ExpandHistoryRegion>(queued.command)
                                || std::holds_alternative<LoadDiff>(queued.command)
                                || std::holds_alternative<LoadFileContent>(queued.command)
                                || std::holds_alternative<LoadBlame>(queued.command);
                            if (remove)
                                TraceTaskStale(queued.task, CommandName(queued.command), 0,
                                    queued.repository_generation, queued.queued);
                            return remove;
                        });
                    }
                    Post(WorkspaceRemoved{value.root, true});
                }
                catch (...)
                {
                    Close();
                    if (std::filesystem::is_directory(value.root)) OpenPath(value.root);
                    throw;
                }
            },
            [&](const TrackPaths& value) {
                const StringArray paths(value.filesets);
                Mutate("track paths", [&](auto* out, auto* operation) {
                    return gg_repository_track_paths(out, gg, paths.Get(), value.include_ignored, operation);
                });
            },
            [&](const UntrackPaths& value) {
                const StringArray paths(value.filesets);
                Mutate("untrack paths", [&](auto* out, auto* operation) {
                    return gg_repository_untrack_paths(out, gg, paths.Get(), operation);
                });
            },
            [&](const ChmodPaths& value) {
                const StringArray paths(value.filesets);
                Mutate("change executable bit", [&](auto* out, auto* operation) {
                    return gg_repository_chmod(out, gg, paths.Get(), value.executable, operation);
                });
            },
            [&](const auto&) { throw std::runtime_error("command is not a mutation"); }},
        command);
}

std::string CommandName(const Command& command)
{
    return std::visit(
        Overloaded{[](const OpenRepository&) { return "open"; }, [](const CloseRepository&) { return "close"; },
            [](const InitRepository&) { return "init"; },
            [](const CloneRepository&) { return "clone"; }, [](const Refresh&) { return "refresh"; },
            [](const RebuildHistory&) { return "rebuild history"; },
            [](const ExpandHistoryRegion&) { return "expand history"; },
            [](const Fetch& value) { return value.tracked_only ? "pull" : "fetch"; },
            [](const Push&) { return "push"; },
            [](const AddRemote&) { return "add remote"; },
            [](const DeleteRemote&) { return "delete remote"; },
            [](const LoadDiff&) { return "diff"; }, [](const LoadFileContent&) { return "load file"; },
            [](const LoadBlame&) { return "blame"; },
            [](const ApplyPatch&) { return "apply patch"; },
            [](const ResolveConflict&) { return "resolve conflict"; },
            [](const RevertFile&) { return "revert file"; },
            [](const DeleteFile&) { return "delete file"; },
            [](const RevertFiles&) { return "revert files"; },
            [](const DeleteFiles&) { return "delete files"; },
            [](const NewChange&) { return "new"; },
            [](const Describe&) { return "describe"; }, [](const Metaedit&) { return "metaedit"; },
            [](const Edit&) { return "edit"; }, [](const MoveChange&) { return "move"; },
            [](const Commit&) { return "commit"; },
            [](const Amend&) { return "amend"; },
            [](const Rebase&) { return "rebase"; }, [](const Duplicate&) { return "duplicate"; },
            [](const Reorder&) { return "reorder"; },
            [](const Split&) { return "split"; }, [](const Squash&) { return "squash"; },
            [](const Abandon&) { return "abandon"; },
            [](const RemoteBranchDelete&) { return "delete remote branch"; },
            [](const Restore&) { return "restore"; },
            [](const MoveFiles&) { return "move files"; },
            [](const MoveDiffLines&) { return "move diff lines"; },
            [](const RevertDiffLines&) { return "revert diff lines"; },
            [](const SimplifyParents&) { return "simplify parents"; }, [](const Branch&) { return "branch"; },
            [](const Tag&) { return "tag"; }, [](const Undo&) { return "undo"; }, [](const Redo&) { return "redo"; },
            [](const RestoreOperation&) { return "restore operation"; },
            [](const WorkspaceAdd&) { return "add workspace"; },
            [](const WorkspaceForget&) { return "forget workspace"; },
            [](const WorkspaceRename&) { return "rename workspace"; },
            [](const WorkspaceRemove&) { return "remove workspace"; },
            [](const TrackPaths&) { return "track paths"; }, [](const UntrackPaths&) { return "untrack paths"; },
            [](const ChmodPaths&) { return "chmod"; }},
        command);
}

} // namespace Ggui

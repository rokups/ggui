// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <memory>
#include <stdexcept>
#include <string>
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
                options.message = value.message.c_str();
                options.author = value.author.c_str();
                options.message_provided = !value.message.empty();
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
                options.edit = value.edit;
                options.conflict = value.conflict;
                Mutate("move working copy", [&](auto* out, auto* operation) {
                    return gg_repository_move(out, gg, &options, operation);
                });
            },
            [&](const Commit& value) {
                gg_commit_options options = GG_COMMIT_OPTIONS_INIT;
                const StringArray filesets(value.filesets);
                options.filesets = filesets.Get();
                options.message = value.message.c_str();
                options.message_provided = 1;
                Mutate("commit change", [&](auto* out, auto* operation) {
                    return gg_repository_commit(out, gg, &options, operation);
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
                gg_squash_options options = GG_SQUASH_OPTIONS_INIT;
                options.source = value.source.c_str();
                options.destination = value.destination.c_str();
                options.message = value.message.c_str();
                Mutate("squash change", [&](auto* out, auto* operation) {
                    return gg_repository_squash(out, gg, &options, operation);
                });
            },
            [&](const Abandon& value) {
                for (const RemoteBookmarkDelete& bookmark : value.remote_bookmarks)
                    RemoveRemoteBookmark(bookmark, false);
                gg_abandon_options options = GG_ABANDON_OPTIONS_INIT;
                const StringArray revisions(value.revisions);
                options.revisions = revisions.Get();
                options.retain_bookmarks = value.retain_bookmarks;
                options.restore_descendants = value.restore_descendants;
                Mutate("abandon change", [&](auto* out, auto* operation) {
                    return gg_repository_abandon(out, gg, &options, operation);
                });
            },
            [&](const RemoteBookmarkDelete& value) { RemoveRemoteBookmark(value, true); },
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
                options.from = value.from.c_str();
                options.into = value.into.c_str();
                Mutate("restore files", [&](auto* out, auto* operation) {
                    return gg_repository_restore(out, gg, &options, operation);
                });
            },
            [&](const MoveFiles& value) {
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
            [&](const Bookmark& value) {
                gg_bookmark_options options = GG_BOOKMARK_OPTIONS_INIT;
                std::vector<std::string> names = value.names;
                if (!value.rename_to.empty())
                    names.push_back(value.rename_to);
                const StringArray array(names);
                options.action = value.action;
                options.names = array.Get();
                options.revision = value.revision.c_str();
                options.allow_backwards = value.allow_backwards;
                Mutate("update bookmark", [&](auto* out, auto* operation) {
                    return gg_repository_bookmark(out, gg, &options, operation);
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
                Mutation mutation;
                gg_operation_options operation = OperationOptions();
                Check(gg_repository_undo(&mutation.value, gg, &operation), "undo");
                PublishSnapshot();
            },
            [&](const Redo&) {
                Mutation mutation;
                gg_operation_options operation = OperationOptions();
                Check(gg_repository_redo(&mutation.value, gg, &operation), "redo");
                PublishSnapshot();
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
                    return gg_repository_workspace_rename(out, gg, value.name.c_str(), operation);
                });
            },
            [&](const TrackPaths& value) {
                const StringArray paths(value.filesets);
                Mutate("track paths", [&](auto* out, auto* operation) {
                    return gg_repository_track_paths(out, gg, paths.Get(), value.include_ignored, operation);
                }, true);
            },
            [&](const UntrackPaths& value) {
                const StringArray paths(value.filesets);
                Mutate("untrack paths", [&](auto* out, auto* operation) {
                    return gg_repository_untrack_paths(out, gg, paths.Get(), operation);
                }, true);
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

std::string RepositoryEngine::Impl::CommandName(const Command& command)
{
    return std::visit(
        Overloaded{[](const OpenRepository&) { return "open"; }, [](const CloseRepository&) { return "close"; },
            [](const InitRepository&) { return "init"; },
            [](const CloneRepository&) { return "clone"; }, [](const Refresh&) { return "refresh"; },
            [](const Fetch& value) { return value.tracked_only ? "pull" : "fetch"; },
            [](const Push&) { return "push"; },
            [](const AddRemote&) { return "add remote"; },
            [](const DeleteRemote&) { return "delete remote"; },
            [](const LoadDiff&) { return "diff"; }, [](const ApplyPatch&) { return "apply patch"; },
            [](const RevertFile&) { return "revert file"; },
            [](const DeleteFile&) { return "delete file"; },
            [](const NewChange&) { return "new"; },
            [](const Describe&) { return "describe"; }, [](const Metaedit&) { return "metaedit"; },
            [](const Edit&) { return "edit"; }, [](const MoveChange&) { return "move"; },
            [](const Commit&) { return "commit"; },
            [](const Rebase&) { return "rebase"; }, [](const Duplicate&) { return "duplicate"; },
            [](const Reorder&) { return "reorder"; },
            [](const Split&) { return "split"; }, [](const Squash&) { return "squash"; },
            [](const Abandon&) { return "abandon"; },
            [](const RemoteBookmarkDelete&) { return "delete remote bookmark"; },
            [](const Restore&) { return "restore"; },
            [](const MoveFiles&) { return "move files"; },
            [](const MoveDiffLines&) { return "move diff lines"; },
            [](const RevertDiffLines&) { return "revert diff lines"; },
            [](const SimplifyParents&) { return "simplify parents"; }, [](const Bookmark&) { return "bookmark"; },
            [](const Tag&) { return "tag"; }, [](const Undo&) { return "undo"; }, [](const Redo&) { return "redo"; },
            [](const RestoreOperation&) { return "restore operation"; },
            [](const WorkspaceAdd&) { return "add workspace"; },
            [](const WorkspaceForget&) { return "forget workspace"; },
            [](const WorkspaceRename&) { return "rename workspace"; },
            [](const TrackPaths&) { return "track paths"; }, [](const UntrackPaths&) { return "untrack paths"; },
            [](const ChmodPaths&) { return "chmod"; }},
        command);
}

} // namespace Ggui

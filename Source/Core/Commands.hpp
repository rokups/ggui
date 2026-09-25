// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "Model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace Ggui
{

struct OpenRepository { std::string path; };
struct CloseRepository {};
struct InitRepository { std::string path; };
struct CloneRepository { std::string url; std::string path; };
struct Refresh
{
    bool inspect_working_tree = true;
    // Empty means a complete status read. Watcher-originated refreshes carry
    // coalesced repository-relative paths. Refreshes never create commits.
    std::vector<std::string> paths;
    // Explicit refreshes participate in the foreground operation lifecycle;
    // watcher refreshes remain unobtrusive background reconciliation.
    bool foreground = false;
};
struct RebuildHistory { HistoryQuery query; };
struct ExpandHistoryRegion
{
    std::string id;
    // Merge history is toggleable; ordinary collapsed regions only expand.
    bool merge_history = false;
};
struct Fetch { std::string remote; bool tracked_only = false; };
struct Push { std::string branch; std::string remote; bool force = false; };
struct AddRemote { std::string name; std::string url; };
struct DeleteRemote { std::string name; };

struct LoadDiff
{
    LoadDiff() = default;
    LoadDiff(std::string revision, std::string path, bool fallback_to_first = false, DiffOptions options = {},
        std::string compare_to = {}, bool file_comparison = false)
        : revision(std::move(revision))
        , path(std::move(path))
        , fallback_to_first(fallback_to_first)
        , options(options)
        , compare_to(std::move(compare_to))
        , file_comparison(file_comparison)
    {
    }

    std::string revision;
    std::string path;
    bool fallback_to_first = false;
    DiffOptions options;
    std::string compare_to;
    bool file_comparison = false;
};
struct LoadFileContent { std::string revision; std::string path; };
struct LoadBlame { std::string revision; std::string path; };

struct ApplyPatch { std::string text; std::string path; };
struct ResolveConflict
{
    std::string revision;
    std::string path;
    std::string contents;
    bool present = true;
};
struct RevertFile
{
    std::string source;
    std::string old_path;
    std::string path;
    std::vector<DiffLine> lines;
};
struct DeleteFile { std::string path; };
// Batches revert or delete many files in one working-tree update.
struct RevertFiles
{
    std::string source;
    std::vector<StatusEntry> files;
};
struct DeleteFiles { std::vector<std::string> paths; };
struct NewChange
{
    std::string message;
    std::vector<std::string> parents;
    std::vector<std::string> insert_before;
    std::vector<std::string> insert_after;
    bool no_edit = false;
    // Leave the current branch in place; HEAD detaches at the new change.
    bool detach = false;
};
struct Describe { std::string revision; std::string message; };
struct Metaedit { std::string revision; std::optional<std::string> message; std::string author; };
struct Edit { std::string revision; };
struct MoveChange
{
    gg_move_direction direction = GG_MOVE_NEXT;
    std::uint64_t offset = 1;
    bool conflict = false;
};
struct Commit { std::string message; };
struct Amend { std::string revision; std::string message; };
struct Rebase { std::string source; std::string destination; bool entire_branch = false; };
struct Duplicate { std::string revision; bool descendants = false; };
struct Reorder
{
    std::string source;
    std::string target;
    gg_reorder_placement placement = GG_REORDER_BEFORE;
    bool copy = false;
};
struct Split { std::string revision; std::string message; std::vector<std::string> filesets; };
struct Squash
{
    std::string source;
    std::string destination;
    std::string message;
    bool entire_branch = false;
    bool descendants = false;
    bool message_provided = false;
};
struct RemoteBranchDelete { std::string branch; std::string remote; };
struct Abandon
{
    std::vector<std::string> revisions;
    bool retain_branches = false;
    bool restore_descendants = false;
    std::vector<RemoteBranchDelete> remote_branches;
};
struct Restore { std::string from; std::string into; std::vector<std::string> filesets; };
struct MoveFiles { std::string source; std::string destination; std::vector<std::string> filesets; };
struct MoveDiffLines
{
    std::string source;
    std::string destination;
    std::string path;
    std::vector<DiffLine> lines;
};
struct RevertDiffLines { std::string source; std::string path; std::vector<DiffLine> lines; };
struct SimplifyParents { std::vector<std::string> revisions; };
struct Branch
{
    gg_branch_action action = GG_BRANCH_CREATE;
    std::vector<std::string> names;
    std::string revision;
    std::string rename_to;
    bool allow_backwards = false;
};
struct Tag
{
    gg_tag_action action = GG_TAG_SET;
    std::vector<std::string> names;
    std::string revision;
    bool allow_move = false;
};
struct Undo {};
struct Redo {};
struct RestoreOperation { std::string operation; };
struct WorkspaceAdd
{
    std::string destination;
    std::string name;
    std::string revision;
    std::string message;
};
struct WorkspaceForget { std::vector<std::string> names; };
struct WorkspaceRename
{
    explicit WorkspaceRename(std::string name) : name(std::move(name)) {}
    WorkspaceRename(std::string old_name, std::string name)
        : old_name(std::move(old_name)), name(std::move(name)) {}
    std::string old_name;
    std::string name;
};
struct WorkspaceRemove
{
    std::string name;
    std::string root;
    std::string controller;
    bool current = false;
};
struct TrackPaths { std::vector<std::string> filesets; bool include_ignored = false; };
struct UntrackPaths { std::vector<std::string> filesets; };
struct ChmodPaths { std::vector<std::string> filesets; bool executable = false; };

using Command = std::variant<OpenRepository, CloseRepository, InitRepository, CloneRepository, Refresh, RebuildHistory,
    ExpandHistoryRegion, Fetch, Push,
    AddRemote, DeleteRemote, LoadDiff, LoadFileContent, LoadBlame, ApplyPatch, ResolveConflict, RevertFile, DeleteFile, RevertFiles, DeleteFiles, NewChange,
    Describe, Metaedit, Edit, MoveChange, Commit, Amend, Rebase, Duplicate, Reorder, Split, Squash, Abandon, RemoteBranchDelete, Restore,
    MoveFiles, MoveDiffLines, RevertDiffLines, SimplifyParents, Branch, Tag, Undo, Redo, RestoreOperation,
    WorkspaceAdd, WorkspaceForget, WorkspaceRename, WorkspaceRemove, TrackPaths, UntrackPaths, ChmodPaths>;

std::string CommandName(const Command& command);

} // namespace Ggui

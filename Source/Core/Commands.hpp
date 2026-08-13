// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "Model.hpp"

#include <cstdint>
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
struct Refresh { bool snapshot_working_copy = true; };
struct Fetch { std::string remote; bool tracked_only = false; };
struct Push { std::string bookmark; std::string remote; bool force = false; };
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

struct ApplyPatch { std::string text; std::string path; };
struct RevertFile
{
    std::string source;
    std::string old_path;
    std::string path;
    std::vector<DiffLine> lines;
};
struct DeleteFile { std::string path; };
struct NewChange
{
    std::string message;
    std::vector<std::string> parents;
    std::vector<std::string> insert_before;
    std::vector<std::string> insert_after;
    bool no_edit = false;
};
struct Describe { std::string revision; std::string message; };
struct Metaedit { std::string revision; std::string message; std::string author; };
struct Edit { std::string revision; };
struct MoveChange
{
    gg_move_direction direction = GG_MOVE_NEXT;
    std::uint64_t offset = 1;
    bool edit = false;
    bool conflict = false;
};
struct Commit { std::string message; std::vector<std::string> filesets; };
struct Rebase { std::string source; std::string destination; bool entire_branch = false; };
struct Reorder
{
    std::string source;
    std::string target;
    gg_reorder_placement placement = GG_REORDER_BEFORE;
};
struct Split { std::string revision; std::string message; std::vector<std::string> filesets; };
struct Squash { std::string source; std::string destination; std::string message; };
struct RemoteBookmarkDelete { std::string bookmark; std::string remote; };
struct Abandon
{
    std::vector<std::string> revisions;
    bool retain_bookmarks = false;
    bool restore_descendants = false;
    std::vector<RemoteBookmarkDelete> remote_bookmarks;
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
struct Bookmark
{
    gg_bookmark_action action = GG_BOOKMARK_CREATE;
    std::vector<std::string> names;
    std::string revision;
    std::string rename_to;
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
struct WorkspaceRename { std::string name; };
struct TrackPaths { std::vector<std::string> filesets; bool include_ignored = false; };
struct UntrackPaths { std::vector<std::string> filesets; };
struct ChmodPaths { std::vector<std::string> filesets; bool executable = false; };

using Command = std::variant<OpenRepository, CloseRepository, InitRepository, CloneRepository, Refresh, Fetch, Push,
    AddRemote, DeleteRemote, LoadDiff, ApplyPatch, RevertFile, DeleteFile, NewChange, Describe, Metaedit, Edit, MoveChange,
    Commit, Rebase, Reorder, Split, Squash, Abandon, RemoteBookmarkDelete, Restore, MoveFiles, MoveDiffLines,
    RevertDiffLines, SimplifyParents, Bookmark, Tag, Undo, Redo, RestoreOperation, WorkspaceAdd, WorkspaceForget,
    WorkspaceRename, TrackPaths, UntrackPaths, ChmodPaths>;

} // namespace Ggui

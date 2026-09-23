// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <gg/gg.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Ggui
{

struct Revision
{
    std::string oid;
    std::vector<std::string> parents;
    std::vector<std::string> aliases;
    std::string description;
    std::string author;
    std::int64_t timestamp = 0;
    bool working_copy = false;
    bool conflicted = false;
    bool pushed = false;
    bool empty = false;
    std::string author_email{};
};

enum class HistoryItemKind { Commit, CollapsedRegion, WorkingTree };

struct HistoryItem
{
    // Stable within a repository generation. Commit item IDs are their OIDs;
    // collapsed IDs identify the ancestry interval they summarize. Working
    // tree IDs are virtual and must never be resolved as revisions.
    std::string id;
    HistoryItemKind kind = HistoryItemKind::Commit;
    Revision revision;
    // Item IDs, never raw OIDs outside this view.
    std::vector<std::string> parents;
    bool search_match = false;
};

inline HistoryItem MakeWorkingTreeHistoryItem(std::uint64_t repository_generation, std::string parent)
{
    HistoryItem item;
    item.id = "working-tree:" + std::to_string(repository_generation);
    item.kind = HistoryItemKind::WorkingTree;
    if (!parent.empty())
        item.parents.push_back(std::move(parent));
    return item;
}

struct HistoryView
{
    std::uint64_t repository_generation = 0;
    std::uint64_t request = 0;
    bool skeleton = false;
    std::string search;
    std::vector<HistoryItem> items;
};

struct HistoryQuery
{
    std::vector<std::string> bookmarks;
    std::vector<std::string> tags;
    std::vector<std::string> remotes;
    std::string search;
    std::uint64_t repository_generation = 0;
};

struct NamedRef
{
    std::string name;
    std::string remote;
    std::string target;
    gg_named_ref_kind kind = GG_NAMED_REF_LOCAL_BOOKMARK;
    bool tracked = false;
    bool conflicted = false;
    std::size_t local_commits = 0;
    std::size_t remote_commits = 0;
    bool desync_known = false;
};

struct StatusEntry
{
    std::string old_path;
    std::string path;
    git_delta_t status = GIT_DELTA_UNMODIFIED;
    bool conflicted = false;
    bool operator==(const StatusEntry&) const = default;
};

struct Operation
{
    std::string oid;
    std::string description;
    std::int64_t timestamp = 0;
};

// A single entry in the Git HEAD reflog. Reflog entries are intentionally
// kept separate from gg operations: the former records every physical HEAD
// move (including external Git commands), while the latter is the undoable gg
// operation journal.
struct ReflogEntry
{
    std::size_t index = 0;
    std::string old_hash;
    std::string new_hash;
    bool old_commit_available = false;
    bool new_commit_available = false;
    std::string author;
    std::string author_email;
    std::int64_t timestamp = 0;
    std::string message;
};

struct Workspace
{
    std::string name;
    std::string root;
    std::string working_copy;
    bool stale = false;
    bool managed = true;
    bool current = false;
    bool primary = false;
};

struct Remote
{
    std::string name;
    std::string fetch_url;
    std::string push_url;
};

struct Conflict
{
    std::string path;
    std::size_t removes = 0;
    std::size_t adds = 0;
};

struct BlameLine
{
    std::size_t line = 0;
    std::size_t original_line = 0;
    std::string revision;
    std::string author;
    std::string author_email;
    std::int64_t timestamp = 0;
    std::string summary;
    std::string contents;
    bool boundary = false;

    // The source hunk from which libgit2 traced this line. These values are
    // normally the same as the final commit, but differ when copy tracking
    // follows a line across a file or commit boundary.
    std::size_t previous_line = 0;
    std::string previous_revision;
    std::string previous_path;
    std::string previous_author;
    std::string previous_author_email;
    std::int64_t previous_timestamp = 0;
    std::string previous_summary;

    // The first-parent snapshot to use for the per-hunk "blame before this
    // change" action. This is deliberately separate from the origin fields
    // above: orig_commit_id describes where libgit2 found a copied/moved line,
    // whereas this points at the commit immediately before the change that
    // owns the line.
    std::string blame_before_revision;
    std::string blame_before_path;
};

struct BlameResult
{
    std::uint64_t generation = 0;
    std::string revision;
    std::string path;
    std::vector<BlameLine> lines;
    // Metadata for the file snapshot itself. History is intentionally
    // bounded/collapsed in the UI, so blame keeps enough commit information
    // to continue walking parents even when the viewed revision is not
    // currently materialized in the history panel.
    Revision viewed_revision;
};

struct RepoSnapshot
{
    enum class WorktreeState { Unscanned, Scanning, Ready, Stale };
    std::uint64_t generation = 0;
    std::uint64_t repository_generation = 0;
    std::string root;
    // Synthetic UI snapshots default to a worktree; repository snapshots set
    // this from git_repository_workdir() so views can guard filesystem actions.
    bool has_worktree = true;
    std::string working_copy;
    std::string head;
    // Compatibility storage for synthetic snapshots injected by UI tests.
    // RepositoryEngine never publishes history here; HistoryReady owns it.
    std::vector<Revision> revisions;
    std::vector<NamedRef> refs;
    std::vector<StatusEntry> status;
    std::vector<Operation> operations;
    std::vector<Workspace> workspaces;
    std::vector<Remote> remotes;
    std::vector<Conflict> conflicts;
    WorktreeState worktree_state = WorktreeState::Unscanned;
    bool can_undo = false;
    bool can_redo = false;
    std::vector<ReflogEntry> reflog;
};

enum class DiffWhitespaceMode
{
    Normal,
    IgnoreWhitespace,
    IgnoreAllWhitespace,
};

enum class DiffLineKind
{
    Context,
    Addition,
    Deletion,
};

struct DiffLine
{
    DiffLineKind kind = DiffLineKind::Context;
    int old_line = -1;
    int new_line = -1;
    int hunk = -1;
};

struct DiffOptions
{
    DiffWhitespaceMode whitespace_mode = DiffWhitespaceMode::Normal;
    int context_lines = 3;
};

struct DiffResult
{
    DiffResult() = default;
    DiffResult(std::uint64_t generation, std::string revision, std::string path, std::string before, std::string after,
        bool binary, std::vector<StatusEntry> files, std::string compare_to = {}, bool file_comparison = false)
        : generation(generation)
        , revision(std::move(revision))
        , compare_to(std::move(compare_to))
        , file_comparison(file_comparison)
        , path(std::move(path))
        , before(std::move(before))
        , after(std::move(after))
        , binary(binary)
        , files(std::move(files))
    {
        for (const StatusEntry& file : this->files)
            if (file.path == this->path || file.old_path == this->path)
            {
                selected_status = file.status;
                break;
            }
    }

    std::uint64_t generation = 0;
    std::string revision;
    std::string compare_to;
    bool file_comparison = false;
    std::string path;
    std::string before;
    std::string after;
    std::string full_before;
    std::string full_after;
    bool binary = false;
    git_delta_t selected_status = GIT_DELTA_UNMODIFIED;
    std::vector<StatusEntry> files;
    std::vector<DiffLine> lines{};
    std::string patch;
    std::string old_oid;
    std::string new_oid;
    unsigned int old_mode = 0;
    unsigned int new_mode = 0;
    DiffOptions options;
};

} // namespace Ggui

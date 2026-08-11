// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <gg/gg.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace Ggui
{

struct Revision
{
    std::string oid;
    std::vector<std::string> parents;
    std::string change_id;
    std::string description;
    std::string author;
    std::int64_t timestamp = 0;
    bool working_copy = false;
    bool conflicted = false;
    bool pushed = false;
    bool empty = false;
    std::string author_email{};
};

struct NamedRef
{
    std::string name;
    std::string remote;
    std::string target;
    gg_named_ref_kind kind = GG_NAMED_REF_LOCAL_BOOKMARK;
    bool tracked = false;
    bool conflicted = false;
};

struct StatusEntry
{
    std::string old_path;
    std::string path;
    git_delta_t status = GIT_DELTA_UNMODIFIED;
    bool conflicted = false;
};

struct Operation
{
    std::string oid;
    std::string description;
    std::int64_t timestamp = 0;
};

struct Workspace
{
    std::string name;
    std::string root;
    std::string working_copy;
    bool stale = false;
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

struct RepoSnapshot
{
    std::uint64_t generation = 0;
    std::string root;
    std::string working_copy;
    std::vector<Revision> revisions;
    std::vector<NamedRef> refs;
    std::vector<StatusEntry> status;
    std::vector<Operation> operations;
    std::vector<Workspace> workspaces;
    std::vector<Remote> remotes;
    std::vector<Conflict> conflicts;
    bool can_undo = false;
    bool can_redo = false;
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

struct OpenRepository
{
    std::string path;
};
struct CloseRepository
{
};
struct InitRepository
{
    std::string path;
};
struct CloneRepository
{
    std::string url;
    std::string path;
};
struct Refresh
{
    bool snapshot_working_copy = true;
};
struct Fetch
{
    std::string remote;
    bool tracked_only = false;
};
struct Push
{
    std::string bookmark;
    std::string remote;
};
struct AddRemote
{
    std::string name;
    std::string url;
};
struct DeleteRemote
{
    std::string name;
};
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
struct ApplyPatch
{
    std::string text;
    std::string path;
};
struct RevertFile
{
    std::string source;
    std::string old_path;
    std::string path;
    std::vector<DiffLine> lines;
};
struct DeleteFile
{
    std::string path;
};
struct NewChange
{
    std::string message;
    std::vector<std::string> parents;
    std::vector<std::string> insert_before;
    std::vector<std::string> insert_after;
    bool no_edit = false;
};
struct Describe
{
    std::string revision;
    std::string message;
};
struct Metaedit
{
    std::string revision;
    std::string message;
    std::string author;
};
struct Edit
{
    std::string revision;
};
struct MoveChange
{
    gg_move_direction direction = GG_MOVE_NEXT;
    std::uint64_t offset = 1;
    bool edit = false;
    bool conflict = false;
};
struct Commit
{
    std::string message;
    std::vector<std::string> filesets;
};
struct Rebase
{
    std::string source;
    std::string destination;
    bool entire_branch = false;
};
struct Reorder
{
    std::string source;
    std::string target;
    gg_reorder_placement placement = GG_REORDER_BEFORE;
};
struct Split
{
    std::string revision;
    std::string message;
    std::vector<std::string> filesets;
};
struct Squash
{
    std::string source;
    std::string destination;
    std::string message;
};
struct RemoteBookmarkDelete
{
    std::string bookmark;
    std::string remote;
};
struct Abandon
{
    std::vector<std::string> revisions;
    bool retain_bookmarks = false;
    bool restore_descendants = false;
    std::vector<RemoteBookmarkDelete> remote_bookmarks;
};
struct Restore
{
    std::string from;
    std::string into;
    std::vector<std::string> filesets;
};
struct MoveFiles
{
    std::string source;
    std::string destination;
    std::vector<std::string> filesets;
};
struct MoveDiffLines
{
    std::string source;
    std::string destination;
    std::string path;
    std::vector<DiffLine> lines;
};
struct RevertDiffLines
{
    std::string source;
    std::string path;
    std::vector<DiffLine> lines;
};
struct SimplifyParents
{
    std::vector<std::string> revisions;
};
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
struct Undo
{
};
struct Redo
{
};
struct RestoreOperation
{
    std::string operation;
};
struct WorkspaceAdd
{
    std::string destination;
    std::string name;
    std::string revision;
    std::string message;
};
struct WorkspaceForget
{
    std::vector<std::string> names;
};
struct WorkspaceRename
{
    std::string name;
};
struct TrackPaths
{
    std::vector<std::string> filesets;
    bool include_ignored = false;
};
struct UntrackPaths
{
    std::vector<std::string> filesets;
};
struct ChmodPaths
{
    std::vector<std::string> filesets;
    bool executable = false;
};

using Command = std::variant<OpenRepository, CloseRepository, InitRepository, CloneRepository, Refresh, Fetch, Push,
    AddRemote, DeleteRemote, LoadDiff, ApplyPatch, RevertFile, DeleteFile, NewChange, Describe, Metaedit, Edit, MoveChange,
    Commit, Rebase, Reorder, Split, Squash, Abandon, RemoteBookmarkDelete, Restore, MoveFiles, MoveDiffLines,
    RevertDiffLines, SimplifyParents, Bookmark, Tag, Undo, Redo, RestoreOperation, WorkspaceAdd, WorkspaceForget,
    WorkspaceRename, TrackPaths, UntrackPaths, ChmodPaths>;

struct SnapshotReady
{
    std::shared_ptr<const RepoSnapshot> snapshot;
};
struct DiffReady
{
    DiffResult diff;
};
struct OperationStarted
{
    std::string name;
};
struct OperationProgress
{
    std::string phase;
    std::size_t completed = 0;
    std::size_t total = 0;
};
struct OperationFinished
{
    std::string name;
};
struct ErrorEvent
{
    std::string operation;
    std::string message;
};
struct CredentialRequest
{
    std::string url;
    std::string username;
    unsigned int allowed_types = 0;
};
using Event = std::variant<SnapshotReady, DiffReady, OperationStarted, OperationProgress, OperationFinished, ErrorEvent,
    CredentialRequest>;

struct CredentialResponse
{
    enum class Method
    {
        UserPass,
        SshAgent,
        SshKey,
    };
    Method method = Method::UserPass;
    std::string username;
    std::string secret;
    std::string private_key;
    std::string public_key;
};

class RepositoryEngine
{
public:
    RepositoryEngine();
    ~RepositoryEngine();
    RepositoryEngine(const RepositoryEngine&) = delete;
    RepositoryEngine& operator=(const RepositoryEngine&) = delete;

    void Enqueue(Command command);
    std::vector<Event> PollEvents();
    void Cancel();
    void SubmitCredential(CredentialResponse response);
    void CancelCredential();
#ifdef GGUI_TESTING
    void ResetCredentialStateForTest(bool ssh_agent_attempted = false, int credential_attempts = 0);
    int AcquireCredentialForTest(
        git_credential** out, const char* url, const char* username, unsigned int allowed_types);
    int TransferProgressForTest(const git_indexer_progress& progress);
    void DispatchMutationForTest(const Command& command);
    void SetCommandsSuppressedForTest(bool suppressed);
    static std::vector<Conflict> ConvertConflictsForTest();
    static void FinishCloneForTest(const std::filesystem::path& temporary, const std::filesystem::path& destination);
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

std::string ShortId(const std::string& value, std::size_t length = 8);
std::vector<std::size_t> UniquePrefixLengths(const std::vector<std::string>& values, std::size_t minimum = 1);
void MarkPushedRevisions(
    std::vector<Revision>& revisions, const std::vector<NamedRef>& refs, git_repository* repository = nullptr);
std::string FirstLine(const std::string& value);

} // namespace Ggui

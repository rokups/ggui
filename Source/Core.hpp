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
    std::vector<Conflict> conflicts;
};

struct DiffResult
{
    std::uint64_t generation = 0;
    std::string revision;
    std::string path;
    std::string before;
    std::string after;
    std::string patch;
    bool binary = false;
    std::vector<StatusEntry> files;
};

struct OpenRepository
{
    std::string path;
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
struct LoadDiff
{
    std::string revision;
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
struct Abandon
{
    std::vector<std::string> revisions;
    bool retain_bookmarks = false;
    bool restore_descendants = false;
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

using Command = std::variant<OpenRepository, InitRepository, CloneRepository, Refresh, LoadDiff, NewChange, Describe,
    Metaedit, Edit, MoveChange, Commit, Rebase, Reorder, Split, Squash, Abandon, Restore, MoveFiles,
    SimplifyParents, Bookmark, Tag, Undo, Redo, RestoreOperation, WorkspaceAdd, WorkspaceForget, WorkspaceRename, TrackPaths,
    UntrackPaths, ChmodPaths>;

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
std::vector<std::size_t> UniquePrefixLengths(const std::vector<std::string>& values, std::size_t minimum = 8);
void MarkPushedRevisions(std::vector<Revision>& revisions, const std::vector<NamedRef>& refs);
std::string FirstLine(const std::string& value);

} // namespace Ggui

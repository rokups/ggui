// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "RepositoryEngine.hpp"
#include "RepositoryDiagnostics.hpp"

#include <efsw/efsw.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace Ggui::RepositoryInternal
{

std::string OidString(const git_oid& oid);
std::string LastGitError(std::string_view fallback);
void Check(int result, std::string_view action);
std::string BlobText(git_repository* repository, git_tree* tree, const char* path, bool& binary);
DiffLine DiffLineFromRaw(const git_diff_line& line, int hunk);
bool SameChangedLine(const DiffLine& first, const DiffLine& second);
bool SameInverseLine(const DiffLine& forward, const DiffLine& reverse);
std::vector<std::string_view> TextLines(std::string_view text);
bool CommitIsEmpty(git_repository* repository, const git_oid& oid);
void AppendConflicts(std::vector<Conflict>& destination, const gg_conflict_array& conflicts);
void FinishClone(const std::filesystem::path& temporary, const std::filesystem::path& destination);

struct GitStringArray
{
    git_strarray value{};
    ~GitStringArray() { git_strarray_dispose(&value); }
};

struct RepositoryDeleter
{
    void operator()(git_repository* value) const { git_repository_free(value); }
};
using GitRepositoryPtr = std::unique_ptr<git_repository, RepositoryDeleter>;

struct Mutation
{
    gg_mutation_result value{};
    ~Mutation() { gg_mutation_result_dispose(&value); }
};

struct TransportPlan
{
    gg_transport_plan value{};
    ~TransportPlan() { gg_transport_plan_dispose(&value); }
};

struct Revisions
{
    gg_revision_array value{};
    ~Revisions() { gg_revision_array_dispose(&value); }
};

struct Oids
{
    gg_oid_array value{};
    ~Oids() { gg_oid_array_dispose(&value); }
};

struct References
{
    gg_reference_array value{};
    ~References() { gg_reference_array_dispose(&value); }
};

struct NamedRefs
{
    gg_named_ref_array value{};
    ~NamedRefs() { gg_named_ref_array_dispose(&value); }
};

struct Status
{
    gg_status value{};
    ~Status() { gg_status_dispose(&value); }
};

struct Operations
{
    gg_operation_array value{};
    ~Operations() { gg_operation_array_dispose(&value); }
};

struct Workspaces
{
    gg_workspace_array value{};
    ~Workspaces() { gg_workspace_array_dispose(&value); }
};

struct Conflicts
{
    gg_conflict_array value{};
    ~Conflicts() { gg_conflict_array_dispose(&value); }
};

struct StringArray
{
    explicit StringArray(const std::vector<std::string>& strings)
    {
        values.reserve(strings.size());
        for (const std::string& value : strings)
            values.push_back(value.c_str());
    }

    gg_string_array Get() const { return {const_cast<const char**>(values.data()), values.size()}; }
    std::vector<const char*> values;
};

template <class... Ts> struct Overloaded : Ts...
{
    using Ts::operator()...;
};
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

class RepositoryWatcher final : public efsw::FileWatchListener
{
public:
    struct Changes
    {
        bool worktree = false;
        bool metadata = false;
        bool full_scan = false;
        std::vector<std::string> paths;
    };

    explicit RepositoryWatcher(std::condition_variable& wake) : _wake(wake) {}
    void Watch(const std::filesystem::path& worktree, const std::filesystem::path& common_directory,
        git_repository* repository);
    void Clear();
    bool Changed() const { return _worktree_changed.load() || _metadata_changed.load(); }
    Changes ConsumeChanges();

private:
    void handleFileAction(efsw::WatchID, const std::string& directory, const std::string& filename,
        efsw::Action, const std::string&) override;

    std::condition_variable& _wake;
    std::filesystem::path _common_directory;
    std::filesystem::path _worktree;
    bool _split_worktree = false;
    std::string _excluded_worktree_child;
    std::mutex _changes_mutex;
    std::vector<std::string> _changed_paths;
    std::vector<std::filesystem::path> _pending_watches;
    std::vector<std::filesystem::path> _skipped_worktree_directories;
    std::atomic_bool _full_scan = false;
    std::atomic_bool _worktree_changed = false;
    std::atomic_bool _metadata_changed = false;
    std::unique_ptr<efsw::FileWatcher> _watcher;
};

} // namespace Ggui::RepositoryInternal

namespace Ggui
{

struct RepositoryEngine::Impl
{
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    struct QueuedCommand
    {
        Command command;
        std::uint64_t task = 0;
        std::uint64_t repository_generation = 0;
        RepositoryInternal::DiagnosticClock::time_point queued;
    };
    std::deque<QueuedCommand> commands;
    std::atomic_uint64_t diagnostic_task = 0;
    std::atomic_bool stopping = false;

    std::mutex event_mutex;
    std::vector<Event> events;

    std::mutex credential_mutex;
    std::condition_variable credential_cv;
    std::optional<CredentialResponse> credential_response;
    bool credential_cancelled = false;
    bool ssh_agent_attempted = false;
    int credential_attempts = 0;
    std::string transfer_phase = "clone";

    std::atomic_bool cancel_requested = false;
    std::atomic_bool test_commands_suppressed = false;
    RepositoryInternal::RepositoryWatcher watcher{queue_cv};
    std::thread worker;
    struct InspectorRequest
    {
        std::variant<LoadDiff, LoadFileContent, LoadBlame> command;
        std::string path;
        std::uint64_t repository_generation = 0;
        std::uint64_t snapshot_generation = 0;
        std::uint64_t session = 0;
        std::uint64_t request = 0;
        std::uint64_t task = 0;
        RepositoryInternal::DiagnosticClock::time_point queued;
    };
    std::mutex inspector_mutex;
    std::condition_variable inspector_cv;
    std::deque<InspectorRequest> inspector_requests;
    std::vector<std::thread> inspectors;
    std::atomic_uint64_t inspector_request = 0;

    std::mutex history_mutex;
    std::condition_variable history_cv;
    std::thread history_worker;
    struct HistoryRequest
    {
        HistoryQuery query;
        std::string expand;
        std::string path;
        std::uint64_t session = 0;
        std::uint64_t request = 0;
        std::uint64_t task = 0;
        RepositoryInternal::DiagnosticClock::time_point queued;
    };
    std::optional<HistoryRequest> history_request;
    std::string repository_path;
    std::uint64_t repository_path_session = 0;
    HistoryQuery active_history_query;
    std::vector<std::string> expanded_history_regions;
    std::atomic_uint64_t history_request_version = 0;
    RepositoryInternal::GitRepositoryPtr git;
    gg_repository* gg = nullptr;
    std::atomic_uint64_t session = 0;
    std::atomic_uint64_t generation = 0;
    std::atomic_uint64_t topology_generation = 0;
    std::atomic_uint64_t background_activity_id = 0;
    std::mutex snapshot_mutex;
    std::shared_ptr<RepoSnapshot> latest_snapshot;
    std::vector<StatusEntry> cached_status;
    bool worktree_ready = false;

    struct BackgroundActivityGuard
    {
        BackgroundActivityGuard(Impl& owner, std::string name);
        ~BackgroundActivityGuard();
        BackgroundActivityGuard(const BackgroundActivityGuard&) = delete;
        BackgroundActivityGuard& operator=(const BackgroundActivityGuard&) = delete;

        Impl& owner;
        std::uint64_t id = 0;
    };

    Impl();
    ~Impl();

    void Close();
    void Post(Event event);
    static int CancelCallback(void* payload);
    static void ProgressCallback(const char* phase, size_t completed, size_t total, void* payload);
    gg_operation_options OperationOptions(bool report_progress = true);
    static int CredentialCallback(
        git_credential** out, const char* url, const char* username, unsigned int allowed, void* payload);
    int AcquireCredential(git_credential** out, const char* url, const char* username, unsigned int allowed);
    static int TransferProgress(const git_indexer_progress* stats, void* payload);
    void Attach(RepositoryInternal::GitRepositoryPtr repository);
    void OpenPath(const std::string& path);
    void InitPath(const std::string& path);
    void ClonePath(const CloneRepository& command);
    git_remote_callbacks RemoteCallbacks();
    void FetchRemote(const Fetch& command);
    void PushBookmark(const Push& command);
    void RemoveRemoteBookmark(const RemoteBookmarkDelete& command, bool publish);
    bool Sync(bool report_progress = true, const std::vector<std::string>& paths = {});
    std::shared_ptr<RepoSnapshot> ReadSnapshot(bool include_worktree = true, bool history_changed = false,
        const std::vector<std::string>& status_paths = {});
    void PublishSnapshot(bool include_worktree = true, bool history_changed = false,
        const std::vector<std::string>& status_paths = {});
    void LoadPatch(const LoadDiff& command);
    void LoadFile(const LoadFileContent& command);
    void LoadPatch(const LoadDiff& command, git_repository* repository, gg_repository* gg_repository,
        std::uint64_t snapshot_generation, std::uint64_t request_generation, std::uint64_t request_session);
    void LoadFile(const LoadFileContent& command, git_repository* repository, gg_repository* gg_repository,
        std::uint64_t request_generation, std::uint64_t request_session);
    void LoadBlameFile(const Ggui::LoadBlame& command, git_repository* repository, gg_repository* gg_repository,
        std::uint64_t snapshot_generation, std::uint64_t request_generation, std::uint64_t request_session);
    void ApplyPatchText(const ApplyPatch& command);
    void ResolveConflictFile(const ResolveConflict& command);
    void RevertFileChange(const RevertFile& command);
    void DeleteWorkingFile(const DeleteFile& command);
    void MoveDiffSelection(const MoveDiffLines& command, bool revert = false);
    template <class Function> void Mutate(std::string_view action, Function function, bool snapshot_after = false)
    {
        Sync();
        RepositoryInternal::Mutation mutation;
        gg_operation_options options = OperationOptions();
        RepositoryInternal::Check(function(&mutation.value, &options), action);
        if (snapshot_after)
            Sync();
        PublishSnapshot(true, true);
    }
    void DispatchMutation(const Command& command);
    void Execute(const Command& command, std::uint64_t task,
        std::uint64_t repository_generation, RepositoryInternal::DiagnosticClock::time_point queued);
    void Run();
    void RunInspector();
    void RunHistory();
    void RequestHistory(HistoryQuery query, std::string expand = {}, bool toggle = false);
};

} // namespace Ggui

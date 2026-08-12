// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "RepositoryEngine.hpp"

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
    };

    explicit RepositoryWatcher(std::condition_variable& wake) : _wake(wake) {}
    void Watch(const std::filesystem::path& worktree, const std::filesystem::path& common_directory);
    void Clear();
    bool Changed() const { return _worktree_changed.load() || _metadata_changed.load(); }
    Changes ConsumeChanges();

private:
    void handleFileAction(efsw::WatchID, const std::string& directory, const std::string& filename,
        efsw::Action, const std::string&) override;

    std::condition_variable& _wake;
    std::filesystem::path _common_directory;
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
    std::deque<Command> commands;
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
    RepositoryInternal::GitRepositoryPtr git;
    gg_repository* gg = nullptr;
    std::uint64_t generation = 0;

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
    void Sync(bool report_progress = true);
    std::shared_ptr<RepoSnapshot> ReadSnapshot();
    void PublishSnapshot();
    void LoadPatch(const LoadDiff& command);
    void ApplyPatchText(const ApplyPatch& command);
    void RevertFileChange(const RevertFile& command);
    void DeleteWorkingFile(const DeleteFile& command);
    void MoveDiffSelection(const MoveDiffLines& command, bool revert = false);
    template <class Function> void Mutate(std::string_view action, Function function)
    {
        Sync();
        RepositoryInternal::Mutation mutation;
        gg_operation_options options = OperationOptions();
        RepositoryInternal::Check(function(&mutation.value, &options), action);
        PublishSnapshot();
    }
    void DispatchMutation(const Command& command);
    std::string CommandName(const Command& command);
    void Execute(const Command& command);
    void Run();
};

} // namespace Ggui

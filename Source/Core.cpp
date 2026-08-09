// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Core.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace Ggui
{

namespace
{

std::string OidString(const git_oid& oid)
{
    char buffer[GIT_OID_MAX_HEXSIZE + 1]{};
    git_oid_tostr(buffer, sizeof(buffer), &oid);
    return buffer;
}

std::string LastGitError(std::string_view fallback)
{
    const git_error* error = git_error_last();
    return error != nullptr && error->message != nullptr ? error->message : std::string(fallback);
}

void Check(int result, std::string_view action)
{
    if (result < 0)
        throw std::runtime_error(std::string(action) + ": " + LastGitError("unknown error"));
}

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

struct SparsePatterns
{
    gg_owned_string_array value{};
    ~SparsePatterns() { gg_owned_string_array_dispose(&value); }
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

void AppendConflicts(std::vector<Conflict>& destination, const gg_conflict_array& conflicts)
{
    for (size_t index = 0; index < conflicts.count; ++index)
    {
        const gg_conflict& source = conflicts.items[index];
        destination.push_back(
            {source.path == nullptr ? "" : source.path, source.remove_count, source.add_count});
    }
}

void FinishClone(const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
    std::error_code rename_error;
    std::filesystem::rename(temporary, destination, rename_error);
    if (rename_error)
    {
        const std::string message = rename_error.message();
        std::error_code ignored;
        std::filesystem::remove_all(temporary, ignored);
        throw std::runtime_error("finish clone: " + message);
    }
}

} // namespace

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

    std::atomic_bool cancel_requested = false;
    std::atomic_bool test_commands_suppressed = false;
    std::thread worker;
    GitRepositoryPtr git;
    gg_repository* gg = nullptr;
    std::uint64_t generation = 0;

    Impl()
    {
        if (git_libgit2_init() < 0)
            throw std::runtime_error("initialize libgit2: " + LastGitError("unknown error")); // GCOV_EXCL_LINE: forced libgit2 bootstrap failure
        try
        {
            worker = std::thread([this] { Run(); });
        }
        catch (...) // GCOV_EXCL_START: forced standard-library thread construction failure
        {
            git_libgit2_shutdown();
            throw;
        }
        // GCOV_EXCL_STOP
    }

    ~Impl()
    {
        {
            std::lock_guard lock(queue_mutex);
            stopping = true;
        }
        {
            std::lock_guard lock(credential_mutex);
            credential_cancelled = true;
        }
        queue_cv.notify_all();
        credential_cv.notify_all();
        if (worker.joinable())
            worker.join();
        Close();
        git_libgit2_shutdown();
    }

    void Close()
    {
        if (gg != nullptr)
            gg_repository_free(gg);
        gg = nullptr;
        git.reset();
    }

    void Post(Event event)
    {
        std::lock_guard lock(event_mutex);
        events.push_back(std::move(event));
    }

    static int CancelCallback(void* payload)
    {
        return static_cast<Impl*>(payload)->cancel_requested.load() ? 1 : 0;
    }

    static void ProgressCallback(const char* phase, size_t completed, size_t total, void* payload)
    {
        static_cast<Impl*>(payload)->Post(
            OperationProgress{phase == nullptr ? "" : phase, completed, total});
    }

    gg_operation_options OperationOptions()
    {
        gg_operation_options options = GG_OPERATION_OPTIONS_INIT;
        options.cancel_cb = CancelCallback;
        options.progress_cb = ProgressCallback;
        options.payload = this;
        return options;
    }

    static int CredentialCallback(
        git_credential** out, const char* url, const char* username, unsigned int allowed, void* payload)
    {
        return static_cast<Impl*>(payload)->AcquireCredential(out, url, username, allowed);
    }

    int AcquireCredential(git_credential** out, const char* url, const char* username, unsigned int allowed)
    {
        if (++credential_attempts > 3)
            return GIT_EAUTH;
        if ((allowed & GIT_CREDENTIAL_DEFAULT) != 0)
        {
            if (git_credential_default_new(out) == GIT_OK)
                return GIT_OK;
        }
        if (!ssh_agent_attempted && (allowed & GIT_CREDENTIAL_SSH_KEY) != 0)
        {
            ssh_agent_attempted = true;
            const char* user = username != nullptr && *username != '\0' ? username : "git";
            if (git_credential_ssh_key_from_agent(out, user) == GIT_OK)
                return GIT_OK;
        }

        {
            std::lock_guard lock(credential_mutex);
            credential_response.reset();
            credential_cancelled = false;
        }
        Post(CredentialRequest{url == nullptr ? "" : url, username == nullptr ? "" : username, allowed});

        std::unique_lock lock(credential_mutex);
        credential_cv.wait(lock, [this] {
            return credential_response.has_value() || credential_cancelled || stopping || cancel_requested.load();
        });
        if (!credential_response.has_value())
            return GIT_EUSER;
        CredentialResponse response = std::move(*credential_response);
        credential_response.reset();
        lock.unlock();

        int result = GIT_EUSER;
        if (response.method == CredentialResponse::Method::UserPass && (allowed & GIT_CREDENTIAL_USERPASS_PLAINTEXT))
        {
            result = git_credential_userpass_plaintext_new(out, response.username.c_str(), response.secret.c_str());
        }
        else if (response.method == CredentialResponse::Method::SshAgent && (allowed & GIT_CREDENTIAL_SSH_KEY))
        {
            const char* user = response.username.empty() ? "git" : response.username.c_str();
            result = git_credential_ssh_key_from_agent(out, user);
        }
        else if (response.method == CredentialResponse::Method::SshKey && (allowed & GIT_CREDENTIAL_SSH_KEY))
        {
            const char* user = response.username.empty() ? "git" : response.username.c_str();
            const char* public_key = response.public_key.empty() ? nullptr : response.public_key.c_str();
            result = git_credential_ssh_key_new(
                out, user, public_key, response.private_key.c_str(), response.secret.c_str());
        }
        std::fill(response.secret.begin(), response.secret.end(), '\0');
        return result;
    }

    static int TransferProgress(const git_indexer_progress* stats, void* payload)
    {
        auto* self = static_cast<Impl*>(payload);
        self->Post(OperationProgress{"clone", stats->received_objects, stats->total_objects});
        return self->cancel_requested.load() ? GIT_EUSER : GIT_OK;
    }

    void Attach(GitRepositoryPtr repository)
    {
        gg_repository* attached = nullptr;
        Check(gg_repository_attach(&attached, repository.get()), "attach gg repository");
        gg_operation_options options = OperationOptions();
        const int adopted = gg_repository_adopt_git_history(attached, &options);
        if (adopted < 0)
        {
            gg_repository_free(attached); // GCOV_EXCL_LINE: libgg returned an attached handle and then rejected it
            Check(adopted, "adopt Git history"); // GCOV_EXCL_LINE: requires corrupt libgg attach state
        }
        Close();
        git = std::move(repository);
        gg = attached;
        Sync();
        PublishSnapshot();
    }

    void OpenPath(const std::string& path)
    {
        git_repository* raw = nullptr;
        Check(git_repository_open_ext(&raw, path.c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr), "open repository");
        Attach(GitRepositoryPtr(raw));
    }

    void InitPath(const std::string& path)
    {
        git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
        options.flags = GIT_REPOSITORY_INIT_MKPATH;
        git_repository* raw = nullptr;
        Check(git_repository_init_ext(&raw, path.c_str(), &options), "initialize repository");
        Attach(GitRepositoryPtr(raw));
    }

    void ClonePath(const CloneRepository& command)
    {
        namespace fs = std::filesystem;
        const fs::path destination = fs::absolute(command.path);
        if (fs::exists(destination))
            throw std::runtime_error("clone destination must not exist");
        const fs::path temporary = destination.string() + ".ggui-clone";
        if (fs::exists(temporary))
            throw std::runtime_error("temporary clone destination already exists: " + temporary.string());

        git_clone_options options = GIT_CLONE_OPTIONS_INIT;
        options.fetch_opts.callbacks.credentials = CredentialCallback;
        options.fetch_opts.callbacks.transfer_progress = TransferProgress;
        options.fetch_opts.callbacks.payload = this;
        ssh_agent_attempted = false;
        credential_attempts = 0;
        git_repository* raw = nullptr;
        const int result = git_clone(&raw, command.url.c_str(), temporary.string().c_str(), &options);
        if (result < 0)
        {
            if (raw != nullptr)
                git_repository_free(raw); // GCOV_EXCL_LINE: libgit2 normally returns null on clone failure
            std::error_code ignored;
            fs::remove_all(temporary, ignored);
            Check(result, "clone repository");
        }
        GitRepositoryPtr cloned(raw);
        cloned.reset();
        FinishClone(temporary, destination);
        OpenPath(destination.string());
    }

    void Sync()
    {
        if (gg == nullptr)
            return;
        gg_operation_options options = OperationOptions();
        Check(gg_repository_adopt_git_history(gg, &options), "adopt external Git history");
        int changed = 0;
        Check(gg_repository_snapshot_working_copy(&changed, gg, &options), "snapshot working copy");
    }

    std::shared_ptr<RepoSnapshot> ReadSnapshot()
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
        query.limit = 2000;
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
            value.change_id = source.change_id == nullptr ? "" : source.change_id;
            value.description = source.description == nullptr ? "" : source.description;
            value.author = source.author == nullptr || source.author->name == nullptr ? "" : source.author->name;
            value.timestamp = source.committer == nullptr ? 0 : source.committer->when.time;
            value.working_copy = value.oid == result->working_copy;
            value.conflicted = source.has_conflicts != 0;
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

        Workspaces workspaces;
        Check(gg_repository_workspaces(&workspaces.value, gg), "load workspaces");
        result->workspaces.reserve(workspaces.value.count);
        for (size_t index = 0; index < workspaces.value.count; ++index)
        {
            const gg_workspace& source = workspaces.value.items[index];
            result->workspaces.push_back({source.name == nullptr ? "" : source.name,
                source.root == nullptr ? "" : source.root, OidString(source.working_copy), source.stale != 0});
        }

        SparsePatterns sparse;
        Check(gg_repository_sparse_patterns(&sparse.value, gg), "load sparse patterns");
        for (size_t index = 0; index < sparse.value.count; ++index)
            result->sparse_patterns.emplace_back(sparse.value.strings[index]);

        if (!result->working_copy.empty())
        {
            Conflicts conflicts;
            Check(gg_repository_conflicts(&conflicts.value, gg, &working), "load conflicts");
            AppendConflicts(result->conflicts, conflicts.value);
        }
        return result;
    }

    void PublishSnapshot()
    {
        if (gg != nullptr)
            Post(SnapshotReady{ReadSnapshot()});
    }

    void LoadPatch(const LoadDiff& command)
    {
        git_oid oid{};
        Check(gg_repository_resolve(&oid, gg, command.revision.c_str()), "resolve diff revision");
        git_commit* raw_commit = nullptr;
        Check(git_commit_lookup(&raw_commit, git.get(), &oid), "load diff revision");
        std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
        git_tree* raw_new_tree = nullptr;
        Check(git_commit_tree(&raw_new_tree, commit.get()), "load revision tree");
        std::unique_ptr<git_tree, decltype(&git_tree_free)> new_tree(raw_new_tree, git_tree_free);
        git_tree* raw_old_tree = nullptr;
        if (git_commit_parentcount(commit.get()) != 0)
        {
            git_commit* raw_parent = nullptr;
            Check(git_commit_parent(&raw_parent, commit.get(), 0), "load revision parent");
            std::unique_ptr<git_commit, decltype(&git_commit_free)> parent(raw_parent, git_commit_free);
            Check(git_commit_tree(&raw_old_tree, parent.get()), "load parent tree");
        }
        std::unique_ptr<git_tree, decltype(&git_tree_free)> old_tree(raw_old_tree, git_tree_free);
        git_diff_options options = GIT_DIFF_OPTIONS_INIT;
        char* path = const_cast<char*>(command.path.c_str());
        if (!command.path.empty())
            options.pathspec = {&path, 1};
        git_diff* raw_diff = nullptr;
        Check(git_diff_tree_to_tree(&raw_diff, git.get(), old_tree.get(), new_tree.get(), &options), "create diff");
        std::unique_ptr<git_diff, decltype(&git_diff_free)> diff(raw_diff, git_diff_free);
        git_buf patch = GIT_BUF_INIT;
        Check(git_diff_to_buf(&patch, diff.get(), GIT_DIFF_FORMAT_PATCH), "format diff");
        DiffResult result{generation, command.revision, command.path,
            patch.ptr == nullptr ? "" : std::string(patch.ptr, patch.size), false};
        for (size_t index = 0; index < git_diff_num_deltas(diff.get()); ++index)
        {
            const git_diff_delta* delta = git_diff_get_delta(diff.get(), index);
            result.binary |= delta != nullptr && (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;
        }
        git_buf_dispose(&patch);
        Post(DiffReady{std::move(result)});
    }

    template <class Function> void Mutate(std::string_view action, Function function)
    {
        Sync();
        Mutation mutation;
        gg_operation_options options = OperationOptions();
        Check(function(&mutation.value, &options), action);
        PublishSnapshot();
    }

    void DispatchMutation(const Command& command)
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
                    gg_rebase_options options = GG_REBASE_OPTIONS_INIT;
                    options.source = value.source.c_str();
                    options.destination = value.destination.c_str();
                    Mutate("rebase change", [&](auto* out, auto* operation) {
                        return gg_repository_rebase(out, gg, &options, operation);
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
                    gg_abandon_options options = GG_ABANDON_OPTIONS_INIT;
                    const StringArray revisions(value.revisions);
                    options.revisions = revisions.Get();
                    options.retain_bookmarks = value.retain_bookmarks;
                    options.restore_descendants = value.restore_descendants;
                    Mutate("abandon change", [&](auto* out, auto* operation) {
                        return gg_repository_abandon(out, gg, &options, operation);
                    });
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
                    Mutate("undo", [&](auto* out, auto* operation) { return gg_repository_undo(out, gg, operation); });
                },
                [&](const Redo&) {
                    Mutate("redo", [&](auto* out, auto* operation) { return gg_repository_redo(out, gg, operation); });
                },
                [&](const RestoreOperation& value) {
                    Mutate("restore operation", [&](auto* out, auto* operation) {
                        return gg_repository_restore_operation(out, gg, value.operation.c_str(), GG_RESTORE_ALL, operation);
                    });
                },
                [&](const WorkspaceAdd& value) {
                    gg_workspace_add_options options = GG_WORKSPACE_ADD_OPTIONS_INIT;
                    static constexpr const char* modes[] = {"copy", "full", "empty"};
                    options.destination = value.destination.c_str();
                    options.name = value.name.c_str();
                    options.revision = value.revision.c_str();
                    options.message = value.message.c_str();
                    options.sparse_patterns = modes[std::clamp(value.sparse_mode, 0, 2)];
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
                [&](const SparseReset&) {
                    Mutate("reset sparse patterns", [&](auto* out, auto* operation) {
                        return gg_repository_sparse_reset(out, gg, operation);
                    });
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
            Overloaded{[](const OpenRepository&) { return "open"; }, [](const InitRepository&) { return "init"; },
                [](const CloneRepository&) { return "clone"; }, [](const Refresh&) { return "refresh"; },
                [](const LoadDiff&) { return "diff"; }, [](const NewChange&) { return "new"; },
                [](const Describe&) { return "describe"; }, [](const Metaedit&) { return "metaedit"; },
                [](const Edit&) { return "edit"; }, [](const MoveChange&) { return "move"; },
                [](const Commit&) { return "commit"; },
                [](const Rebase&) { return "rebase"; }, [](const Reorder&) { return "reorder"; },
                [](const Split&) { return "split"; }, [](const Squash&) { return "squash"; },
                [](const Abandon&) { return "abandon"; }, [](const Restore&) { return "restore"; },
                [](const SimplifyParents&) { return "simplify parents"; }, [](const Bookmark&) { return "bookmark"; },
                [](const Tag&) { return "tag"; }, [](const Undo&) { return "undo"; }, [](const Redo&) { return "redo"; },
                [](const RestoreOperation&) { return "restore operation"; },
                [](const WorkspaceAdd&) { return "add workspace"; },
                [](const WorkspaceForget&) { return "forget workspace"; },
                [](const WorkspaceRename&) { return "rename workspace"; },
                [](const SparseReset&) { return "reset sparse patterns"; },
                [](const TrackPaths&) { return "track paths"; }, [](const UntrackPaths&) { return "untrack paths"; },
                [](const ChmodPaths&) { return "chmod"; }},
            command);
    }

    void Execute(const Command& command)
    {
        const std::string name = CommandName(command);
        const bool quiet = std::holds_alternative<Refresh>(command) || std::holds_alternative<LoadDiff>(command);
        if (!quiet)
            Post(OperationStarted{name});
        cancel_requested = false;
        try
        {
            if (const auto* value = std::get_if<OpenRepository>(&command))
                OpenPath(value->path);
            else if (const auto* value = std::get_if<InitRepository>(&command))
                InitPath(value->path);
            else if (const auto* value = std::get_if<CloneRepository>(&command))
                ClonePath(*value);
            else if (std::holds_alternative<Refresh>(command))
            {
                Sync();
                PublishSnapshot();
            }
            else if (const auto* value = std::get_if<LoadDiff>(&command))
                LoadPatch(*value);
            else
                DispatchMutation(command);
            if (!quiet)
                Post(OperationFinished{name});
        }
        catch (const std::exception& error)
        {
            spdlog::error("{} failed: {}", name, error.what());
            Post(ErrorEvent{name, error.what()});
        }
    }

    void Run()
    {
        while (true)
        {
            std::optional<Command> command;
            {
                std::unique_lock lock(queue_mutex);
                queue_cv.wait_for(lock, std::chrono::seconds(1), [this] { return stopping || !commands.empty(); });
                if (stopping)
                    break;
                if (!commands.empty())
                {
                    command = std::move(commands.front());
                    commands.pop_front();
                }
            }
            if (command.has_value())
                Execute(*command);
            else if (gg != nullptr && !test_commands_suppressed)
                Execute(Refresh{});
        }
    }
};

RepositoryEngine::RepositoryEngine() : _impl(std::make_unique<Impl>()) {}
RepositoryEngine::~RepositoryEngine() = default;

void RepositoryEngine::Enqueue(Command command)
{
#ifdef GGUI_TESTING
    if (_impl->test_commands_suppressed)
        return;
#endif
    {
        std::lock_guard lock(_impl->queue_mutex);
        _impl->commands.push_back(std::move(command));
    }
    _impl->queue_cv.notify_one();
}

std::vector<Event> RepositoryEngine::PollEvents()
{
    std::lock_guard lock(_impl->event_mutex);
    std::vector<Event> result;
    result.swap(_impl->events);
    return result;
}

void RepositoryEngine::Cancel()
{
    _impl->cancel_requested = true;
    _impl->credential_cv.notify_all();
}

void RepositoryEngine::SubmitCredential(CredentialResponse response)
{
    {
        std::lock_guard lock(_impl->credential_mutex);
        _impl->credential_response = std::move(response);
        _impl->credential_cancelled = false;
    }
    _impl->credential_cv.notify_all();
}

void RepositoryEngine::CancelCredential()
{
    {
        std::lock_guard lock(_impl->credential_mutex);
        _impl->credential_cancelled = true;
    }
    _impl->credential_cv.notify_all();
}

#ifdef GGUI_TESTING
void RepositoryEngine::ResetCredentialStateForTest(bool ssh_agent_attempted, int credential_attempts)
{
    std::lock_guard lock(_impl->credential_mutex);
    _impl->credential_response.reset();
    _impl->credential_cancelled = false;
    _impl->ssh_agent_attempted = ssh_agent_attempted;
    _impl->credential_attempts = credential_attempts;
    _impl->cancel_requested = false;
}

int RepositoryEngine::AcquireCredentialForTest(
    git_credential** out, const char* url, const char* username, unsigned int allowed_types)
{
    return _impl->CredentialCallback(out, url, username, allowed_types, _impl.get());
}

int RepositoryEngine::TransferProgressForTest(const git_indexer_progress& progress)
{
    return _impl->TransferProgress(&progress, _impl.get());
}

void RepositoryEngine::DispatchMutationForTest(const Command& command)
{
    _impl->DispatchMutation(command);
}

void RepositoryEngine::SetCommandsSuppressedForTest(bool suppressed)
{
    _impl->test_commands_suppressed = suppressed;
    if (!suppressed)
        return;
    {
        std::lock_guard lock(_impl->queue_mutex);
        _impl->commands.clear();
    }
    {
        std::lock_guard lock(_impl->event_mutex);
        _impl->events.clear();
    }
}

std::vector<Conflict> RepositoryEngine::ConvertConflictsForTest()
{
    char path[] = "conflicted.txt";
    gg_conflict values[2]{};
    values[0].path = path;
    values[0].remove_count = 2;
    values[0].add_count = 3;
    values[1].path = nullptr;
    std::vector<Conflict> result;
    AppendConflicts(result, {values, 2});
    return result;
}

void RepositoryEngine::FinishCloneForTest(
    const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
    FinishClone(temporary, destination);
}
#endif

std::string ShortId(const std::string& value, std::size_t length)
{
    return value.substr(0, std::min(length, value.size()));
}

std::string FirstLine(const std::string& value)
{
    return value.substr(0, value.find('\n'));
}

} // namespace Ggui

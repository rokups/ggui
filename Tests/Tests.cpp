// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Core.hpp"
#include "Graph.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Ggui
{
namespace
{

using namespace std::chrono_literals;

void CheckGit(int result)
{
    if (result < 0)
    {
        const git_error* error = git_error_last();
        throw std::runtime_error(error == nullptr ? "libgit2 operation failed" : error->message);
    }
}

struct TemporaryRepository
{
    TemporaryRepository()
    {
        static std::atomic_uint counter = 0;
        path = std::filesystem::temp_directory_path() /
            ("ggui-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(++counter));
        std::filesystem::remove_all(path);

        if (git_libgit2_init() <= 0)
            throw std::runtime_error("could not initialize libgit2");
        git_repository* raw_repository = nullptr;
        CheckGit(git_repository_init(&raw_repository, path.string().c_str(), 0));
        std::unique_ptr<git_repository, decltype(&git_repository_free)> repository(raw_repository, git_repository_free);
        CheckGit(git_repository_set_head(repository.get(), "refs/heads/main"));

        git_config* raw_config = nullptr;
        CheckGit(git_repository_config(&raw_config, repository.get()));
        std::unique_ptr<git_config, decltype(&git_config_free)> config(raw_config, git_config_free);
        CheckGit(git_config_set_string(config.get(), "user.name", "ggui test"));
        CheckGit(git_config_set_string(config.get(), "user.email", "ggui@example.test"));

        std::ofstream(path / "tracked.txt") << "base\n";
        const char binary_contents[]{'b', '\0', 'n'};
        std::ofstream binary(path / "binary.dat", std::ios::binary);
        binary.write(binary_contents, sizeof(binary_contents));
        binary.close();
        git_index* raw_index = nullptr;
        CheckGit(git_repository_index(&raw_index, repository.get()));
        std::unique_ptr<git_index, decltype(&git_index_free)> index(raw_index, git_index_free);
        CheckGit(git_index_add_bypath(index.get(), "tracked.txt"));
        CheckGit(git_index_add_bypath(index.get(), "binary.dat"));
        git_index_entry gitlink{};
        gitlink.mode = GIT_FILEMODE_COMMIT;
        gitlink.id = git_index_get_bypath(index.get(), "tracked.txt", 0)->id;
        gitlink.path = "gitlink";
        CheckGit(git_index_add(index.get(), &gitlink));
        CheckGit(git_index_write(index.get()));
        git_oid tree_oid{};
        CheckGit(git_index_write_tree(&tree_oid, index.get()));

        git_tree* raw_tree = nullptr;
        CheckGit(git_tree_lookup(&raw_tree, repository.get(), &tree_oid));
        std::unique_ptr<git_tree, decltype(&git_tree_free)> tree(raw_tree, git_tree_free);
        git_signature* raw_signature = nullptr;
        CheckGit(git_signature_now(&raw_signature, "ggui test", "ggui@example.test"));
        std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(raw_signature, git_signature_free);
        git_oid commit_oid{};
        CheckGit(git_commit_create(&commit_oid, repository.get(), "HEAD", signature.get(), signature.get(), nullptr,
            "base", tree.get(), 0, nullptr));
        git_remote* raw_remote = nullptr;
        CheckGit(git_remote_create(&raw_remote, repository.get(), "origin", "https://example.test/repository.git"));
        git_remote_free(raw_remote);
        repository.reset();
        git_libgit2_shutdown();
    }

    ~TemporaryRepository() { std::filesystem::remove_all(path); }

    std::filesystem::path path;
};

struct RemovePath
{
    ~RemovePath() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

std::shared_ptr<const RepoSnapshot> WaitForSnapshot(
    RepositoryEngine& engine, const std::function<bool(const RepoSnapshot&)>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::shared_ptr<const RepoSnapshot> last_snapshot;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const Event& event : engine.PollEvents())
        {
            if (const auto* error = std::get_if<ErrorEvent>(&event))
            {
                ADD_FAILURE() << error->operation << ": " << error->message;
                return {};
            }
            if (const auto* ready = std::get_if<SnapshotReady>(&event))
            {
                last_snapshot = ready->snapshot;
                if (predicate(*ready->snapshot))
                    return ready->snapshot;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    ADD_FAILURE() << "timed out waiting for repository snapshot; last generation="
                  << (last_snapshot == nullptr ? 0 : last_snapshot->generation)
                  << ", revisions=" << (last_snapshot == nullptr ? 0 : last_snapshot->revisions.size())
                  << ", status=" << (last_snapshot == nullptr ? 0 : last_snapshot->status.size());
    return {};
}

struct TerminalEvent
{
    bool finished = false;
    std::string message;
};

TerminalEvent WaitForTerminal(RepositoryEngine& engine, const std::string& operation)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const Event& event : engine.PollEvents())
        {
            if (const auto* finished = std::get_if<OperationFinished>(&event); finished != nullptr
                && finished->name == operation)
                return {true, {}};
            if (const auto* error = std::get_if<ErrorEvent>(&event); error != nullptr && error->operation == operation)
                return {false, error->message};
        }
        std::this_thread::sleep_for(5ms);
    }
    ADD_FAILURE() << "timed out waiting for operation " << operation;
    return {};
}

#ifdef GGUI_TESTING
std::optional<CredentialRequest> WaitForCredentialRequest(RepositoryEngine& engine)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const Event& event : engine.PollEvents())
            if (const auto* request = std::get_if<CredentialRequest>(&event))
                return *request;
        std::this_thread::sleep_for(5ms);
    }
    ADD_FAILURE() << "timed out waiting for credential request";
    return std::nullopt;
}
#endif

std::optional<DiffResult> WaitForDiff(RepositoryEngine& engine)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const Event& event : engine.PollEvents())
        {
            if (const auto* diff = std::get_if<DiffReady>(&event))
                return diff->diff;
            if (const auto* error = std::get_if<ErrorEvent>(&event))
            {
                ADD_FAILURE() << error->operation << ": " << error->message;
                return std::nullopt;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    ADD_FAILURE() << "timed out waiting for diff";
    return std::nullopt;
}

void ExerciseCommand(RepositoryEngine& engine, Command command, const std::string& operation)
{
    engine.Enqueue(std::move(command));
    const TerminalEvent terminal = WaitForTerminal(engine, operation);
    EXPECT_TRUE(terminal.finished || !terminal.message.empty()) << operation;
}

TEST(GraphLayout, HandlesLinearAndArbitraryParentGraphs)
{
    const std::vector<GraphNode> nodes{
        {"merge", {"left", "right", "octopus"}},
        {"left", {"base"}},
        {"right", {"base"}},
        {"octopus", {"base"}},
        {"base", {}},
    };
    const std::vector<GraphRow> rows = BuildGraphLayout(nodes);
    ASSERT_EQ(rows.size(), nodes.size());
    EXPECT_EQ(rows.front().parent_columns.size(), 3U);
    EXPECT_GE(GraphColumnCount(rows), 3);
    EXPECT_TRUE(rows.back().tracks_after.empty());
    EXPECT_EQ(rows[0].parent_columns, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(rows[0].tracks_after, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(rows[1].tracks_before, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(rows[2].tracks_after, (std::vector<int>{0, 2}));
    EXPECT_EQ(rows[3].tracks_before, (std::vector<int>{0, 2}));
    EXPECT_EQ(rows[4].tracks_before, (std::vector<int>{0}));
    EXPECT_EQ(GraphColumnCount(rows.front()), 3);
    EXPECT_EQ(GraphColumnCount(rows.back()), 1);
    for (std::size_t row = 0; row + 1 < rows.size(); ++row)
        EXPECT_EQ(rows[row].tracks_after, rows[row + 1].tracks_before);
}

TEST(GraphLayout, MaintainsUniqueContinuousLanesAcrossComplexDag)
{
    const std::vector<GraphNode> nodes{{"merge", {"a", "b", "c"}}, {"a", {"d", "e"}},
        {"b", {"e", "f"}}, {"c", {"d", "f"}}, {"d", {"root"}}, {"e", {"root"}},
        {"f", {"root"}}, {"root", {}}};
    const std::vector<GraphRow> rows = BuildGraphLayout(nodes);
    ASSERT_EQ(rows.size(), nodes.size());
    for (std::size_t row = 0; row < rows.size(); ++row)
    {
        std::vector<int> unique = rows[row].tracks_after;
        std::sort(unique.begin(), unique.end());
        EXPECT_EQ(std::adjacent_find(unique.begin(), unique.end()), unique.end());
        if (row + 1 < rows.size())
        {
            EXPECT_EQ(rows[row].tracks_after, rows[row + 1].tracks_before);
        }
        for (int parent : rows[row].parent_columns)
            EXPECT_GE(parent, 0);
        for (int parent : rows[row].parent_columns)
            EXPECT_LT(parent, static_cast<int>(rows[row].tracks_after.size()));
    }
}

TEST(GraphLayout, IsDeterministic)
{
    const std::vector<GraphNode> nodes{{"c", {"b"}}, {"b", {"a"}}, {"a", {}}};
    EXPECT_EQ(BuildGraphLayout(nodes).front().column, BuildGraphLayout(nodes).front().column);
    EXPECT_EQ(GraphColumnCount(BuildGraphLayout(nodes)), 1);
}

TEST(GraphLayout, MarksMissingParentsAndIgnoresBackwardsParents)
{
    const std::vector<GraphNode> nodes{{"a", {"missing", "a"}}, {"b", {"a"}}};
    const auto rows = BuildGraphLayout(nodes);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_TRUE(rows.front().parent_columns.empty());
    EXPECT_TRUE(rows.front().continues_beyond_layout);
    EXPECT_FALSE(rows.back().continues_beyond_layout);
}

TEST(TextHelpers, ShortensAndSelectsFirstLine)
{
    EXPECT_EQ(ShortId("abcdefghijkl", 8), "abcdefgh");
    EXPECT_EQ(UniquePrefixLengths({"alpha", "beta"}), (std::vector<std::size_t>{1, 1}));
    EXPECT_EQ(UniquePrefixLengths({"abcdef00", "abcdef11", "xyz00000"}, 2),
        (std::vector<std::size_t>{7, 7, 2}));
    EXPECT_EQ(FirstLine("subject\nbody"), "subject");
}

TEST(RevisionHelpers, MarksRemoteAncestryAsPushed)
{
    std::vector<Revision> revisions{
        {"tip", {"root"}, {}, {}, {}, 0, false, false, false},
        {"side", {"root"}, {}, {}, {}, 0, false, false, false},
        {"root", {}, {}, {}, {}, 0, false, false, false},
    };
    MarkPushedRevisions(revisions, {{"main", "origin", "tip", GG_NAMED_REF_REMOTE_BOOKMARK, true, false}});
    EXPECT_TRUE(revisions[0].pushed);
    EXPECT_FALSE(revisions[1].pushed);
    EXPECT_TRUE(revisions[2].pushed);
}

TEST(RepositoryEngine, OpensAndAutomaticallyRefreshesARepository)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); });
    ASSERT_NE(opened, nullptr);
    EXPECT_EQ(opened->revisions.back().description, "base");
    ASSERT_EQ(opened->remotes.size(), 1U);
    EXPECT_EQ(opened->remotes.front().name, "origin");
    EXPECT_EQ(opened->remotes.front().fetch_url, "https://example.test/repository.git");
    EXPECT_TRUE(opened->can_undo);
    EXPECT_FALSE(opened->can_redo);
    std::this_thread::sleep_for(1200ms);
    const auto idle_events = engine.PollEvents();
    EXPECT_TRUE(std::none_of(idle_events.begin(), idle_events.end(),
        [](const Event& event) { return std::holds_alternative<SnapshotReady>(event); }));

    NewChange create;
    create.message = "work";
    engine.Enqueue(std::move(create));
    const auto working = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.working_copy.empty();
    });
    ASSERT_NE(working, nullptr);

    std::ofstream(repository.path / "tracked.txt") << "changed\n";
    const auto refreshed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > working->generation && !snapshot.status.empty();
    });
    ASSERT_NE(refreshed, nullptr);
    EXPECT_EQ(refreshed->status.front().path, "tracked.txt");
    EXPECT_EQ(refreshed->status.front().status, GIT_DELTA_MODIFIED);
}

TEST(RepositoryEngine, ClonesThroughATemporaryDestination)
{
    TemporaryRepository source;
    RemovePath destination{source.path.string() + "-clone"};
    std::filesystem::remove_all(destination.path);
    RepositoryEngine engine;
    engine.Enqueue(CloneRepository{source.path.string(), destination.path.string()});
    const auto cloned = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.revisions.empty();
    });
    ASSERT_NE(cloned, nullptr);
    EXPECT_TRUE(std::filesystem::exists(destination.path / ".git"));
    EXPECT_EQ(std::filesystem::weakly_canonical(cloned->root), std::filesystem::weakly_canonical(destination.path));
    EXPECT_FALSE(std::filesystem::exists(destination.path.string() + ".ggui-clone"));
}

TEST(RepositoryEngine, LoadsRootRevisionDiffs)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(Refresh{});
    std::this_thread::sleep_for(20ms);
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto snapshot = WaitForSnapshot(engine, [](const RepoSnapshot& value) { return !value.revisions.empty(); });
    ASSERT_NE(snapshot, nullptr);
    const Revision& root = snapshot->revisions.back();
    engine.Enqueue(LoadDiff{root.oid, "tracked.txt"});
    const auto diff = WaitForDiff(engine);
    ASSERT_TRUE(diff.has_value());
    EXPECT_EQ(diff->revision, root.oid);
    EXPECT_EQ(diff->path, "tracked.txt");
    EXPECT_TRUE(diff->before.empty());
    EXPECT_EQ(diff->after, "base\n");
    const auto tracked = std::ranges::find(diff->files, "tracked.txt", &StatusEntry::path);
    ASSERT_NE(tracked, diff->files.end());
    EXPECT_EQ(tracked->status, GIT_DELTA_ADDED);
    EXPECT_FALSE(diff->patch.empty());
    EXPECT_FALSE(diff->binary);

    engine.Enqueue(LoadDiff{root.oid, "missing.txt"});
    const auto missing = WaitForDiff(engine);
    ASSERT_TRUE(missing.has_value());
    EXPECT_TRUE(missing->after.empty());

    engine.Enqueue(LoadDiff{root.oid, "gitlink"});
    const auto gitlink = WaitForDiff(engine);
    ASSERT_TRUE(gitlink.has_value());
    EXPECT_TRUE(gitlink->after.empty());

    engine.Enqueue(LoadDiff{root.oid, "binary.dat"});
    const auto binary = WaitForDiff(engine);
    ASSERT_TRUE(binary.has_value());
    EXPECT_TRUE(binary->after.empty());
    EXPECT_TRUE(binary->binary);
}

TEST(RepositoryEngine, InitializesAndReportsFilesystemErrors)
{
    RemovePath initialized{std::filesystem::temp_directory_path() / "ggui-init-coverage"};
    std::filesystem::remove_all(initialized.path);
    RepositoryEngine init_engine;
    init_engine.Enqueue(InitRepository{initialized.path.string()});
    const auto snapshot = WaitForSnapshot(init_engine, [](const RepoSnapshot&) { return true; });
    ASSERT_NE(snapshot, nullptr);
    EXPECT_TRUE(std::filesystem::exists(initialized.path / ".git"));

    RepositoryEngine open_engine;
    open_engine.Enqueue(OpenRepository{(initialized.path / "missing").string()});
    const TerminalEvent open = WaitForTerminal(open_engine, "open");
    EXPECT_FALSE(open.finished);
    EXPECT_FALSE(open.message.empty());

    TemporaryRepository source;
    RepositoryEngine clone_engine;
    clone_engine.Enqueue(CloneRepository{source.path.string(), initialized.path.string()});
    const TerminalEvent destination_exists = WaitForTerminal(clone_engine, "clone");
    EXPECT_FALSE(destination_exists.finished);
    EXPECT_NE(destination_exists.message.find("must not exist"), std::string::npos);

    RemovePath destination{source.path.string() + "-blocked-clone"};
    std::filesystem::create_directories(destination.path.string() + ".ggui-clone");
    clone_engine.Enqueue(CloneRepository{source.path.string(), destination.path.string()});
    const TerminalEvent temporary_exists = WaitForTerminal(clone_engine, "clone");
    EXPECT_FALSE(temporary_exists.finished);
    EXPECT_NE(temporary_exists.message.find("temporary clone destination"), std::string::npos);

    RemovePath failed_clone{source.path.string() + "-failed-clone"};
    std::filesystem::remove_all(failed_clone.path);
    clone_engine.Enqueue(CloneRepository{"/definitely/missing/clone-source", failed_clone.path.string()});
    const TerminalEvent clone_error = WaitForTerminal(clone_engine, "clone");
    EXPECT_FALSE(clone_error.finished);
    EXPECT_FALSE(clone_error.message.empty());

#ifdef GGUI_TESTING
    RemovePath rename_source{source.path.string() + "-rename-source"};
    RemovePath rename_destination{source.path.string() + "-rename-destination"};
    std::filesystem::create_directories(rename_source.path);
    std::filesystem::create_directories(rename_destination.path);
    std::ofstream(rename_destination.path / "occupied.txt") << "occupied\n";
    EXPECT_THROW(RepositoryEngine::FinishCloneForTest(rename_source.path, rename_destination.path), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(rename_source.path));
#endif
}

TEST(RepositoryEngine, DispatchesEveryMutationCommand)
{
    TemporaryRepository repository;
    RemovePath workspace{repository.path.string() + "-workspace"};
    std::filesystem::remove_all(workspace.path);
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); }), nullptr);

    std::ofstream(repository.path / "untracked.txt") << "new\n";
    const std::vector<std::pair<Command, std::string>> commands{
        {NewChange{"new change", {}, {}, {}, false}, "new"},
        {Describe{"missing", "description"}, "describe"},
        {Metaedit{"missing", "description", "Author <author@example.test>"}, "metaedit"},
        {Edit{"missing"}, "edit"},
        {MoveChange{GG_MOVE_PREVIOUS, 1, true, true}, "move"},
        {Commit{"commit", {"tracked.txt"}}, "commit"},
        {TrackPaths{{"untracked.txt"}, true}, "track paths"},
        {ChmodPaths{{"tracked.txt"}, true}, "chmod"},
        {UntrackPaths{{"tracked.txt"}}, "untrack paths"},
        {Rebase{"missing-source", "missing-destination"}, "rebase"},
        {Reorder{"missing-source", "missing-target", GG_REORDER_AFTER}, "reorder"},
        {Split{"missing", "selected", {"tracked.txt"}}, "split"},
        {Squash{"missing-source", "missing-destination", "combined"}, "squash"},
        {Abandon{{"missing"}, true, true}, "abandon"},
        {Restore{"missing-from", "missing-into", {"tracked.txt"}}, "restore"},
        {MoveFiles{"missing-source", "missing-destination", {"tracked.txt"}}, "move files"},
        {SimplifyParents{{"missing"}}, "simplify parents"},
        {Bookmark{GG_BOOKMARK_RENAME, {"missing"}, "missing", "renamed"}, "bookmark"},
        {Tag{GG_TAG_SET, {"coverage-tag"}, "missing", true}, "tag"},
        {Undo{}, "undo"},
        {Redo{}, "redo"},
        {RestoreOperation{"missing"}, "restore operation"},
        {WorkspaceAdd{workspace.path.string(), "coverage", "@", "workspace"}, "add workspace"},
        {WorkspaceForget{{"missing"}}, "forget workspace"},
        {WorkspaceRename{"coverage-renamed"}, "rename workspace"},
    };
    for (const auto& [command, operation] : commands)
        ExerciseCommand(engine, command, operation);

#ifdef GGUI_TESTING
    EXPECT_THROW(engine.DispatchMutationForTest(Refresh{}), std::runtime_error);
    const auto conflicts = RepositoryEngine::ConvertConflictsForTest();
    ASSERT_EQ(conflicts.size(), 2U);
    EXPECT_EQ(conflicts[0].path, "conflicted.txt");
    EXPECT_EQ(conflicts[0].removes, 2U);
    EXPECT_EQ(conflicts[0].adds, 3U);
    EXPECT_TRUE(conflicts[1].path.empty());
#endif
}

#ifdef GGUI_TESTING
TEST(RepositoryEngine, HandlesCredentialMethodsCancellationAndProgress)
{
    RepositoryEngine engine;
    auto acquire = [&](unsigned int allowed) {
        git_credential* credential = nullptr;
        const int result = engine.AcquireCredentialForTest(
            &credential, "ssh://example.test/repository", "git", allowed);
        if (credential != nullptr)
            git_credential_free(credential);
        return result;
    };

    engine.ResetCredentialStateForTest();
    EXPECT_EQ(acquire(GIT_CREDENTIAL_DEFAULT), GIT_OK);

#ifndef _WIN32
    const char* old_agent_socket = std::getenv("SSH_AUTH_SOCK");
    const std::string saved_agent_socket = old_agent_socket == nullptr ? "" : old_agent_socket;
    setenv("SSH_AUTH_SOCK", "/tmp/ggui-no-such-agent.sock", 1);
    engine.ResetCredentialStateForTest();
    auto automatic_agent = std::async(std::launch::async, [&] { return acquire(GIT_CREDENTIAL_SSH_KEY); });
    if (automatic_agent.wait_for(100ms) == std::future_status::ready)
        EXPECT_LE(automatic_agent.get(), GIT_OK);
    else
    {
        ASSERT_TRUE(WaitForCredentialRequest(engine).has_value());
        engine.CancelCredential();
        EXPECT_EQ(automatic_agent.get(), GIT_EUSER);
    }
    if (old_agent_socket == nullptr)
        unsetenv("SSH_AUTH_SOCK");
    else
        setenv("SSH_AUTH_SOCK", saved_agent_socket.c_str(), 1);
#endif

    engine.ResetCredentialStateForTest(true);
    auto userpass = std::async(std::launch::async, [&] { return acquire(GIT_CREDENTIAL_USERPASS_PLAINTEXT); });
    const auto userpass_request = WaitForCredentialRequest(engine);
    ASSERT_TRUE(userpass_request.has_value());
    EXPECT_EQ(userpass_request->url, "ssh://example.test/repository");
    engine.SubmitCredential({CredentialResponse::Method::UserPass, "user", "token", {}, {}});
    EXPECT_EQ(userpass.get(), GIT_OK);

    engine.ResetCredentialStateForTest(true);
    auto key = std::async(std::launch::async, [&] { return acquire(GIT_CREDENTIAL_SSH_KEY); });
    ASSERT_TRUE(WaitForCredentialRequest(engine).has_value());
    engine.SubmitCredential(
        {CredentialResponse::Method::SshKey, {}, "passphrase", "/tmp/private-key", {}});
    EXPECT_EQ(key.get(), GIT_OK);

    engine.ResetCredentialStateForTest(true);
    auto agent = std::async(std::launch::async, [&] { return acquire(GIT_CREDENTIAL_SSH_KEY); });
    ASSERT_TRUE(WaitForCredentialRequest(engine).has_value());
    engine.SubmitCredential({CredentialResponse::Method::SshAgent, {}, {}, {}, {}});
    EXPECT_LE(agent.get(), GIT_OK);

    engine.ResetCredentialStateForTest(true);
    auto cancelled = std::async(std::launch::async, [&] { return acquire(GIT_CREDENTIAL_USERPASS_PLAINTEXT); });
    ASSERT_TRUE(WaitForCredentialRequest(engine).has_value());
    engine.CancelCredential();
    EXPECT_EQ(cancelled.get(), GIT_EUSER);

    engine.ResetCredentialStateForTest(true);
    auto operation_cancelled =
        std::async(std::launch::async, [&] { return acquire(GIT_CREDENTIAL_USERPASS_PLAINTEXT); });
    ASSERT_TRUE(WaitForCredentialRequest(engine).has_value());
    engine.Cancel();
    EXPECT_EQ(operation_cancelled.get(), GIT_EUSER);

    engine.ResetCredentialStateForTest(true);
    auto unsupported = std::async(std::launch::async, [&] { return acquire(GIT_CREDENTIAL_USERNAME); });
    ASSERT_TRUE(WaitForCredentialRequest(engine).has_value());
    engine.SubmitCredential({CredentialResponse::Method::UserPass, "user", "token", {}, {}});
    EXPECT_EQ(unsupported.get(), GIT_EUSER);

    engine.ResetCredentialStateForTest(true, 3);
    EXPECT_EQ(acquire(GIT_CREDENTIAL_USERPASS_PLAINTEXT), GIT_EAUTH);

    engine.ResetCredentialStateForTest();
    git_indexer_progress progress{};
    progress.received_objects = 2;
    progress.total_objects = 5;
    EXPECT_EQ(engine.TransferProgressForTest(progress), GIT_OK);
    const auto events = engine.PollEvents();
    ASSERT_EQ(events.size(), 1U);
    const auto* reported = std::get_if<OperationProgress>(&events.front());
    ASSERT_NE(reported, nullptr);
    EXPECT_EQ(reported->phase, "clone");
    EXPECT_EQ(reported->completed, 2U);
    EXPECT_EQ(reported->total, 5U);
    engine.Cancel();
    EXPECT_EQ(engine.TransferProgressForTest(progress), GIT_EUSER);
}
#endif

} // namespace
} // namespace Ggui

// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Core/RepositoryEngine.hpp"
#include "Core/Settings.hpp"
#include "Graph/Layout.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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

std::string Quote(const std::filesystem::path& value)
{
    std::string result = "'";
    for (const char character : value.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
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
        std::filesystem::create_directory(path / "gitlink");
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

    void AppendEmptyCommits(std::size_t count)
    {
        if (git_libgit2_init() <= 0)
            throw std::runtime_error("could not initialize libgit2");
        struct ShutdownGit
        {
            ~ShutdownGit() { git_libgit2_shutdown(); }
        } shutdown_git;
        git_repository* raw_repository = nullptr;
        CheckGit(git_repository_open(&raw_repository, path.string().c_str()));
        std::unique_ptr<git_repository, decltype(&git_repository_free)> repository(
            raw_repository, git_repository_free);
        git_commit* raw_parent = nullptr;
        CheckGit(git_revparse_single(reinterpret_cast<git_object**>(&raw_parent), repository.get(), "HEAD"));
        std::unique_ptr<git_commit, decltype(&git_commit_free)> parent(raw_parent, git_commit_free);
        git_tree* raw_tree = nullptr;
        CheckGit(git_commit_tree(&raw_tree, parent.get()));
        std::unique_ptr<git_tree, decltype(&git_tree_free)> tree(raw_tree, git_tree_free);
        git_signature* raw_signature = nullptr;
        CheckGit(git_signature_now(&raw_signature, "ggui test", "ggui@example.test"));
        std::unique_ptr<git_signature, decltype(&git_signature_free)> signature(raw_signature, git_signature_free);
        for (std::size_t index = 0; index < count; ++index)
        {
            const git_commit* parents[]{parent.get()};
            git_oid oid{};
            CheckGit(git_commit_create(&oid, repository.get(), "HEAD", signature.get(), signature.get(), nullptr,
                ("history " + std::to_string(index)).c_str(), tree.get(), 1, parents));
            git_commit* raw_next = nullptr;
            CheckGit(git_commit_lookup(&raw_next, repository.get(), &oid));
            parent.reset(raw_next);
        }
    }

    std::filesystem::path path;
};

struct RemovePath
{
    ~RemovePath() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

#ifndef _WIN32
struct ScopedEnvironmentVariable
{
    ScopedEnvironmentVariable(const char* variable, const char* value) : name(variable)
    {
        if (const char* current = std::getenv(variable)) previous = current;
        setenv(variable, value, 1);
    }

    ~ScopedEnvironmentVariable()
    {
        if (previous.has_value())
            setenv(name.c_str(), previous->c_str(), 1);
        else
            unsetenv(name.c_str());
    }

    std::string name;
    std::optional<std::string> previous;
};
#endif

struct TemporaryGlobalConfig
{
    TemporaryGlobalConfig()
    {
        if (git_libgit2_init() <= 0)
            throw std::runtime_error("could not initialize libgit2");
        CheckGit(git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, &previous));
        path = std::filesystem::temp_directory_path() /
            ("ggui-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
        std::ofstream(path / ".gitconfig");
        CheckGit(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, path.string().c_str()));
    }

    ~TemporaryGlobalConfig()
    {
        git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, previous.ptr);
        git_buf_dispose(&previous);
        git_libgit2_shutdown();
        std::filesystem::remove_all(path);
    }

    git_buf previous = GIT_BUF_INIT;
    std::filesystem::path path;
};

std::shared_ptr<const RepoSnapshot> WaitForSnapshot(
    RepositoryEngine& engine, const std::function<bool(const RepoSnapshot&)>& predicate,
    bool* operation_progress = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::shared_ptr<const RepoSnapshot> last_snapshot;
    std::uint64_t requested_topology = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const Event& event : engine.PollEvents())
        {
            if (operation_progress != nullptr && std::holds_alternative<OperationProgress>(event))
                *operation_progress = true;
            if (const auto* error = std::get_if<ErrorEvent>(&event))
            {
                ADD_FAILURE() << error->operation << ": " << error->message;
                return {};
            }
            if (const auto* ready = std::get_if<SnapshotReady>(&event))
            {
                last_snapshot = ready->snapshot;
                if (requested_topology != ready->snapshot->repository_generation)
                {
                    requested_topology = ready->snapshot->repository_generation;
                    engine.Enqueue(RebuildHistory{HistoryQuery{{}, {}, {}, {}, requested_topology}});
                }
            }
            if (const auto* ready = std::get_if<HistoryReady>(&event);
                ready != nullptr && !ready->view->skeleton && last_snapshot != nullptr
                && ready->view->repository_generation == last_snapshot->repository_generation)
            {
                auto combined = std::make_shared<RepoSnapshot>(*last_snapshot);
                for (const HistoryItem& item : ready->view->items)
                    if (item.kind == HistoryItemKind::Commit) combined->revisions.push_back(item.revision);
                last_snapshot = combined;
                if (predicate(*combined)) return combined;
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

auto FindRevision(const RepoSnapshot& snapshot, std::string_view id)
{
    return std::ranges::find_if(snapshot.revisions, [&](const Revision& revision) {
        return revision.oid == id || std::ranges::find(revision.aliases, id) != revision.aliases.end();
    });
}

TEST(Settings, ParsesFileSizes)
{
    EXPECT_EQ(ParseFileSize("0"), 0U);
    EXPECT_EQ(ParseFileSize("1K"), 1024U);
    EXPECT_EQ(ParseFileSize("2KB"), 2048U);
    EXPECT_EQ(ParseFileSize("3KiB"), 3072U);
    EXPECT_EQ(ParseFileSize("2MiB"), 2U * 1024 * 1024);
    EXPECT_EQ(ParseFileSize("1GiB"), 1024U * 1024 * 1024);
    EXPECT_FALSE(ParseFileSize("").has_value());
    EXPECT_FALSE(ParseFileSize("-1").has_value());
    EXPECT_FALSE(ParseFileSize("1.5MiB").has_value());
    EXPECT_FALSE(ParseFileSize("18446744073709551615GiB").has_value());
}

TEST(Settings, ReadsWritesUnsetsAndResolvesNativeScopes)
{
    TemporaryRepository repository;
    TemporaryGlobalConfig global;
    const std::optional<std::filesystem::path> path = repository.path;
    MaxNewFileSizeValues values = ReadMaxNewFileSizeValues(path);
    EXPECT_FALSE(values[0].has_value());
    EXPECT_FALSE(values[1].has_value());
    EXPECT_FALSE(values[2].has_value());
    EXPECT_EQ(InheritedMaxNewFileSize(values, ConfigScope::User).value, "1MiB");
    EXPECT_THROW(WriteMaxNewFileSizeValue(path, ConfigScope::User, "invalid"), std::invalid_argument);

    WriteMaxNewFileSizeValue(path, ConfigScope::User, "2MiB");
    WriteMaxNewFileSizeValue(path, ConfigScope::Repository, "3MiB");
    WriteMaxNewFileSizeValue(path, ConfigScope::Workspace, "4MiB");
    values = ReadMaxNewFileSizeValues(path);
    ASSERT_EQ(values[0], "2MiB");
    ASSERT_EQ(values[1], "3MiB");
    ASSERT_EQ(values[2], "4MiB");
    const InheritedConfigValue repository_inherited =
        InheritedMaxNewFileSize(values, ConfigScope::Repository);
    EXPECT_EQ(repository_inherited.value, "2MiB");
    EXPECT_EQ(repository_inherited.source, ConfigScope::User);
    const InheritedConfigValue workspace_inherited =
        InheritedMaxNewFileSize(values, ConfigScope::Workspace);
    EXPECT_EQ(workspace_inherited.value, "3MiB");
    EXPECT_EQ(workspace_inherited.source, ConfigScope::Repository);

    git_config* raw_config = nullptr;
    CheckGit(git_config_open_ondisk(&raw_config, (repository.path / ".git/config").string().c_str()));
    std::unique_ptr<git_config, decltype(&git_config_free)> config(raw_config, git_config_free);
    int enabled = 0;
    CheckGit(git_config_get_bool(&enabled, config.get(), "extensions.worktreeConfig"));
    EXPECT_EQ(enabled, 1);

    WriteMaxNewFileSizeValue(path, ConfigScope::Workspace, std::nullopt);
    WriteMaxNewFileSizeValue(path, ConfigScope::Repository, std::nullopt);
    WriteMaxNewFileSizeValue(path, ConfigScope::User, std::nullopt);
    values = ReadMaxNewFileSizeValues(path);
    EXPECT_FALSE(values[0].has_value());
    EXPECT_FALSE(values[1].has_value());
    EXPECT_FALSE(values[2].has_value());
}

TEST(Settings, ReadsWritesAndResolvesEditorScopes)
{
    TemporaryRepository repository;
    TemporaryGlobalConfig global;
    const std::optional<std::filesystem::path> path = repository.path;
    EditorValues values = ReadEditorValues(path);
    EXPECT_TRUE(EffectiveEditor(values).empty());
    EXPECT_TRUE(InheritedEditor(values, ConfigScope::User).value.empty());

    WriteEditorValue(path, ConfigScope::User, "code --wait");
    WriteEditorValue(path, ConfigScope::Repository, "zed --wait");
    WriteEditorValue(path, ConfigScope::Workspace, "cursor --wait");
    values = ReadEditorValues(path);
    EXPECT_EQ(values[0], "code --wait");
    EXPECT_EQ(values[1], "zed --wait");
    EXPECT_EQ(values[2], "cursor --wait");
    EXPECT_EQ(InheritedEditor(values, ConfigScope::Repository).value, "code --wait");
    EXPECT_EQ(InheritedEditor(values, ConfigScope::Workspace).value, "zed --wait");
    EXPECT_EQ(EffectiveEditor(values), "cursor --wait");

    WriteEditorValue(path, ConfigScope::Workspace, std::nullopt);
    WriteEditorValue(path, ConfigScope::Repository, std::nullopt);
    WriteEditorValue(path, ConfigScope::User, std::nullopt);
    EXPECT_TRUE(EffectiveEditor(ReadEditorValues(path)).empty());
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

std::optional<FileContentReady> WaitForFileContent(RepositoryEngine& engine)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const Event& event : engine.PollEvents())
        {
            if (const auto* file = std::get_if<FileContentReady>(&event))
                return *file;
            if (const auto* error = std::get_if<ErrorEvent>(&event))
            {
                ADD_FAILURE() << error->operation << ": " << error->message;
                return std::nullopt;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    ADD_FAILURE() << "timed out waiting for file contents";
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

TEST(GraphLayout, KeepsEveryEdgeColorStableBetweenCommitDots)
{
    // The side reaches base first in display order. The later primary edge
    // adopts base's already-active track at the primary dot; it must not
    // change color halfway between primary and base.
    const std::vector<GraphNode> nodes{{"merge", {"primary", "side"}},
        {"side", {"base"}}, {"primary", {"base"}}, {"base", {}}};
    const std::vector<GraphRow> rows = BuildGraphLayout(nodes);
    ASSERT_EQ(rows.size(), 4U);
    EXPECT_EQ(rows[1].tracks_after, (std::vector<int>{0, 1}));
    EXPECT_EQ(rows[2].tracks_after, (std::vector<int>{1}));
    ASSERT_EQ(rows[2].parent_tracks.size(), 1U);
    EXPECT_EQ(rows[2].parent_tracks.front(), rows[3].track);
    for (const GraphRow& row : rows)
    {
        ASSERT_EQ(row.parent_columns.size(), row.parent_tracks.size());
        for (std::size_t parent = 0; parent < row.parent_columns.size(); ++parent)
            EXPECT_EQ(row.parent_tracks[parent], row.tracks_after[row.parent_columns[parent]]);
    }
}

TEST(GraphLayout, RejectsMissingEndpoints)
{
    EXPECT_THROW(BuildGraphLayout({{"child", {"missing"}}}), std::invalid_argument);
}

TEST(GraphLayout, RejectsParentsThatPrecedeChildren)
{
    EXPECT_THROW(BuildGraphLayout({{"parent", {}}, {"child", {"parent"}}}), std::invalid_argument);
}

TEST(GraphLayout, SupportsEveryLaneWithoutACap)
{
    std::vector<GraphNode> nodes;
    for (int index = 0; index < 64; ++index)
        nodes.push_back({"child-" + std::to_string(index), {"parent-" + std::to_string(index)}});
    for (int index = 0; index < 64; ++index)
        nodes.push_back({"parent-" + std::to_string(index), {}});
    const auto rows = BuildGraphLayout(nodes);
    EXPECT_GT(GraphColumnCount(rows), 16);
}

TEST(TextHelpers, ShortensAndSelectsFirstLine)
{
    EXPECT_EQ(ShortId("abcdefghijkl", 8), "abcdefgh");
    EXPECT_EQ(UniquePrefixLengths({"alpha", "beta"}), (std::vector<std::size_t>{1, 1}));
    EXPECT_EQ(UniquePrefixLengths({"abcdef00", "abcdef11", "xyz00000"}, 2), (std::vector<std::size_t>{7, 7, 2}));
    EXPECT_EQ(UniquePrefixLengths({"abcdef00", "abcdef00", "abcdef11"}),
        (std::vector<std::size_t>{7, 7, 7}));
    EXPECT_EQ(FirstLine("subject\nbody"), "subject");
}

TEST(RevisionHelpers, MarksRemoteAncestryAsPushed)
{
    std::vector<Revision> revisions{
        {"tip", {"root"}, {}, {}, {}, 0, false, false, false},
        {"side", {"root"}, {}, {}, {}, 0, false, false, false},
        {"root", {}, {}, {}, {}, 0, false, false, false},
    };
    MarkPushedRevisions(revisions,
        {{"main", "origin", "tip", GG_NAMED_REF_REMOTE_BOOKMARK, true, false},
            {"release", "origin", "tip", GG_NAMED_REF_REMOTE_TAG, true, false},
            {"missing", "origin", "absent", GG_NAMED_REF_REMOTE_BOOKMARK, true, false}});
    EXPECT_TRUE(revisions[0].pushed);
    EXPECT_FALSE(revisions[1].pushed);
    EXPECT_TRUE(revisions[2].pushed);
}

TEST(RevisionHelpers, KeepsUnchangedAncestorsLockedAcrossARewrite)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt") << "child\n";
    const std::string commit = "git -C " + Quote(repository.path) + " add tracked.txt && git -C "
        + Quote(repository.path) + " commit -m child >/dev/null 2>&1";
    ASSERT_EQ(std::system(commit.c_str()), 0);

    git_repository* raw_repository = nullptr;
    CheckGit(git_repository_open(&raw_repository, repository.path.string().c_str()));
    std::unique_ptr<git_repository, decltype(&git_repository_free)> git(raw_repository, git_repository_free);
    git_object* raw_tip = nullptr;
    git_object* raw_root = nullptr;
    CheckGit(git_revparse_single(&raw_tip, git.get(), "HEAD"));
    CheckGit(git_revparse_single(&raw_root, git.get(), "HEAD^"));
    std::unique_ptr<git_object, decltype(&git_object_free)> tip(raw_tip, git_object_free);
    std::unique_ptr<git_object, decltype(&git_object_free)> root(raw_root, git_object_free);
    const auto oid = [](const git_object* object)
    {
        std::array<char, GIT_OID_MAX_HEXSIZE + 1> value{};
        git_oid_tostr(value.data(), value.size(), git_object_id(object));
        return std::string(value.data());
    };

    std::vector<Revision> revisions{{oid(root.get()), {}, {}, {}, {}, 0, false, false, false}};
    MarkPushedRevisions(
        revisions, {{"main", "origin", oid(tip.get()), GG_NAMED_REF_REMOTE_BOOKMARK, true, false}}, git.get());
    EXPECT_TRUE(revisions.front().pushed);
}

TEST(RepositoryEngine, OpensAndAutomaticallyRefreshesARepository)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); });
    ASSERT_NE(opened, nullptr);
    EXPECT_EQ(opened->revisions.back().description, "base");
    EXPECT_TRUE(opened->working_copy.empty());
    EXPECT_EQ(opened->head, opened->revisions.back().oid);
    EXPECT_EQ(opened->revisions.back().author_email, "ggui@example.test");
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
    const auto empty_change = std::ranges::find(working->revisions, working->working_copy, &Revision::oid);
    ASSERT_NE(empty_change, working->revisions.end());
    EXPECT_TRUE(empty_change->empty);

    engine.Enqueue(NewChange{{}, {"@"}, {}, {}, false});
    const auto child = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > working->generation && snapshot.working_copy != working->working_copy;
    });
    ASSERT_NE(child, nullptr);
    engine.Enqueue(Abandon{{working->working_copy}, true, false, {}});
    const auto replacement = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > child->generation
            && std::ranges::none_of(snapshot.revisions,
                [&](const Revision& revision) { return revision.oid == working->working_copy; });
    });
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->revisions.size(), working->revisions.size());
    const auto replacement_change =
        std::ranges::find(replacement->revisions, replacement->working_copy, &Revision::oid);
    ASSERT_NE(replacement_change, replacement->revisions.end());
    EXPECT_TRUE(replacement_change->empty);
    EXPECT_EQ(replacement_change->parents, empty_change->parents);

    std::ofstream(repository.path / "tracked.txt") << "changed\n";
    bool automatic_progress = false;
    const auto refreshed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > replacement->generation && !snapshot.status.empty();
    }, &automatic_progress);
    ASSERT_NE(refreshed, nullptr);
    EXPECT_FALSE(automatic_progress);
    const auto nonempty_change = std::ranges::find(refreshed->revisions, refreshed->working_copy, &Revision::oid);
    ASSERT_NE(nonempty_change, refreshed->revisions.end());
    EXPECT_FALSE(nonempty_change->empty);
    EXPECT_EQ(refreshed->status.front().path, "tracked.txt");
    EXPECT_EQ(refreshed->status.front().status, GIT_DELTA_MODIFIED);
}

TEST(RepositoryEngine, PublishesProgressiveConnectedHistoryView)
{
    TemporaryRepository repository;
    for (int index = 0; index < 300; ++index)
    {
        std::ofstream(repository.path / "tracked.txt") << index << '\n';
        const std::string message = index == 150 ? "old-description" : "history-" + std::to_string(index);
        ASSERT_EQ(std::system(("git -C " + Quote(repository.path)
            + " add tracked.txt && git -C " + Quote(repository.path)
            + " commit -m " + message + " >/dev/null 2>&1").c_str()), 0);
    }
    ASSERT_EQ(std::system(("git -C " + Quote(repository.path)
        + " update-ref refs/remotes/origin/main HEAD").c_str()), 0);
    ASSERT_EQ(std::system(("git -C " + Quote(repository.path) + " tag old HEAD~290").c_str()), 0);
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto snapshot = WaitForSnapshot(engine, [](const RepoSnapshot& value) { return !value.root.empty(); });
    ASSERT_NE(snapshot, nullptr);
    engine.Enqueue(RebuildHistory{HistoryQuery{{}, {"old"}, {}, {}, snapshot->repository_generation}});

    bool saw_skeleton = false;
    std::shared_ptr<const HistoryView> detail;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline && detail == nullptr)
    {
        for (const Event& event : engine.PollEvents())
            if (const auto* ready = std::get_if<HistoryReady>(&event))
            {
                if (ready->view->skeleton) saw_skeleton = true;
                else detail = ready->view;
            }
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(saw_skeleton);
    ASSERT_NE(detail, nullptr);
    EXPECT_EQ(detail->repository_generation, snapshot->repository_generation);
    EXPECT_LE(std::ranges::count_if(detail->items, [](const HistoryItem& item) {
        return item.kind == HistoryItemKind::Commit;
    }), 258);
    EXPECT_TRUE(std::ranges::any_of(detail->items, [](const HistoryItem& item) {
        return item.kind == HistoryItemKind::CollapsedRegion;
    }));
    EXPECT_TRUE(std::ranges::all_of(detail->items, [](const HistoryItem& item) {
        return item.kind != HistoryItemKind::Commit || item.revision.pushed;
    }));
    std::unordered_set<std::string> ids;
    for (const HistoryItem& item : detail->items) ids.insert(item.id);
    for (const HistoryItem& item : detail->items)
        for (const std::string& parent : item.parents) EXPECT_TRUE(ids.contains(parent));
    std::unordered_map<std::string, std::vector<std::string>> neighbors;
    for (const HistoryItem& item : detail->items)
        for (const std::string& parent : item.parents)
        {
            neighbors[item.id].push_back(parent);
            neighbors[parent].push_back(item.id);
        }
    std::vector<std::string> pending{detail->items.front().id};
    std::unordered_set<std::string> connected;
    while (!pending.empty())
    {
        const std::string current = std::move(pending.back());
        pending.pop_back();
        if (!connected.insert(current).second) continue;
        pending.insert(pending.end(), neighbors[current].begin(), neighbors[current].end());
    }
    EXPECT_EQ(connected.size(), detail->items.size());

    // A ref/ID hit is already a complete direct search result. Do not launch
    // the repository-wide description scan (which would also match the older
    // commit named "old-description").
    engine.Enqueue(RebuildHistory{HistoryQuery{{}, {"old"}, {}, "old", snapshot->repository_generation}});
    std::shared_ptr<const HistoryView> direct_search;
    const auto direct_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < direct_deadline && direct_search == nullptr)
    {
        for (const Event& event : engine.PollEvents())
            if (const auto* ready = std::get_if<HistoryReady>(&event);
                ready != nullptr && !ready->view->skeleton && ready->view->request > detail->request)
                direct_search = ready->view;
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_NE(direct_search, nullptr);
    EXPECT_EQ(std::ranges::count_if(direct_search->items,
        [](const HistoryItem& item) { return item.search_match; }), 1);

    engine.Enqueue(RebuildHistory{HistoryQuery{{}, {"old"}, {}, "history-0", snapshot->repository_generation}});
    std::shared_ptr<const HistoryView> searched;
    const auto search_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < search_deadline && searched == nullptr)
    {
        for (const Event& event : engine.PollEvents())
            if (const auto* ready = std::get_if<HistoryReady>(&event);
                ready != nullptr && !ready->view->skeleton && ready->view->request > direct_search->request)
                searched = ready->view;
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_NE(searched, nullptr);
    EXPECT_TRUE(std::ranges::any_of(searched->items, [](const HistoryItem& item) {
        return item.kind == HistoryItemKind::Commit && item.search_match
            && item.revision.description.find("history-0") != std::string::npos;
    }));
    ids.clear();
    for (const HistoryItem& item : searched->items) ids.insert(item.id);
    for (const HistoryItem& item : searched->items)
        for (const std::string& parent : item.parents) EXPECT_TRUE(ids.contains(parent));

    const auto region = std::ranges::find_if(searched->items, [](const HistoryItem& item) {
        return item.kind == HistoryItemKind::CollapsedRegion;
    });
    ASSERT_NE(region, searched->items.end());
    const auto searched_commits = std::ranges::count_if(searched->items, [](const HistoryItem& item) {
        return item.kind == HistoryItemKind::Commit;
    });
    engine.Enqueue(ExpandHistoryRegion{region->id});
    std::shared_ptr<const HistoryView> expanded;
    const auto expand_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < expand_deadline && expanded == nullptr)
    {
        for (const Event& event : engine.PollEvents())
            if (const auto* ready = std::get_if<HistoryReady>(&event);
                ready != nullptr && !ready->view->skeleton && ready->view->request > searched->request)
                expanded = ready->view;
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_NE(expanded, nullptr);
    const auto expanded_commits = std::ranges::count_if(expanded->items, [](const HistoryItem& item) {
        return item.kind == HistoryItemKind::Commit;
    });
    EXPECT_GT(expanded_commits, searched_commits);
    EXPECT_LE(expanded_commits, searched_commits + 128);
}

TEST(RepositoryEngine, PublishesClosestBookmarkAsynchronously)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});

    std::uint64_t topology = 0;
    bool saw_detail = false;
    std::optional<ClosestBookmarkReady> closest;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline && (!saw_detail || !closest.has_value()))
    {
        for (const Event& event : engine.PollEvents())
        {
            if (const auto* error = std::get_if<ErrorEvent>(&event))
                FAIL() << error->operation << ": " << error->message;
            if (const auto* ready = std::get_if<SnapshotReady>(&event))
            {
                topology = ready->snapshot->repository_generation;
                engine.Enqueue(RebuildHistory{HistoryQuery{{}, {}, {}, {}, topology}});
            }
            else if (const auto* ready = std::get_if<HistoryReady>(&event);
                ready != nullptr && !ready->view->skeleton
                    && ready->view->repository_generation == topology)
                saw_detail = true;
            else if (const auto* ready = std::get_if<ClosestBookmarkReady>(&event))
                closest = *ready;
        }
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_NE(topology, 0U);
    EXPECT_TRUE(saw_detail);
    ASSERT_TRUE(closest.has_value());
    EXPECT_EQ(closest->repository_generation, topology);
    EXPECT_EQ(closest->label, "main");
}

TEST(RepositoryEngine, OpensRepositoryWithTagPointingToTree)
{
    TemporaryRepository repository;
    ASSERT_EQ(std::system(("git -C " + Quote(repository.path) +
                              " tag -a tree-only -m tree-only 'HEAD^{tree}' >/dev/null 2>&1")
                              .c_str()),
        0);

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened =
        WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); });
    ASSERT_NE(opened, nullptr);
    EXPECT_TRUE(std::ranges::none_of(opened->refs,
        [](const NamedRef& reference) { return reference.name == "tree-only"; }));
}

TEST(RepositoryEngine, SnapshotsPendingFilesBeforeCreatingAChange)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); }), nullptr);

    engine.Enqueue(NewChange{});
    const auto first = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.working_copy.empty();
    });
    ASSERT_NE(first, nullptr);
    const auto first_revision = std::ranges::find(first->revisions, first->working_copy, &Revision::oid);
    ASSERT_NE(first_revision, first->revisions.end());
    ASSERT_TRUE(first_revision->empty);

    std::ofstream(repository.path / "tracked.txt") << "changed\n";
    engine.Enqueue(NewChange{{}, {"@"}, {}, {}, false});
    const auto child = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        const auto working = std::ranges::find(snapshot.revisions, snapshot.working_copy, &Revision::oid);
        if (snapshot.generation <= first->generation || snapshot.working_copy == first->working_copy
            || working == snapshot.revisions.end() || !working->empty || working->parents.size() != 1)
            return false;
        const auto parent = std::ranges::find(snapshot.revisions, working->parents.front(), &Revision::oid);
        return parent != snapshot.revisions.end() && !parent->empty;
    });
    ASSERT_NE(child, nullptr);
    const auto child_revision = std::ranges::find(child->revisions, child->working_copy, &Revision::oid);
    ASSERT_NE(child_revision, child->revisions.end());
    ASSERT_TRUE(child_revision->empty);
    ASSERT_EQ(child_revision->parents.size(), 1U);

    const auto parent = std::ranges::find(child->revisions, child_revision->parents.front(), &Revision::oid);
    ASSERT_NE(parent, child->revisions.end());
    EXPECT_FALSE(parent->empty);

    engine.Enqueue(LoadDiff{parent->oid, "tracked.txt"});
    const auto diff = WaitForDiff(engine);
    ASSERT_TRUE(diff.has_value());
    EXPECT_EQ(diff->before, "base\n");
    EXPECT_EQ(diff->after, "changed\n");
}

TEST(RepositoryEngine, ImportsDirtyGitWorkingTreeOnOpen)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt") << "modified\n";
    std::ofstream(repository.path / "untracked.txt") << "new\n";

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.working_copy.empty() && snapshot.status.size() == 2;
    });
    ASSERT_NE(opened, nullptr);
    const auto modified = std::ranges::find(opened->status, "tracked.txt", &StatusEntry::path);
    const auto added = std::ranges::find(opened->status, "untracked.txt", &StatusEntry::path);
    ASSERT_NE(modified, opened->status.end());
    ASSERT_NE(added, opened->status.end());
    EXPECT_EQ(modified->status, GIT_DELTA_MODIFIED);
    EXPECT_EQ(added->status, GIT_DELTA_ADDED);

    engine.Enqueue(LoadDiff{opened->working_copy, {}, true});
    const auto diff = WaitForDiff(engine);
    ASSERT_TRUE(diff.has_value());
    EXPECT_NE(std::ranges::find(diff->files, "tracked.txt", &StatusEntry::path), diff->files.end());
    EXPECT_NE(std::ranges::find(diff->files, "untracked.txt", &StatusEntry::path), diff->files.end());
}

TEST(RepositoryEngine, ShowsAndExplicitlyTracksOversizedFiles)
{
    TemporaryRepository repository;
    ASSERT_EQ(std::system(("git -C " + Quote(repository.path)
                             + " config snapshot.max-new-file-size 1")
                              .c_str()),
        0);
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); }), nullptr);
    engine.Enqueue(NewChange{});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.working_copy.empty(); });
    ASSERT_NE(opened, nullptr);

    std::ofstream(repository.path / "oversized.txt") << "xx";
    engine.Enqueue(Refresh{});
    const auto untracked_snapshot = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation
            && std::ranges::any_of(snapshot.status, [](const StatusEntry& entry) {
                   return entry.path == "oversized.txt" && entry.status == GIT_DELTA_UNTRACKED;
               });
    });
    ASSERT_NE(untracked_snapshot, nullptr);

    engine.Enqueue(LoadDiff{untracked_snapshot->working_copy, "oversized.txt"});
    const auto diff = WaitForDiff(engine);
    ASSERT_TRUE(diff.has_value());
    const auto untracked = std::ranges::find(diff->files, "oversized.txt", &StatusEntry::path);
    ASSERT_NE(untracked, diff->files.end());
    EXPECT_EQ(untracked->status, GIT_DELTA_UNTRACKED);

    engine.Enqueue(TrackPaths{{"oversized.txt"}});
    const auto tracked = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > untracked_snapshot->generation
            && std::ranges::any_of(snapshot.status, [](const StatusEntry& entry) {
                   return entry.path == "oversized.txt" && entry.status == GIT_DELTA_ADDED;
               });
    });
    ASSERT_NE(tracked, nullptr);
}

TEST(RepositoryEngine, ShowsRenamedFilesAsSingleChange)
{
    TemporaryRepository repository;
    std::filesystem::rename(repository.path / "tracked.txt", repository.path / "renamed.txt");

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.working_copy.empty()
            && std::ranges::any_of(snapshot.status, [](const StatusEntry& entry) {
                return entry.status == GIT_DELTA_RENAMED;
            });
    });
    ASSERT_NE(opened, nullptr);

    engine.Enqueue(LoadDiff{opened->working_copy, "renamed.txt"});
    const auto diff = WaitForDiff(engine);
    ASSERT_TRUE(diff.has_value());
    ASSERT_EQ(diff->files.size(), 1U);
    EXPECT_EQ(diff->files.front().status, GIT_DELTA_RENAMED);
    EXPECT_EQ(diff->files.front().old_path, "tracked.txt");
    EXPECT_EQ(diff->files.front().path, "renamed.txt");
    EXPECT_EQ(diff->before, "base\n");
    EXPECT_EQ(diff->after, "base\n");
}

TEST(RepositoryEngine, PushesAndFetchesLocalRemotes)
{
    TemporaryRepository repository;
    RemovePath remote{repository.path.string() + "-bare"};
    git_repository* raw_remote = nullptr;
    CheckGit(git_repository_init(&raw_remote, remote.path.string().c_str(), 1));
    git_repository_free(raw_remote);

    git_repository* raw_repository = nullptr;
    CheckGit(git_repository_open(&raw_repository, repository.path.string().c_str()));
    std::unique_ptr<git_repository, decltype(&git_repository_free)> local(raw_repository, git_repository_free);
    CheckGit(git_remote_set_url(local.get(), "origin", remote.path.string().c_str()));
    local.reset();

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); }), nullptr);

    engine.Enqueue(Push{"main", "origin"});
    EXPECT_TRUE(WaitForTerminal(engine, "push").finished);
    CheckGit(git_repository_open_bare(&raw_remote, remote.path.string().c_str()));
    std::unique_ptr<git_repository, decltype(&git_repository_free)> bare(raw_remote, git_repository_free);
    git_reference* raw_main = nullptr;
    EXPECT_EQ(git_reference_lookup(&raw_main, bare.get(), "refs/heads/main"), GIT_OK);
    git_reference_free(raw_main);
    bare.reset();

    engine.Enqueue(Fetch{"origin", false});
    EXPECT_TRUE(WaitForTerminal(engine, "fetch").finished);
    engine.Enqueue(Fetch{"origin", true});
    EXPECT_TRUE(WaitForTerminal(engine, "pull").finished);

    engine.Enqueue(RemoteBookmarkDelete{"main", "origin"});
    EXPECT_TRUE(WaitForTerminal(engine, "delete remote bookmark").finished);
    CheckGit(git_repository_open_bare(&raw_remote, remote.path.string().c_str()));
    bare.reset(raw_remote);
    raw_main = nullptr;
    EXPECT_EQ(git_reference_lookup(&raw_main, bare.get(), "refs/heads/main"), GIT_ENOTFOUND);

    engine.Enqueue(AddRemote{"backup", remote.path.string()});
    const auto added = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return std::ranges::any_of(snapshot.remotes, [](const Remote& candidate) { return candidate.name == "backup"; });
    });
    ASSERT_NE(added, nullptr);

    engine.Enqueue(DeleteRemote{"backup"});
    const auto deleted = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return std::ranges::none_of(snapshot.remotes, [](const Remote& candidate) { return candidate.name == "backup"; });
    });
    ASSERT_NE(deleted, nullptr);
}

TEST(RepositoryEngine, FetchShowsRemoteCommitsAndPullMovesLocalBookmark)
{
    TemporaryRepository repository;
    RemovePath remote{repository.path.string() + "-fetch-bare"};
    RemovePath peer{repository.path.string() + "-fetch-peer"};
    ASSERT_EQ(std::system(("git clone --bare " + Quote(repository.path) + " " + Quote(remote.path)
                             + " >/dev/null 2>&1 && git -C " + Quote(repository.path)
                             + " remote set-url origin " + Quote(remote.path) + " && git clone "
                             + Quote(remote.path) + " " + Quote(peer.path)
                             + " >/dev/null 2>&1 && git -C " + Quote(peer.path)
                             + " config user.name peer && git -C " + Quote(peer.path)
                             + " config user.email peer@example.test")
                              .c_str()),
        0);

    std::ofstream(peer.path / "remote.txt") << "remote\n";
    ASSERT_EQ(std::system(("git -C " + Quote(peer.path) + " add remote.txt && git -C "
                             + Quote(peer.path) + " commit -m remote >/dev/null 2>&1 && git -C "
                             + Quote(peer.path) + " push origin main >/dev/null 2>&1")
                              .c_str()),
        0);

    const auto bookmark_target = [](const RepoSnapshot& snapshot, gg_named_ref_kind kind) {
        const auto ref = std::ranges::find_if(snapshot.refs, [&](const NamedRef& candidate) {
            return candidate.kind == kind && candidate.name == "main"
                && (kind != GG_NAMED_REF_REMOTE_BOOKMARK || candidate.remote == "origin");
        });
        return ref == snapshot.refs.end() ? std::string{} : ref->target;
    };

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.revisions.empty();
    });
    ASSERT_NE(opened, nullptr);
    const std::string local_before = bookmark_target(*opened, GG_NAMED_REF_LOCAL_BOOKMARK);
    ASSERT_FALSE(local_before.empty());
    EXPECT_TRUE(std::ranges::none_of(
        opened->revisions, [](const Revision& revision) { return revision.description == "remote"; }));

    engine.Enqueue(Fetch{"origin", false});
    const auto fetched = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        const std::string remote_target = bookmark_target(snapshot, GG_NAMED_REF_REMOTE_BOOKMARK);
        return snapshot.generation > opened->generation && !remote_target.empty()
            && std::ranges::any_of(snapshot.revisions,
                [&](const Revision& revision) { return revision.oid == remote_target; });
    });
    ASSERT_NE(fetched, nullptr);
    EXPECT_EQ(bookmark_target(*fetched, GG_NAMED_REF_LOCAL_BOOKMARK), local_before);
    const std::string remote_after_fetch = bookmark_target(*fetched, GG_NAMED_REF_REMOTE_BOOKMARK);
    EXPECT_NE(remote_after_fetch, local_before);

    engine.Enqueue(Tag{GG_TAG_SET, {"after-fetch"}, local_before});
    const auto synchronized = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > fetched->generation
            && std::ranges::any_of(snapshot.refs, [](const NamedRef& ref) {
                return ref.kind == GG_NAMED_REF_LOCAL_TAG && ref.name == "after-fetch";
            });
    });
    ASSERT_NE(synchronized, nullptr);
    EXPECT_EQ(bookmark_target(*synchronized, GG_NAMED_REF_LOCAL_BOOKMARK), local_before);

    engine.Enqueue(Fetch{"origin", true});
    const auto pulled = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > synchronized->generation
            && bookmark_target(snapshot, GG_NAMED_REF_LOCAL_BOOKMARK) == remote_after_fetch;
    });
    ASSERT_NE(pulled, nullptr);
}

TEST(RepositoryEngine, ForcePushesDivergedBookmark)
{
    TemporaryRepository repository;
    RemovePath remote{repository.path.string() + "-force-bare"};
    RemovePath peer{repository.path.string() + "-force-peer"};
    ASSERT_EQ(std::system(("git clone --bare " + Quote(repository.path) + " " + Quote(remote.path)
                             + " >/dev/null 2>&1 && git -C " + Quote(repository.path)
                             + " remote set-url origin " + Quote(remote.path) + " && git clone "
                             + Quote(remote.path) + " " + Quote(peer.path)
                             + " >/dev/null 2>&1 && git -C " + Quote(peer.path)
                             + " config user.name peer && git -C " + Quote(peer.path)
                             + " config user.email peer@example.test")
                              .c_str()),
        0);

    std::ofstream(peer.path / "remote.txt") << "remote\n";
    ASSERT_EQ(std::system(("git -C " + Quote(peer.path) + " add remote.txt && git -C "
                             + Quote(peer.path) + " commit -m remote >/dev/null 2>&1 && git -C "
                             + Quote(peer.path) + " push origin main >/dev/null 2>&1")
                              .c_str()),
        0);
    std::ofstream(repository.path / "local.txt") << "local\n";
    ASSERT_EQ(std::system(("git -C " + Quote(repository.path) + " add local.txt && git -C "
                             + Quote(repository.path) + " commit -m local >/dev/null 2>&1")
                              .c_str()),
        0);

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); }), nullptr);

    engine.Enqueue(Push{"main", "origin"});
    EXPECT_FALSE(WaitForTerminal(engine, "push").finished);
    engine.Enqueue(Push{"main", "origin", true});
    EXPECT_TRUE(WaitForTerminal(engine, "push").finished);

    git_repository* raw_local = nullptr;
    git_repository* raw_remote = nullptr;
    CheckGit(git_repository_open(&raw_local, repository.path.string().c_str()));
    CheckGit(git_repository_open_bare(&raw_remote, remote.path.string().c_str()));
    std::unique_ptr<git_repository, decltype(&git_repository_free)> local(raw_local, git_repository_free);
    std::unique_ptr<git_repository, decltype(&git_repository_free)> bare(raw_remote, git_repository_free);
    git_oid local_oid{}, remote_oid{};
    CheckGit(git_reference_name_to_id(&local_oid, local.get(), "refs/heads/main"));
    CheckGit(git_reference_name_to_id(&remote_oid, bare.get(), "refs/heads/main"));
    EXPECT_NE(git_oid_equal(&local_oid, &remote_oid), 0);
}

TEST(RepositoryEngine, ReconcilesDivergedBookmarkAndPushesNormally)
{
    TemporaryRepository repository;
    RemovePath remote{repository.path.string() + "-reconcile-bare"};
    RemovePath peer{repository.path.string() + "-reconcile-peer"};
    ASSERT_EQ(std::system(("git clone --bare " + Quote(repository.path) + " " + Quote(remote.path)
                             + " >/dev/null 2>&1")
                              .c_str()),
        0);
    ASSERT_EQ(std::system(("git -C " + Quote(repository.path) + " remote set-url origin " + Quote(remote.path)
                             + " && git -C " + Quote(repository.path) + " fetch origin >/dev/null 2>&1")
                              .c_str()),
        0);
    ASSERT_EQ(std::system(("git clone " + Quote(remote.path) + " " + Quote(peer.path)
                             + " >/dev/null 2>&1 && git -C " + Quote(peer.path)
                             + " config user.name peer && git -C " + Quote(peer.path)
                             + " config user.email peer@example.test")
                              .c_str()),
        0);
    std::ofstream(peer.path / "remote.txt") << "remote\n";
    ASSERT_EQ(std::system(("git -C " + Quote(peer.path) + " add remote.txt && git -C " + Quote(peer.path)
                             + " commit -m remote >/dev/null 2>&1 && git -C " + Quote(peer.path)
                             + " push origin main >/dev/null 2>&1")
                              .c_str()),
        0);
    std::ofstream(repository.path / "local.txt") << "local\n";
    ASSERT_EQ(std::system(("git -C " + Quote(repository.path)
                             + " add local.txt && git -C " + Quote(repository.path)
                             + " commit -m local >/dev/null 2>&1")
                              .c_str()),
        0);

    const auto relation = [](const RepoSnapshot& snapshot) {
        const auto local = std::ranges::find_if(snapshot.refs, [](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == "main";
        });
        const auto remote_ref = std::ranges::find_if(snapshot.refs, [](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.name == "main" && ref.remote == "origin";
        });
        return local == snapshot.refs.end() || remote_ref == snapshot.refs.end()
            ? BookmarkRelation::Unavailable
            : ClassifyBookmarkRelation(snapshot, local->target, remote_ref->target);
    };
    const auto tips = [](const RepoSnapshot& snapshot) {
        std::pair<std::string, std::string> result;
        for (const NamedRef& ref : snapshot.refs)
        {
            if (ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == "main")
                result.first = ref.target;
            if (ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.name == "main" && ref.remote == "origin")
                result.second = ref.target;
        }
        return result;
    };

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return relation(snapshot) == BookmarkRelation::LocalAhead;
    });
    ASSERT_NE(opened, nullptr);
    engine.Enqueue(Fetch{"origin", true});
    const auto diverged = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && relation(snapshot) == BookmarkRelation::Diverged;
    });
    ASSERT_NE(diverged, nullptr);
    const auto [local_tip, remote_tip] = tips(*diverged);
    ASSERT_FALSE(local_tip.empty());
    ASSERT_FALSE(remote_tip.empty());

    engine.Enqueue(Rebase{local_tip, remote_tip, true});
    const auto reconciled = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > diverged->generation && relation(snapshot) == BookmarkRelation::LocalAhead;
    });
    ASSERT_NE(reconciled, nullptr);
    ASSERT_TRUE(reconciled->can_undo);

    engine.Enqueue(Undo{});
    const auto undone = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > reconciled->generation && relation(snapshot) == BookmarkRelation::Diverged;
    });
    ASSERT_NE(undone, nullptr);
    engine.Enqueue(Redo{});
    ASSERT_NE(WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > undone->generation && relation(snapshot) == BookmarkRelation::LocalAhead;
    }), nullptr);

    engine.Enqueue(Push{"main", "origin"});
    EXPECT_TRUE(WaitForTerminal(engine, "push").finished);
    git_repository* raw_local = nullptr;
    git_repository* raw_remote = nullptr;
    CheckGit(git_repository_open(&raw_local, repository.path.string().c_str()));
    CheckGit(git_repository_open_bare(&raw_remote, remote.path.string().c_str()));
    std::unique_ptr<git_repository, decltype(&git_repository_free)> local_repo(raw_local, git_repository_free);
    std::unique_ptr<git_repository, decltype(&git_repository_free)> remote_repo(raw_remote, git_repository_free);
    git_oid local_oid{}, remote_oid{};
    CheckGit(git_reference_name_to_id(&local_oid, local_repo.get(), "refs/heads/main"));
    CheckGit(git_reference_name_to_id(&remote_oid, remote_repo.get(), "refs/heads/main"));
    EXPECT_NE(git_oid_equal(&local_oid, &remote_oid), 0);
}

TEST(RepositoryEngine, ReconciliationKeepsLogicalConflictsLocalAndBlocksPush)
{
    TemporaryRepository repository;
    RemovePath remote{repository.path.string() + "-conflict-bare"};
    RemovePath peer{repository.path.string() + "-conflict-peer"};
    ASSERT_EQ(std::system(("git clone --bare " + Quote(repository.path) + " " + Quote(remote.path)
                             + " >/dev/null 2>&1 && git -C " + Quote(repository.path)
                             + " remote set-url origin " + Quote(remote.path) + " && git -C "
                             + Quote(repository.path) + " fetch origin >/dev/null 2>&1 && git clone "
                             + Quote(remote.path) + " " + Quote(peer.path) + " >/dev/null 2>&1 && git -C "
                             + Quote(peer.path) + " config user.name peer && git -C " + Quote(peer.path)
                             + " config user.email peer@example.test")
                              .c_str()),
        0);
    std::ofstream(peer.path / "tracked.txt") << "remote\n";
    ASSERT_EQ(std::system(("git -C " + Quote(peer.path) + " add tracked.txt && git -C " + Quote(peer.path)
                             + " commit -m remote >/dev/null 2>&1 && git -C " + Quote(peer.path)
                             + " push origin main >/dev/null 2>&1")
                              .c_str()),
        0);
    std::ofstream(repository.path / "tracked.txt") << "local\n";
    ASSERT_EQ(std::system(("git -C " + Quote(repository.path)
                             + " add tracked.txt && git -C " + Quote(repository.path)
                             + " commit -m local >/dev/null 2>&1")
                              .c_str()),
        0);

    const auto refs = [](const RepoSnapshot& snapshot) {
        std::pair<std::string, std::string> result;
        for (const NamedRef& ref : snapshot.refs)
        {
            if (ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == "main")
                result.first = ref.target;
            if (ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.name == "main" && ref.remote == "origin")
                result.second = ref.target;
        }
        return result;
    };

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        const auto [local, remote_ref] = refs(snapshot);
        return !local.empty() && !remote_ref.empty();
    });
    ASSERT_NE(opened, nullptr);
    engine.Enqueue(Fetch{"origin", true});
    const auto diverged = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        const auto [local, remote_ref] = refs(snapshot);
        return snapshot.generation > opened->generation && !local.empty() && !remote_ref.empty()
            && ClassifyBookmarkRelation(snapshot, local, remote_ref) == BookmarkRelation::Diverged;
    });
    ASSERT_NE(diverged, nullptr);
    const auto [local_tip, remote_tip] = refs(*diverged);
    const auto local_revision = FindRevision(*diverged, local_tip);
    ASSERT_NE(local_revision, diverged->revisions.end());
    ASSERT_FALSE(local_revision->parents.empty());
    const std::string local_base = local_revision->parents.front();

    engine.Enqueue(Rebase{local_tip, remote_tip, true});
    const auto conflicted = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > diverged->generation
            && std::ranges::any_of(snapshot.revisions, [](const Revision& revision) {
                   return revision.conflicted;
               });
    });
    ASSERT_NE(conflicted, nullptr);
    const auto conflict_revision = std::ranges::find_if(conflicted->revisions, [](const Revision& revision) {
        return revision.conflicted;
    });
    ASSERT_NE(conflict_revision, conflicted->revisions.end());
    const std::string conflicted_source = conflict_revision->oid;
    engine.Enqueue(LoadDiff{conflicted_source, "tracked.txt"});
    const auto conflict_diff = WaitForDiff(engine);
    ASSERT_TRUE(conflict_diff.has_value());
    const auto conflict_file = std::ranges::find(conflict_diff->files, "tracked.txt", &StatusEntry::path);
    ASSERT_NE(conflict_file, conflict_diff->files.end());
    EXPECT_TRUE(conflict_file->conflicted);
    EXPECT_NE(conflict_diff->after.find("<<<<<<< Conflict"), std::string::npos);
    engine.Enqueue(Push{"main", "origin"});
    const TerminalEvent push = WaitForTerminal(engine, "push");
    EXPECT_FALSE(push.finished);
    EXPECT_NE(push.message.find("conflict"), std::string::npos);

    engine.Enqueue(NewChange{"alternate destination", {remote_tip}, {}, {}, false});
    const auto alternate = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > conflicted->generation && snapshot.working_copy != conflicted_source
            && std::ranges::any_of(snapshot.revisions, [](const Revision& revision) {
                   return revision.description == "alternate destination";
               });
    });
    ASSERT_NE(alternate, nullptr);
    std::ofstream(repository.path / "tracked.txt") << "alternate\n";
    engine.Enqueue(Refresh{});
    const auto alternate_changed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > alternate->generation && !snapshot.status.empty();
    });
    ASSERT_NE(alternate_changed, nullptr);
    const std::string alternate_id = alternate_changed->working_copy;

    engine.Enqueue(Edit{conflicted_source});
    const auto conflict_checked_out = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= alternate_changed->generation)
            return false;
        const auto revision = FindRevision(snapshot, conflicted_source);
        return revision != snapshot.revisions.end() && revision->conflicted
            && snapshot.working_copy == revision->oid;
    });
    ASSERT_NE(conflict_checked_out, nullptr);

    std::ifstream marker_input(repository.path / "tracked.txt", std::ios::binary);
    const std::string markers{
        std::istreambuf_iterator<char>(marker_input), std::istreambuf_iterator<char>()};
    ASSERT_NE(markers.find("<<<<<<< Conflict"), std::string::npos);
    std::ofstream(repository.path / "tracked.txt", std::ios::binary | std::ios::trunc)
        << "partial resolution\n" << markers;
    engine.Enqueue(Refresh{});
    const auto marker_retained = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > conflict_checked_out->generation
            && std::ranges::any_of(snapshot.status, [](const StatusEntry& entry) {
                   return entry.path == "tracked.txt" && entry.conflicted;
               });
    });
    ASSERT_NE(marker_retained, nullptr);

    engine.Enqueue(Rebase{conflicted_source, local_base});
    const auto graph_preserved = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= marker_retained->generation)
            return false;
        const auto revision = FindRevision(snapshot, conflicted_source);
        return revision != snapshot.revisions.end() && revision->conflicted
            && snapshot.working_copy == revision->oid;
    });
    ASSERT_NE(graph_preserved, nullptr);

    engine.Enqueue(Rebase{conflicted_source, alternate_id});
    const auto conflict_rebased = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= graph_preserved->generation)
            return false;
        const auto revision = FindRevision(snapshot, conflicted_source);
        return revision != snapshot.revisions.end() && revision->conflicted
            && snapshot.working_copy == revision->oid
            && std::ranges::any_of(snapshot.status, [](const StatusEntry& entry) {
                   return entry.path == "tracked.txt" && entry.conflicted;
               });
    });
    ASSERT_NE(conflict_rebased, nullptr);
    engine.Enqueue(LoadDiff{conflict_rebased->working_copy, "tracked.txt"});
    const auto rebased_conflict_diff = WaitForDiff(engine);
    ASSERT_TRUE(rebased_conflict_diff.has_value());
    const auto rebased_conflict_file =
        std::ranges::find(rebased_conflict_diff->files, "tracked.txt", &StatusEntry::path);
    ASSERT_NE(rebased_conflict_file, rebased_conflict_diff->files.end());
    EXPECT_TRUE(rebased_conflict_file->conflicted);
    EXPECT_NE(rebased_conflict_diff->after.find("<<<<<<< Conflict"), std::string::npos);

    const auto rebased_source = FindRevision(*conflict_rebased, conflicted_source);
    ASSERT_NE(rebased_source, conflict_rebased->revisions.end());
    const std::string rebased_source_id = rebased_source->oid;
    std::ifstream binary_before(repository.path / "binary.dat", std::ios::binary);
    const std::string preserved_binary{
        std::istreambuf_iterator<char>(binary_before), std::istreambuf_iterator<char>()};
    engine.Enqueue(NewChange{"resolution child", {rebased_source_id}, {}, {}, false});
    const auto resolution_child = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > conflict_rebased->generation && snapshot.working_copy != rebased_source_id
            && std::ranges::any_of(snapshot.revisions, [](const Revision& revision) {
                   return revision.description == "resolution child";
               });
    });
    ASSERT_NE(resolution_child, nullptr);

    engine.Enqueue(ResolveConflict{rebased_source_id, "tracked.txt", "resolved\n", true});
    const auto resolved = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= resolution_child->generation)
            return false;
        const auto revision = FindRevision(snapshot, rebased_source_id);
        return revision != snapshot.revisions.end() && !revision->conflicted
            && std::ranges::none_of(snapshot.status, [](const StatusEntry& entry) { return entry.conflicted; });
    });
    ASSERT_NE(resolved, nullptr);
    std::ifstream resolved_file(repository.path / "tracked.txt", std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(resolved_file), std::istreambuf_iterator<char>()),
        "resolved\n");
    std::ifstream binary_after(repository.path / "binary.dat", std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(binary_after), std::istreambuf_iterator<char>()),
        preserved_binary);
}

TEST(RepositoryEngine, OpensLinkedWorktree)
{
    TemporaryRepository repository;
    RemovePath worktree{repository.path.string() + "-worktree"};
    const std::string command =
        "git -C " + Quote(repository.path) + " worktree add --detach " + Quote(worktree.path) + " >/dev/null 2>&1";
    ASSERT_EQ(std::system(command.c_str()), 0);

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{worktree.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); });
    ASSERT_NE(opened, nullptr);
    EXPECT_EQ(std::filesystem::weakly_canonical(opened->root), std::filesystem::weakly_canonical(worktree.path));
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
    EXPECT_FALSE(diff->binary);
    EXPECT_FALSE(diff->patch.empty());
    EXPECT_EQ(diff->patch.find("binary.dat"), std::string::npos);
    EXPECT_TRUE(diff->old_oid.empty());
    EXPECT_FALSE(diff->new_oid.empty());
    EXPECT_EQ(diff->old_mode, 0U);
    EXPECT_EQ(diff->new_mode, GIT_FILEMODE_BLOB);

    engine.Enqueue(LoadDiff{root.oid, {}, true});
    const auto fallback = WaitForDiff(engine);
    ASSERT_TRUE(fallback.has_value());
    ASSERT_FALSE(fallback->files.empty());
    EXPECT_EQ(fallback->path, fallback->files.front().path);

    engine.Enqueue(LoadDiff{root.oid, "tracked.txt", true});
    const auto preferred = WaitForDiff(engine);
    ASSERT_TRUE(preferred.has_value());
    EXPECT_EQ(preferred->path, "tracked.txt");
    EXPECT_EQ(preferred->after, "base\n");

    engine.Enqueue(LoadDiff{root.oid, "binary.dat"});
    engine.Enqueue(LoadDiff{root.oid, "missing.txt"});
    engine.Enqueue(LoadDiff{root.oid, "tracked.txt"});
    std::optional<DiffResult> latest;
    for (int attempt = 0; attempt < 3 && (!latest.has_value() || latest->path != "tracked.txt"); ++attempt)
        latest = WaitForDiff(engine);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->path, "tracked.txt");

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

TEST(RepositoryEngine, LoadsFileContentFromARevision)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto snapshot = WaitForSnapshot(engine, [](const RepoSnapshot& value) {
        return !value.revisions.empty();
    });
    ASSERT_NE(snapshot, nullptr);
    const Revision& root = snapshot->revisions.back();

    engine.Enqueue(LoadFileContent{root.oid, "tracked.txt"});
    const std::optional<FileContentReady> file = WaitForFileContent(engine);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->revision, root.oid);
    EXPECT_EQ(file->path, "tracked.txt");
    EXPECT_EQ(file->contents, "base\n");
}

TEST(RepositoryEngine, ComparesTwoRevisionTrees)
{
    TemporaryRepository repository;
    std::filesystem::rename(repository.path / "tracked.txt", repository.path / "renamed.txt");
    std::ofstream(repository.path / "added.txt") << "after\n";
    const std::string commit = "git -C " + Quote(repository.path) + " add -A && git -C "
        + Quote(repository.path) + " commit -m comparison >/dev/null 2>&1";
    ASSERT_EQ(std::system(commit.c_str()), 0);

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto snapshot = WaitForSnapshot(engine, [](const RepoSnapshot& value) { return value.revisions.size() >= 2; });
    ASSERT_NE(snapshot, nullptr);
    const auto comparison = std::ranges::find_if(snapshot->revisions,
        [](const Revision& revision) { return !revision.parents.empty(); });
    ASSERT_NE(comparison, snapshot->revisions.end());
    const auto base = std::ranges::find(snapshot->revisions, comparison->parents.front(), &Revision::oid);
    ASSERT_NE(base, snapshot->revisions.end());

    const DiffOptions options{DiffWhitespaceMode::IgnoreAllWhitespace, 0};
    engine.Enqueue(LoadDiff{base->oid, "renamed.txt", false, options, comparison->oid});
    const auto renamed = WaitForDiff(engine);
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(renamed->revision, base->oid);
    EXPECT_EQ(renamed->compare_to, comparison->oid);
    EXPECT_EQ(renamed->options.whitespace_mode, DiffWhitespaceMode::IgnoreAllWhitespace);
    EXPECT_EQ(renamed->options.context_lines, 0);
    const auto renamed_file = std::ranges::find(renamed->files, "renamed.txt", &StatusEntry::path);
    ASSERT_NE(renamed_file, renamed->files.end());
    EXPECT_EQ(renamed_file->status, GIT_DELTA_RENAMED);
    EXPECT_EQ(renamed_file->old_path, "tracked.txt");
    EXPECT_EQ(renamed->before, "base\n");
    EXPECT_EQ(renamed->after, "base\n");
    EXPECT_FALSE(renamed->patch.empty());
    EXPECT_FALSE(renamed->old_oid.empty());
    EXPECT_EQ(renamed->old_oid, renamed->new_oid);
    EXPECT_EQ(renamed->old_mode, GIT_FILEMODE_BLOB);
    EXPECT_EQ(renamed->new_mode, GIT_FILEMODE_BLOB);

    engine.Enqueue(LoadDiff{base->oid, "added.txt", false, options, comparison->oid});
    const auto added = WaitForDiff(engine);
    ASSERT_TRUE(added.has_value());
    EXPECT_TRUE(added->before.empty());
    EXPECT_EQ(added->after, "after\n");
    const auto added_file = std::ranges::find(added->files, "added.txt", &StatusEntry::path);
    ASSERT_NE(added_file, added->files.end());
    EXPECT_EQ(added_file->status, GIT_DELTA_ADDED);
    EXPECT_NE(added->patch.find("+after"), std::string::npos);

    engine.Enqueue(LoadDiff{base->oid, "tracked.txt", false, options, comparison->oid, true});
    const auto file_comparison = WaitForDiff(engine);
    ASSERT_TRUE(file_comparison.has_value());
    EXPECT_TRUE(file_comparison->file_comparison);
    EXPECT_EQ(file_comparison->compare_to, comparison->oid);
    const auto tracked_file = std::ranges::find(file_comparison->files, "tracked.txt", &StatusEntry::path);
    ASSERT_NE(tracked_file, file_comparison->files.end());
    EXPECT_EQ(tracked_file->status, GIT_DELTA_ADDED);
    EXPECT_EQ(file_comparison->selected_status, GIT_DELTA_RENAMED);
    EXPECT_EQ(std::ranges::find(file_comparison->files, "renamed.txt", &StatusEntry::path),
        file_comparison->files.end());
    EXPECT_EQ(file_comparison->before, "base\n");
    EXPECT_EQ(file_comparison->after, "base\n");
    EXPECT_FALSE(file_comparison->patch.empty());

    engine.Enqueue(LoadDiff{base->oid, "tracked.txt", false, options, base->oid, true});
    const auto identical = WaitForDiff(engine);
    ASSERT_TRUE(identical.has_value());
    EXPECT_EQ(identical->selected_status, GIT_DELTA_UNMODIFIED);
    EXPECT_EQ(identical->before, identical->after);
    EXPECT_TRUE(identical->patch.empty());
}

TEST(RepositoryEngine, ClosesAndReopensWithoutWatcherEvents)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& value) { return !value.revisions.empty(); });
    ASSERT_NE(opened, nullptr);
    engine.Enqueue(LoadDiff{opened->revisions.back().oid, "tracked.txt"});
    engine.Enqueue(CloseRepository{});
    EXPECT_TRUE(WaitForTerminal(engine, "close").finished);

    std::ofstream(repository.path / "tracked.txt") << "changed while closed\n";
    std::this_thread::sleep_for(200ms);
    const std::vector<Event> detached_events = engine.PollEvents();
    EXPECT_TRUE(std::ranges::none_of(detached_events, [](const Event& event) {
        return std::holds_alternative<SnapshotReady>(event) || std::holds_alternative<DiffReady>(event);
    }));

    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto reopened = WaitForSnapshot(engine, [](const RepoSnapshot& value) {
        return !value.working_copy.empty() && !value.status.empty();
    });
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(std::filesystem::weakly_canonical(reopened->root), std::filesystem::weakly_canonical(repository.path));
}

TEST(RepositoryEngine, RenamesBookmarksWithoutOverwriting)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& value) { return !value.revisions.empty(); });
    ASSERT_NE(opened, nullptr);
    const std::string revision = opened->revisions.back().oid;

    engine.Enqueue(Bookmark{GG_BOOKMARK_CREATE, {"taken"}, revision, {}});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& value) {
        return std::ranges::any_of(value.refs, [](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == "taken";
        });
    }), nullptr);
    engine.Enqueue(Bookmark{GG_BOOKMARK_RENAME, {"main"}, {}, "renamed"});
    const auto renamed = WaitForSnapshot(engine, [](const RepoSnapshot& value) {
        return std::ranges::any_of(value.refs, [](const NamedRef& ref) {
                   return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == "renamed";
               })
            && std::ranges::none_of(value.refs, [](const NamedRef& ref) {
                return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == "main";
            });
    });
    ASSERT_NE(renamed, nullptr);

    engine.Enqueue(Bookmark{GG_BOOKMARK_RENAME, {"renamed"}, {}, "taken"});
    const TerminalEvent conflict = WaitForTerminal(engine, "bookmark");
    EXPECT_FALSE(conflict.finished);
    EXPECT_NE(conflict.message.find("already exists"), std::string::npos);
}

TEST(RepositoryEngine, MovesBookmarksBackwardsOnlyWhenAllowed)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt") << "child\n";
    const std::string commit = "git -C " + Quote(repository.path) + " add tracked.txt && git -C "
        + Quote(repository.path) + " commit -m child >/dev/null 2>&1";
    ASSERT_EQ(std::system(commit.c_str()), 0);

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& value) { return value.revisions.size() >= 2; });
    ASSERT_NE(opened, nullptr);
    const auto main = std::ranges::find_if(opened->refs, [](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == "main";
    });
    ASSERT_NE(main, opened->refs.end());
    const auto tip = std::ranges::find(opened->revisions, main->target, &Revision::oid);
    ASSERT_NE(tip, opened->revisions.end());
    ASSERT_FALSE(tip->parents.empty());
    const std::string parent = tip->parents.front();

    engine.Enqueue(Bookmark{GG_BOOKMARK_MOVE, {"main"}, parent, {}});
    const TerminalEvent rejected = WaitForTerminal(engine, "bookmark");
    EXPECT_FALSE(rejected.finished);
    EXPECT_NE(rejected.message.find("refusing to move bookmark"), std::string::npos);

    engine.Enqueue(Bookmark{GG_BOOKMARK_MOVE, {"main"}, parent, {}, true});
    const auto moved = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return std::ranges::any_of(snapshot.refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == "main" && ref.target == parent;
        });
    });
    ASSERT_NE(moved, nullptr);
}

TEST(RepositoryEngine, HonorsDiffWhitespaceAndContextOptions)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt")
        << "line01\nline02\nline03\nline04\nline05\nline06\nline07\nline08\nline09\nline10\n";
    const std::string commit = "git -C " + Quote(repository.path) + " add tracked.txt && git -C "
        + Quote(repository.path) + " commit -m context >/dev/null 2>&1";
    ASSERT_EQ(std::system(commit.c_str()), 0);
    std::ofstream(repository.path / "tracked.txt")
        << "line01\nline02\nchanged\nline04\nline05\nline06\nline07\nline 08\nline09\nline10\n";

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine,
        [](const RepoSnapshot& snapshot) {
            return !snapshot.working_copy.empty() && !snapshot.status.empty() && !snapshot.revisions.empty();
        });
    ASSERT_NE(opened, nullptr);

    engine.Enqueue(LoadDiff{opened->working_copy, "tracked.txt", false,
        DiffOptions{.whitespace_mode = DiffWhitespaceMode::Normal, .context_lines = 0}});
    const auto normal = WaitForDiff(engine);
    ASSERT_TRUE(normal.has_value());
    EXPECT_EQ(normal->before, "line03\nline08\n");
    EXPECT_EQ(normal->after, "changed\nline 08\n");
    EXPECT_EQ(normal->full_before,
        "line01\nline02\nline03\nline04\nline05\nline06\nline07\nline08\nline09\nline10\n");
    EXPECT_EQ(normal->full_after,
        "line01\nline02\nchanged\nline04\nline05\nline06\nline07\nline 08\nline09\nline10\n");
    EXPECT_FALSE(normal->patch.empty());
    ASSERT_GE(normal->lines.size(), 4U);
    EXPECT_EQ(normal->lines[0].old_line, 2);
    EXPECT_EQ(normal->lines[1].new_line, 2);
    EXPECT_EQ(normal->lines[2].old_line, 7);
    EXPECT_EQ(normal->lines[3].new_line, 7);

    engine.Enqueue(LoadDiff{opened->working_copy, "tracked.txt", false,
        DiffOptions{.whitespace_mode = DiffWhitespaceMode::IgnoreAllWhitespace, .context_lines = 0}});
    const auto filtered = WaitForDiff(engine);
    ASSERT_TRUE(filtered.has_value());
    EXPECT_EQ(filtered->before, "line03\n");
    EXPECT_EQ(filtered->after, "changed\n");

    const auto working_copy = std::ranges::find(opened->revisions, opened->working_copy, &Revision::oid);
    ASSERT_NE(working_copy, opened->revisions.end());
    ASSERT_EQ(working_copy->parents.size(), 1U);
    engine.Enqueue(LoadDiff{working_copy->parents.front(), "tracked.txt", false,
        DiffOptions{.whitespace_mode = DiffWhitespaceMode::Normal, .context_lines = 0}, opened->working_copy, true});
    const auto compared = WaitForDiff(engine);
    ASSERT_TRUE(compared.has_value());
    EXPECT_TRUE(compared->file_comparison);
    EXPECT_EQ(compared->before, "line03\nline08\n");
    EXPECT_EQ(compared->after, "changed\nline 08\n");

    engine.Enqueue(LoadDiff{working_copy->parents.front(), "tracked.txt", false,
        DiffOptions{.whitespace_mode = DiffWhitespaceMode::Normal, .context_lines = -1}, opened->working_copy, true});
    const auto full = WaitForDiff(engine);
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(full->before,
        "line01\nline02\nline03\nline04\nline05\nline06\nline07\nline08\nline09\nline10\n");
    EXPECT_EQ(full->after,
        "line01\nline02\nchanged\nline04\nline05\nline06\nline07\nline 08\nline09\nline10\n");
}

TEST(RepositoryEngine, MovesSelectedDiffLinesBetweenAdjacentChanges)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt") << "one\ntwo\nthree\n";
    const std::string commit = "git -C " + Quote(repository.path) + " add tracked.txt && git -C "
        + Quote(repository.path) + " commit -m lines >/dev/null 2>&1";
    ASSERT_EQ(std::system(commit.c_str()), 0);
    std::ofstream(repository.path / "tracked.txt") << "ONE\ntwo\nTHREE\n";

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine,
        [](const RepoSnapshot& snapshot) {
            return !snapshot.working_copy.empty() && !snapshot.status.empty() && !snapshot.revisions.empty();
        });
    ASSERT_NE(opened, nullptr);
    const auto source = std::ranges::find(opened->revisions, opened->working_copy, &Revision::oid);
    ASSERT_NE(source, opened->revisions.end());
    const std::string source_id = source->oid;

    engine.Enqueue(NewChange{{}, {"@"}, {}, {}, false});
    const auto child = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && snapshot.working_copy != opened->working_copy;
    });
    ASSERT_NE(child, nullptr);
    const auto child_revision = std::ranges::find(child->revisions, child->working_copy, &Revision::oid);
    ASSERT_NE(child_revision, child->revisions.end());
    const std::string child_id = child_revision->oid;

    engine.Enqueue(LoadDiff{source->oid, "tracked.txt", false,
        DiffOptions{.whitespace_mode = DiffWhitespaceMode::Normal, .context_lines = -1}});
    const auto source_diff = WaitForDiff(engine);
    ASSERT_TRUE(source_diff.has_value());
    std::vector<DiffLine> first_change;
    std::ranges::copy_if(source_diff->lines, std::back_inserter(first_change), [](const DiffLine& line) {
        return (line.kind == DiffLineKind::Deletion && line.old_line == 0)
            || (line.kind == DiffLineKind::Addition && line.new_line == 0);
    });
    ASSERT_EQ(first_change.size(), 2U);

    engine.Enqueue(MoveDiffLines{source->oid, child_revision->oid, "tracked.txt", first_change});
    const auto moved_to_child = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > child->generation
            && std::ranges::none_of(snapshot.revisions, [&](const Revision& revision) {
                   return revision.oid == source->oid || revision.oid == child_revision->oid;
               });
    });
    ASSERT_NE(moved_to_child, nullptr);
    const auto rewritten_source = FindRevision(*moved_to_child, source_id);
    const auto rewritten_child = FindRevision(*moved_to_child, child_id);
    ASSERT_NE(rewritten_source, moved_to_child->revisions.end());
    ASSERT_NE(rewritten_child, moved_to_child->revisions.end());

    engine.Enqueue(LoadDiff{rewritten_source->oid, "tracked.txt"});
    const auto source_after_move = WaitForDiff(engine);
    ASSERT_TRUE(source_after_move.has_value());
    EXPECT_EQ(source_after_move->before, "one\ntwo\nthree\n");
    EXPECT_EQ(source_after_move->after, "one\ntwo\nTHREE\n");
    engine.Enqueue(LoadDiff{rewritten_child->oid, "tracked.txt"});
    const auto child_after_move = WaitForDiff(engine);
    ASSERT_TRUE(child_after_move.has_value());
    EXPECT_EQ(child_after_move->before, "one\ntwo\nTHREE\n");
    EXPECT_EQ(child_after_move->after, "ONE\ntwo\nTHREE\n");

    std::vector<DiffLine> moved_change;
    std::ranges::copy_if(child_after_move->lines, std::back_inserter(moved_change), [](const DiffLine& line) {
        return line.kind == DiffLineKind::Addition || line.kind == DiffLineKind::Deletion;
    });
    ASSERT_EQ(moved_change.size(), 2U);
    engine.Enqueue(MoveDiffLines{
        rewritten_child->oid, rewritten_source->oid, "tracked.txt", std::move(moved_change)});
    const auto moved_to_parent = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > moved_to_child->generation
            && std::ranges::none_of(snapshot.revisions, [&](const Revision& revision) {
                   return revision.oid == rewritten_source->oid || revision.oid == rewritten_child->oid;
               });
    });
    ASSERT_NE(moved_to_parent, nullptr);
    const auto restored_source = FindRevision(*moved_to_parent, source_id);
    const auto emptied_child = FindRevision(*moved_to_parent, child_id);
    ASSERT_NE(restored_source, moved_to_parent->revisions.end());
    ASSERT_NE(emptied_child, moved_to_parent->revisions.end());
    engine.Enqueue(LoadDiff{restored_source->oid, "tracked.txt"});
    const auto source_after_return = WaitForDiff(engine);
    ASSERT_TRUE(source_after_return.has_value());
    EXPECT_EQ(source_after_return->after, "ONE\ntwo\nTHREE\n");
    engine.Enqueue(LoadDiff{emptied_child->oid, "tracked.txt"});
    const auto child_after_return = WaitForDiff(engine);
    ASSERT_TRUE(child_after_return.has_value());
    EXPECT_TRUE(child_after_return->patch.empty());
}

TEST(RepositoryEngine, RebasesAndSquashesEntireBranchFromDivergence)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.revisions.empty();
    });
    ASSERT_NE(opened, nullptr);
    const auto base = std::ranges::find(opened->revisions, "base", &Revision::description);
    ASSERT_NE(base, opened->revisions.end());

    engine.Enqueue(NewChange{"branch root", {base->oid}, {}, {}, false});
    const auto root_snapshot = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation
            && std::ranges::any_of(snapshot.revisions, [](const Revision& revision) {
                   return revision.description == "branch root";
               });
    });
    ASSERT_NE(root_snapshot, nullptr);
    const auto root = std::ranges::find(root_snapshot->revisions, "branch root", &Revision::description);
    ASSERT_NE(root, root_snapshot->revisions.end());
    const std::string root_id = root->oid;

    engine.Enqueue(NewChange{"branch tip", {"@"}, {}, {}, false});
    const auto tip_snapshot = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > root_snapshot->generation
            && std::ranges::any_of(snapshot.revisions, [](const Revision& revision) {
                   return revision.description == "branch tip";
               });
    });
    ASSERT_NE(tip_snapshot, nullptr);
    const auto tip = std::ranges::find(tip_snapshot->revisions, "branch tip", &Revision::description);
    ASSERT_NE(tip, tip_snapshot->revisions.end());
    const std::string tip_id = tip->oid;

    engine.Enqueue(NewChange{"destination", {base->oid}, {}, {}, false});
    const auto destination_snapshot = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > tip_snapshot->generation
            && std::ranges::any_of(snapshot.revisions, [](const Revision& revision) {
                   return revision.description == "destination";
               });
    });
    ASSERT_NE(destination_snapshot, nullptr);
    const auto destination =
        std::ranges::find(destination_snapshot->revisions, "destination", &Revision::description);
    ASSERT_NE(destination, destination_snapshot->revisions.end());
    const std::string destination_id = destination->oid;

    engine.Enqueue(Edit{tip_id});
    const auto checked_out = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > destination_snapshot->generation && snapshot.working_copy == tip_id;
    });
    ASSERT_NE(checked_out, nullptr);
    EXPECT_TRUE(std::ranges::none_of(checked_out->refs, [&](const NamedRef& ref) {
        return ref.target == checked_out->working_copy;
    }));

    engine.Enqueue(Rebase{tip_id, destination_id});
    const auto single_rebased = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= checked_out->generation)
            return false;
        const auto unchanged_root = FindRevision(snapshot, root_id);
        const auto rewritten_tip = FindRevision(snapshot, tip_id);
        const auto target = FindRevision(snapshot, destination_id);
        return unchanged_root != snapshot.revisions.end() && rewritten_tip != snapshot.revisions.end()
            && target != snapshot.revisions.end() && unchanged_root->parents == std::vector{base->oid}
            && rewritten_tip->parents == std::vector{target->oid};
    });
    ASSERT_NE(single_rebased, nullptr);

    engine.Enqueue(Undo{});
    const auto restored = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= single_rebased->generation)
            return false;
        const auto restored_root = FindRevision(snapshot, root_id);
        const auto restored_tip = FindRevision(snapshot, tip_id);
        return restored_root != snapshot.revisions.end() && restored_tip != snapshot.revisions.end()
            && restored_root->parents == std::vector{base->oid}
            && restored_tip->parents == std::vector{restored_root->oid};
    });
    ASSERT_NE(restored, nullptr);

    engine.Enqueue(Rebase{tip_id, destination_id, true});
    const auto rebased = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= restored->generation)
            return false;
        const auto rewritten_root = FindRevision(snapshot, root_id);
        const auto rewritten_tip = FindRevision(snapshot, tip_id);
        const auto target = FindRevision(snapshot, destination_id);
        return rewritten_root != snapshot.revisions.end() && rewritten_tip != snapshot.revisions.end()
            && target != snapshot.revisions.end() && rewritten_root->parents == std::vector{target->oid}
            && rewritten_tip->parents == std::vector{rewritten_root->oid};
    });
    ASSERT_NE(rebased, nullptr);

    engine.Enqueue(Undo{});
    const auto squash_restored = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= rebased->generation)
            return false;
        const auto restored_root = FindRevision(snapshot, root_id);
        const auto restored_tip = FindRevision(snapshot, tip_id);
        return restored_root != snapshot.revisions.end() && restored_tip != snapshot.revisions.end()
            && restored_root->parents == std::vector{base->oid}
            && restored_tip->parents == std::vector{restored_root->oid};
    });
    ASSERT_NE(squash_restored, nullptr);

    engine.Enqueue(Squash{tip_id, destination_id, {}, true});
    const auto squashed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= squash_restored->generation)
            return false;
        const auto root_alias = FindRevision(snapshot, root_id);
        const auto tip_alias = FindRevision(snapshot, tip_id);
        const auto destination_alias = FindRevision(snapshot, destination_id);
        return root_alias != snapshot.revisions.end() && tip_alias != snapshot.revisions.end()
            && destination_alias != snapshot.revisions.end() && root_alias->oid == tip_alias->oid
            && tip_alias->oid == destination_alias->oid && destination_alias->parents == std::vector{base->oid};
    });
    ASSERT_NE(squashed, nullptr);
}

TEST(RepositoryEngine, RevertsSelectedWorkingCopyDiffLines)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt") << "one\ntwo\nthree\n";
    const std::string commit = "git -C " + Quote(repository.path) + " add tracked.txt && git -C "
        + Quote(repository.path) + " commit -m lines >/dev/null 2>&1";
    ASSERT_EQ(std::system(commit.c_str()), 0);
    std::ofstream(repository.path / "tracked.txt") << "zero\none\ntwo\nTHREE\n";

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine,
        [](const RepoSnapshot& snapshot) {
            return !snapshot.working_copy.empty() && !snapshot.status.empty() && !snapshot.revisions.empty();
        });
    ASSERT_NE(opened, nullptr);
    const auto working = std::ranges::find(opened->revisions, opened->working_copy, &Revision::oid);
    ASSERT_NE(working, opened->revisions.end());
    const std::string working_id = working->oid;
    ASSERT_EQ(working->parents.size(), 1U);
    engine.Enqueue(RevertDiffLines{
        working->parents.front(), "tracked.txt", {{DiffLineKind::Addition, -1, 0, 0}}});
    const TerminalEvent rejected = WaitForTerminal(engine, "revert diff lines");
    EXPECT_FALSE(rejected.finished);
    EXPECT_NE(rejected.message.find("working copy"), std::string::npos);

    engine.Enqueue(LoadDiff{working->oid, "tracked.txt", false,
        DiffOptions{.whitespace_mode = DiffWhitespaceMode::Normal, .context_lines = 0}});
    const auto initial = WaitForDiff(engine);
    ASSERT_TRUE(initial.has_value());
    const auto inserted = std::ranges::find_if(initial->lines, [](const DiffLine& line) {
        return line.kind == DiffLineKind::Addition && line.new_line == 0;
    });
    ASSERT_NE(inserted, initial->lines.end());

    engine.Enqueue(RevertDiffLines{working->oid, "tracked.txt", {*inserted}});
    const auto line_reverted = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && snapshot.working_copy != working->oid;
    });
    ASSERT_NE(line_reverted, nullptr);
    const auto rewritten = FindRevision(*line_reverted, working_id);
    ASSERT_NE(rewritten, line_reverted->revisions.end());
    engine.Enqueue(LoadDiff{rewritten->oid, "tracked.txt", false,
        DiffOptions{.whitespace_mode = DiffWhitespaceMode::Normal, .context_lines = -1}});
    const auto after_line = WaitForDiff(engine);
    ASSERT_TRUE(after_line.has_value());
    EXPECT_EQ(after_line->after, "one\ntwo\nTHREE\n");

    std::vector<DiffLine> hunk;
    std::ranges::copy_if(after_line->lines, std::back_inserter(hunk), [](const DiffLine& line) {
        return line.kind != DiffLineKind::Context;
    });
    ASSERT_EQ(hunk.size(), 2U);
    engine.Enqueue(RevertDiffLines{rewritten->oid, "tracked.txt", std::move(hunk)});
    const auto hunk_reverted = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > line_reverted->generation && snapshot.working_copy != rewritten->oid;
    });
    ASSERT_NE(hunk_reverted, nullptr);
    engine.Enqueue(LoadDiff{hunk_reverted->working_copy, "tracked.txt"});
    const auto clean = WaitForDiff(engine);
    ASSERT_TRUE(clean.has_value());
    EXPECT_TRUE(clean->patch.empty());
}

TEST(RepositoryEngine, RevertsASelectedChangeFileOntoWorkingCopy)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt") << "one\n2\n3\n4\n5\n6\n7\n8\n9\nten\n";
    std::ofstream(repository.path / "old.txt") << "renamed\n";
    const std::string commit = "git -C " + Quote(repository.path) + " add tracked.txt old.txt && git -C "
        + Quote(repository.path) + " commit -m files >/dev/null 2>&1";
    ASSERT_EQ(std::system(commit.c_str()), 0);

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); }), nullptr);
    engine.Enqueue(NewChange{"selected change", {}, {}, {}, false});
    const auto selected = WaitForSnapshot(engine,
        [](const RepoSnapshot& snapshot) { return !snapshot.working_copy.empty(); });
    ASSERT_NE(selected, nullptr);
    std::ofstream(repository.path / "tracked.txt") << "ONE\n2\n3\n4\n5\n6\n7\n8\n9\nten\n";
    std::filesystem::rename(repository.path / "old.txt", repository.path / "new.txt");
    engine.Enqueue(Refresh{});
    const auto changed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > selected->generation && snapshot.status.size() == 2;
    });
    ASSERT_NE(changed, nullptr);
    const auto source = std::ranges::find(changed->revisions, changed->working_copy, &Revision::oid);
    ASSERT_NE(source, changed->revisions.end());
    const std::string source_id = source->oid;

    engine.Enqueue(NewChange{"later change", {"@"}, {}, {}, false});
    const auto child = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > changed->generation && snapshot.working_copy != changed->working_copy;
    });
    ASSERT_NE(child, nullptr);
    std::ofstream(repository.path / "tracked.txt") << "ONE\n2\n3\n4\n5\n6\n7\n8\n9\nTEN\n";
    engine.Enqueue(Refresh{});
    const auto later = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > child->generation && !snapshot.status.empty();
    });
    ASSERT_NE(later, nullptr);

    engine.Enqueue(RevertFile{source_id, {}, "missing.txt", {}});
    const TerminalEvent missing = WaitForTerminal(engine, "revert file");
    EXPECT_FALSE(missing.finished);
    EXPECT_NE(missing.message.find("no changes"), std::string::npos);

    engine.Enqueue(RevertFile{source_id, "tracked.txt", "tracked.txt", {}});
    const auto reverted = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > later->generation;
    });
    ASSERT_NE(reverted, nullptr);
    std::ifstream tracked(repository.path / "tracked.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(tracked), std::istreambuf_iterator<char>()),
        "one\n2\n3\n4\n5\n6\n7\n8\n9\nTEN\n");

    engine.Enqueue(RevertFile{source_id, "old.txt", "new.txt", {}});
    ASSERT_NE(WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > reverted->generation;
    }), nullptr);
    EXPECT_TRUE(std::filesystem::exists(repository.path / "old.txt"));
    EXPECT_FALSE(std::filesystem::exists(repository.path / "new.txt"));
}

TEST(RepositoryEngine, RevertsSelectedHunksOntoWorkingCopy)
{
    TemporaryRepository repository;
    const auto contents = [](std::string first, std::string fifth) {
        std::string result = std::move(first) + "\n";
        for (int line = 2; line <= 20; ++line)
            result += (line == 5 ? std::move(fifth) : std::to_string(line)) + "\n";
        return result;
    };
    std::ofstream(repository.path / "tracked.txt") << contents("one", "5");
    const std::string commit = "git -C " + Quote(repository.path)
        + " add tracked.txt && git -C " + Quote(repository.path) + " commit -m expanded >/dev/null 2>&1";
    ASSERT_EQ(std::system(commit.c_str()), 0);

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); }), nullptr);
    engine.Enqueue(NewChange{"selected hunks", {}, {}, {}, false});
    const auto selected = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.working_copy.empty();
    });
    ASSERT_NE(selected, nullptr);
    std::ofstream(repository.path / "tracked.txt") << contents("ONE", "FIVE");
    engine.Enqueue(Refresh{});
    const auto changed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > selected->generation && !snapshot.status.empty();
    });
    ASSERT_NE(changed, nullptr);
    const std::string source = changed->working_copy;
    engine.Enqueue(LoadDiff{source, "tracked.txt", false,
        DiffOptions{.whitespace_mode = DiffWhitespaceMode::Normal, .context_lines = 0}});
    const auto diff = WaitForDiff(engine);
    ASSERT_TRUE(diff.has_value());
    std::vector<DiffLine> first_hunk;
    std::ranges::copy_if(diff->lines, std::back_inserter(first_hunk), [](const DiffLine& line) {
        return line.hunk == 0 && line.kind != DiffLineKind::Context;
    });
    ASSERT_FALSE(first_hunk.empty());
    ASSERT_TRUE(std::ranges::any_of(diff->lines, [](const DiffLine& line) {
        return line.hunk == 1 && line.kind != DiffLineKind::Context;
    }));

    engine.Enqueue(NewChange{"working child", {"@"}, {}, {}, false});
    const auto child = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > changed->generation && snapshot.working_copy != source;
    });
    ASSERT_NE(child, nullptr);
    engine.Enqueue(RevertFile{source, "tracked.txt", "tracked.txt", first_hunk});
    const TerminalEvent reverted = WaitForTerminal(engine, "revert file");
    ASSERT_TRUE(reverted.finished) << reverted.message;
    std::ifstream tracked(repository.path / "tracked.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(tracked), std::istreambuf_iterator<char>()),
        contents("one", "FIVE"));

    engine.Enqueue(RevertFile{source, "tracked.txt", "tracked.txt",
        {{DiffLineKind::Addition, -1, 9999, 0}}});
    const TerminalEvent stale = WaitForTerminal(engine, "revert file");
    EXPECT_FALSE(stale.finished);
    EXPECT_NE(stale.message.find("no longer matches"), std::string::npos);
}

TEST(RepositoryEngine, DeletesWorkingCopyFiles)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); });
    ASSERT_NE(opened, nullptr);

    engine.Enqueue(DeleteFile{"missing.txt"});
    EXPECT_FALSE(WaitForTerminal(engine, "delete file").finished);
    engine.Enqueue(DeleteFile{"."});
    EXPECT_FALSE(WaitForTerminal(engine, "delete file").finished);

    engine.Enqueue(DeleteFile{"tracked.txt"});
    const auto deleted = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation
            && std::ranges::any_of(snapshot.status, [](const StatusEntry& file) {
                   return file.path == "tracked.txt" && file.status == GIT_DELTA_DELETED;
               });
    });
    ASSERT_NE(deleted, nullptr);
    EXPECT_FALSE(std::filesystem::exists(repository.path / "tracked.txt"));
}

TEST(RepositoryEngine, AppliesPatchTextAndFilesToWorkingCopy)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "untracked.txt") << "new\n";
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    ASSERT_NE(
        WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.working_copy.empty(); }), nullptr);

    const std::string first_patch =
        "diff --git a/tracked.txt b/tracked.txt\n--- a/tracked.txt\n+++ b/tracked.txt\n@@ -1 +1 @@\n-base\n+patched\n";
    engine.Enqueue(ApplyPatch{first_patch, {}});
    EXPECT_TRUE(WaitForTerminal(engine, "apply patch").finished);
    std::ifstream first_result(repository.path / "tracked.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(first_result), std::istreambuf_iterator<char>()), "patched\n");

    const std::filesystem::path patch_path = repository.path / "second.diff";
    std::ofstream(patch_path) << "diff --git a/tracked.txt b/tracked.txt\n--- a/tracked.txt\n+++ b/tracked.txt\n@@ -1 "
                                 "+1 @@\n-patched\n+twice\n";
    engine.Enqueue(ApplyPatch{{}, patch_path.string()});
    EXPECT_TRUE(WaitForTerminal(engine, "apply patch").finished);
    std::ifstream second_result(repository.path / "tracked.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(second_result), std::istreambuf_iterator<char>()), "twice\n");

    engine.Enqueue(ApplyPatch{});
    const TerminalEvent empty = WaitForTerminal(engine, "apply patch");
    EXPECT_FALSE(empty.finished);
    EXPECT_NE(empty.message.find("patch is empty"), std::string::npos);

    engine.Enqueue(ApplyPatch{{}, (repository.path / "missing.diff").string()});
    const TerminalEvent missing = WaitForTerminal(engine, "apply patch");
    EXPECT_FALSE(missing.finished);
    EXPECT_NE(missing.message.find("could not open patch file"), std::string::npos);
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
        {Duplicate{"missing", true}, "duplicate"},
        {Reorder{"missing-source", "missing-target", GG_REORDER_AFTER}, "reorder"},
        {Split{"missing", "selected", {"tracked.txt"}}, "split"},
        {Squash{"missing-source", "missing-destination", "combined"}, "squash"},
        {Abandon{{"missing"}, true, true, {}}, "abandon"},
        {RemoteBookmarkDelete{"missing", "missing"}, "delete remote bookmark"},
        {AddRemote{"origin", "https://example.test/duplicate.git"}, "add remote"},
        {DeleteRemote{"missing"}, "delete remote"},
        {Restore{"missing-from", "missing-into", {"tracked.txt"}}, "restore"},
        {MoveFiles{"missing-source", "missing-destination", {"tracked.txt"}}, "move files"},
        {MoveDiffLines{"missing-source", "missing-destination", "tracked.txt",
             {{DiffLineKind::Addition, -1, 0, 0}}},
            "move diff lines"},
        {RevertDiffLines{"missing-source", "tracked.txt", {{DiffLineKind::Addition, -1, 0, 0}}},
            "revert diff lines"},
        {RevertFile{}, "revert file"},
        {DeleteFile{"../outside"}, "delete file"},
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

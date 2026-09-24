// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Core/RepositoryEngine.hpp"
#include "Core/Settings.hpp"
#include "Graph/Layout.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

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

class TraceCaptureSink final : public spdlog::sinks::base_sink<std::mutex>
{
public:
    std::string Text()
    {
        std::lock_guard lock(mutex_);
        return _text;
    }

private:
    void sink_it_(const spdlog::details::log_msg& message) override
    {
        _text.append(message.payload.data(), message.payload.size());
        _text.push_back('\n');
    }
    void flush_() override {}

    std::string _text;
};

class ScopedTraceCapture
{
public:
    ScopedTraceCapture() : _previous(spdlog::default_logger())
    {
        static std::atomic_uint counter = 0;
        _sink = std::make_shared<TraceCaptureSink>();
        auto logger = std::make_shared<spdlog::logger>(
            "ggui-test-trace-" + std::to_string(++counter), _sink);
        logger->set_level(spdlog::level::trace);
        logger->set_pattern("%v");
        spdlog::set_default_logger(std::move(logger));
    }

    ~ScopedTraceCapture() { spdlog::set_default_logger(std::move(_previous)); }

    std::string Text() const { return _sink->Text(); }

private:
    std::shared_ptr<spdlog::logger> _previous;
    std::shared_ptr<TraceCaptureSink> _sink;
};

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

struct TemporaryEmptyRepository
{
    TemporaryEmptyRepository()
    {
        static std::atomic_uint counter = 0;
        path = std::filesystem::temp_directory_path() /
            ("ggui-empty-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                + "-" + std::to_string(++counter));
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
        repository.reset();
        git_libgit2_shutdown();
    }

    ~TemporaryEmptyRepository() { std::filesystem::remove_all(path); }

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
    std::vector<Revision> history_revisions;
    std::uint64_t requested_topology = 0;
    std::uint64_t history_ready_topology = 0;
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
                    history_revisions.clear();
                    history_ready_topology = 0;
                    engine.Enqueue(RebuildHistory{HistoryQuery{{}, {}, {}, {}, requested_topology}});
                }
                if (history_ready_topology == ready->snapshot->repository_generation)
                {
                    auto combined = std::make_shared<RepoSnapshot>(*last_snapshot);
                    combined->revisions = history_revisions;
                    last_snapshot = std::move(combined);
                    if (predicate(*last_snapshot)) return last_snapshot;
                }
            }
            if (const auto* ready = std::get_if<HistoryReady>(&event);
                ready != nullptr && !ready->view->skeleton && last_snapshot != nullptr
                && ready->view->repository_generation == last_snapshot->repository_generation)
            {
                history_revisions.clear();
                for (const HistoryItem& item : ready->view->items)
                    if (item.kind == HistoryItemKind::Commit) history_revisions.push_back(item.revision);
                history_ready_topology = ready->view->repository_generation;
                auto combined = std::make_shared<RepoSnapshot>(*last_snapshot);
                combined->revisions = history_revisions;
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

std::shared_ptr<const RepoSnapshot> WaitForRawSnapshot(
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
                if (predicate(*last_snapshot)) return last_snapshot;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    ADD_FAILURE() << "timed out waiting for raw repository snapshot; last generation="
                  << (last_snapshot == nullptr ? 0 : last_snapshot->generation)
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

std::optional<BlameResult> WaitForBlame(RepositoryEngine& engine)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const Event& event : engine.PollEvents())
        {
            if (const auto* blame = std::get_if<BlameReady>(&event))
                return blame->blame;
            if (const auto* error = std::get_if<ErrorEvent>(&event))
            {
                ADD_FAILURE() << error->operation << ": " << error->message;
                return std::nullopt;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    ADD_FAILURE() << "timed out waiting for blame";
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

TEST(GraphLayout, ConnectsVirtualWorkingTreeToActiveCommit)
{
    const HistoryItem working_tree = MakeWorkingTreeHistoryItem(42, "active");
    EXPECT_EQ(working_tree.kind, HistoryItemKind::WorkingTree);
    EXPECT_EQ(working_tree.id, "working-tree:42");
    EXPECT_TRUE(working_tree.revision.oid.empty());
    EXPECT_EQ(working_tree.parents, (std::vector<std::string>{"active"}));
    const std::vector<GraphRow> rows = BuildGraphLayout(
        {{working_tree.id, working_tree.parents}, {"active", {"base"}}, {"base", {}}}, working_tree.id);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows.front().parent_tracks, (std::vector<int>{rows[1].track}));
    EXPECT_EQ(rows.front().tracks_after, rows[1].tracks_before);
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

TEST(GraphLayout, PreferredBranchKeepsColorThroughAnEarlierSideChild)
{
    // Display order lets side activate base before primary reaches it. Color
    // identity must nevertheless connect base through primary to the active
    // merge commit rather than following side.
    const std::vector<GraphNode> nodes{{"merge", {"primary", "side"}},
        {"side", {"base"}}, {"primary", {"base"}}, {"base", {}}};
    const std::vector<GraphRow> rows = BuildGraphLayout(nodes, "merge");
    ASSERT_EQ(rows.size(), 4U);
    EXPECT_EQ(rows[0].track, 0);
    EXPECT_EQ(rows[0].parent_tracks, (std::vector<int>{0, 1}));
    EXPECT_EQ(rows[1].track, 1);
    EXPECT_EQ(rows[1].parent_tracks, (std::vector<int>{1}));
    EXPECT_EQ(rows[2].track, 0);
    EXPECT_EQ(rows[2].parent_tracks, (std::vector<int>{0}));
    EXPECT_EQ(rows[2].tracks_after, (std::vector<int>{0, 1}));
    EXPECT_EQ(rows[3].track, 0);
    EXPECT_EQ(rows[3].incoming_tracks, (std::vector<int>{0, 1}));
    for (std::size_t row = 0; row + 1 < rows.size(); ++row)
        EXPECT_EQ(rows[row].tracks_after, rows[row + 1].tracks_before);
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
        {{"main", "origin", "tip", GG_NAMED_REF_REMOTE_BRANCH, true, false},
            {"release", "origin", "tip", GG_NAMED_REF_REMOTE_TAG, true, false},
            {"missing", "origin", "absent", GG_NAMED_REF_REMOTE_BRANCH, true, false}});
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
        revisions, {{"main", "origin", oid(tip.get()), GG_NAMED_REF_REMOTE_BRANCH, true, false}}, git.get());
    EXPECT_TRUE(revisions.front().pushed);
}

TEST(RepositorySummary, ReportsBranchAndUpstreamDivergence)
{
    TemporaryRepository repository;
    ASSERT_GT(git_libgit2_init(), 0);
    struct ShutdownGit
    {
        ~ShutdownGit() { git_libgit2_shutdown(); }
    } shutdown_git;
    const std::string git = "git -C " + Quote(repository.path) + " ";

    RepositorySummary summary = SummarizeRepository(repository.path.string());
    EXPECT_TRUE(summary.available);
    EXPECT_EQ(summary.branch, "main");
    EXPECT_TRUE(summary.upstream.empty());

    // Git's upstream: one local commit and two remote-only commits.
    ASSERT_EQ(std::system((git + "update-ref refs/remotes/origin/main HEAD"
        " && " + git + "config branch.main.remote origin"
        " && " + git + "config branch.main.merge refs/heads/main"
        " && " + git + "commit --allow-empty -q -m local"
        " && " + git + "update-ref refs/remotes/origin/main $(" + git + "commit-tree -p origin/main -m r1 HEAD^{tree})"
        " && " + git + "update-ref refs/remotes/origin/main $(" + git + "commit-tree -p origin/main -m r2 HEAD^{tree})")
                              .c_str()),
        0);
    summary = SummarizeRepository(repository.path.string());
    EXPECT_EQ(summary.upstream, "origin/main");
    EXPECT_EQ(summary.outgoing, 1U);
    EXPECT_EQ(summary.incoming, 2U);

    // gg tracking without Git configuration.
    ASSERT_EQ(std::system((git + "config --unset branch.main.remote && " + git
        + "update-ref refs/gg/tracking/branches/origin/main origin/main").c_str()), 0);
    summary = SummarizeRepository(repository.path.string());
    EXPECT_EQ(summary.upstream, "origin/main");
    EXPECT_EQ(summary.outgoing, 1U);
    EXPECT_EQ(summary.incoming, 2U);

    ASSERT_EQ(std::system((git + "checkout -q --detach").c_str()), 0);
    summary = SummarizeRepository(repository.path.string());
    EXPECT_TRUE(summary.available);
    EXPECT_TRUE(summary.branch.empty());
    EXPECT_EQ(summary.incoming + summary.outgoing, 0U);

    EXPECT_FALSE(SummarizeRepository((repository.path / "missing").string()).available);
}

TEST(RepositoryEngine, OpensWithoutScanningAndRefreshesOnRequest)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForRawSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.root.empty(); });
    ASSERT_NE(opened, nullptr);
    EXPECT_TRUE(opened->has_worktree);
    EXPECT_EQ(opened->worktree_state, RepoSnapshot::WorktreeState::Unscanned);
    EXPECT_TRUE(opened->status.empty());
    // @ is Git's HEAD as soon as the repository is adopted.
    EXPECT_FALSE(opened->head.empty());
    EXPECT_EQ(opened->working_copy, opened->head);
    EXPECT_EQ(opened->head_branch, "main");
    EXPECT_TRUE(opened->revisions.empty());
    std::this_thread::sleep_for(100ms);
    const auto open_events = engine.PollEvents();
    EXPECT_TRUE(std::ranges::none_of(open_events, [](const Event& event) {
        return std::holds_alternative<DiffReady>(event);
    }));
    ASSERT_EQ(opened->remotes.size(), 1U);
    EXPECT_EQ(opened->remotes.front().name, "origin");
    EXPECT_EQ(opened->remotes.front().fetch_url, "https://example.test/repository.git");
    EXPECT_FALSE(opened->can_undo);
    EXPECT_FALSE(opened->can_redo);
    std::ofstream(repository.path / "tracked.txt") << "changed\n";
    engine.Enqueue(Refresh{true, {}, true});
    const auto refreshed = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation
            && snapshot.worktree_state == RepoSnapshot::WorktreeState::Ready;
    });
    ASSERT_NE(refreshed, nullptr);
    ASSERT_EQ(refreshed->status.size(), 1U);
    EXPECT_EQ(refreshed->status.front().path, "tracked.txt");
    EXPECT_EQ(refreshed->status.front().status, GIT_DELTA_MODIFIED);
    EXPECT_EQ(refreshed->head, opened->head);
    EXPECT_EQ(refreshed->working_copy, refreshed->head);
    engine.Enqueue(LoadDiff{MakeWorkingTreeHistoryItem(refreshed->repository_generation,
                                refreshed->head).id,
        "tracked.txt"});
    const auto diff = WaitForDiff(engine);
    ASSERT_TRUE(diff.has_value());
    EXPECT_EQ(diff->before, "base\n");
    EXPECT_EQ(diff->after, "changed\n");
    std::ifstream tracked(repository.path / "tracked.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(tracked), {}), "changed\n");
}

TEST(RepositoryEngine, IncrementalRefreshUpdatesOnlyTouchedStatusPaths)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt") << "modified\n";
    std::ofstream(repository.path / "untracked.txt") << "new\n";

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForRawSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.root.empty();
    });
    ASSERT_NE(opened, nullptr);
    EXPECT_EQ(opened->worktree_state, RepoSnapshot::WorktreeState::Unscanned);
    EXPECT_TRUE(opened->status.empty());
    engine.Enqueue(Refresh{true, {}, true});
    const auto scanned = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && snapshot.status.size() == 2
            && snapshot.worktree_state == RepoSnapshot::WorktreeState::Ready;
    });
    ASSERT_NE(scanned, nullptr);
    ASSERT_NE(std::ranges::find(scanned->status, "tracked.txt", &StatusEntry::path), scanned->status.end());
    ASSERT_NE(std::ranges::find(scanned->status, "untracked.txt", &StatusEntry::path), scanned->status.end());

    std::filesystem::remove(repository.path / "untracked.txt");
    engine.Enqueue(Refresh{true, {"untracked.txt"}});
    const auto refreshed = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > scanned->generation && snapshot.status.size() == 1;
    });
    ASSERT_NE(refreshed, nullptr);
    EXPECT_EQ(refreshed->status.front().path, "tracked.txt");
    EXPECT_EQ(refreshed->status.front().status, GIT_DELTA_MODIFIED);

    std::filesystem::create_directories(repository.path / "untracked-dir");
    std::ofstream(repository.path / "untracked-dir" / "nested.txt") << "new again\n";
    engine.Enqueue(Refresh{true, {"untracked-dir"}});
    const auto added = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > refreshed->generation && snapshot.status.size() == 2;
    });
    ASSERT_NE(added, nullptr);
    EXPECT_NE(std::ranges::find(added->status, "untracked-dir/nested.txt", &StatusEntry::path), added->status.end());
}

TEST(RepositoryEngine, MarksNewTopLevelDirectoriesStaleUntilExplicitRefresh)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForRawSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.root.empty() && snapshot.worktree_state == RepoSnapshot::WorktreeState::Unscanned;
    });
    ASSERT_NE(opened, nullptr);
    engine.Enqueue(Refresh{true, {}, true});
    const auto scanned = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation
            && snapshot.worktree_state == RepoSnapshot::WorktreeState::Ready;
    });
    ASSERT_NE(scanned, nullptr);

    std::filesystem::create_directories(repository.path / "new-directory");
    std::ofstream(repository.path / "new-directory" / "file.txt") << "new\n";
    const auto stale = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > scanned->generation
            && snapshot.worktree_state == RepoSnapshot::WorktreeState::Stale;
    });
    ASSERT_NE(stale, nullptr);
    EXPECT_TRUE(stale->status.empty());
    engine.Enqueue(Refresh{true, {}, true});
    const auto refreshed = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > stale->generation
            && snapshot.worktree_state == RepoSnapshot::WorktreeState::Ready;
    });
    ASSERT_NE(refreshed, nullptr);
    const auto file = std::ranges::find(refreshed->status, "new-directory/file.txt", &StatusEntry::path);
    ASSERT_NE(file, refreshed->status.end());
    EXPECT_EQ(file->status, GIT_DELTA_UNTRACKED);
}

TEST(RepositoryEngine, InvalidatesHistoryAfterAbandoningANonCurrentChange)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.revisions.empty();
    });
    ASSERT_NE(opened, nullptr);
    const std::string base = opened->head;

    engine.Enqueue(NewChange{"side", {base}, {}, {}, false});
    const auto side = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && !snapshot.working_copy.empty();
    });
    ASSERT_NE(side, nullptr);
    const std::string abandoned = side->working_copy;

    engine.Enqueue(NewChange{"current", {base}, {}, {}, false});
    const auto current = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > side->generation && snapshot.working_copy != abandoned;
    });
    ASSERT_NE(current, nullptr);

    engine.Enqueue(Abandon{{abandoned}, true, false, {}});
    const auto updated = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > current->generation;
    });
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated->working_copy, current->working_copy);
    EXPECT_GT(updated->repository_generation, current->repository_generation);
    EXPECT_TRUE(std::ranges::none_of(updated->revisions,
        [&](const Revision& revision) { return revision.oid == abandoned; }));
}

TEST(RepositoryEngine, AbandonsEmptyLastCommitWithUncommittedChanges)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.revisions.empty();
    });
    ASSERT_NE(opened, nullptr);
    const std::string base = opened->head;

    engine.Enqueue(NewChange{"empty", {base}, {}, {}, false});
    const auto created = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && !snapshot.working_copy.empty()
            && snapshot.working_copy != base;
    });
    ASSERT_NE(created, nullptr);
    const std::string empty = created->working_copy;

    std::ofstream(repository.path / "tracked.txt") << "uncommitted\n";
    engine.Enqueue(Abandon{{empty}, false, false, {}});
    const TerminalEvent terminal = WaitForTerminal(engine, "abandon");
    EXPECT_TRUE(terminal.finished) << terminal.message;
    const auto abandoned = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return std::ranges::none_of(snapshot.revisions,
            [&](const Revision& revision) { return revision.oid == empty; });
    });
    ASSERT_NE(abandoned, nullptr);
    EXPECT_EQ(abandoned->working_copy.empty() ? abandoned->head : abandoned->working_copy, base);
    std::ifstream input(repository.path / "tracked.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()),
        "uncommitted\n");
}

TEST(RepositoryEngine, KeepsUnnamedLocalHeadsVisibleAfterSwitchingChanges)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.revisions.empty();
    });
    ASSERT_NE(opened, nullptr);

    engine.Enqueue(NewChange{"first", {opened->head}, {}, {}, false});
    const auto first = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && !snapshot.working_copy.empty();
    });
    ASSERT_NE(first, nullptr);
    const std::string first_head = first->working_copy;

    engine.Enqueue(NewChange{"second", {opened->head}, {}, {}, false});
    const auto second = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > first->generation && snapshot.working_copy != first_head;
    });
    ASSERT_NE(second, nullptr);
    EXPECT_NE(std::ranges::find(second->revisions, first_head, &Revision::oid), second->revisions.end());
    EXPECT_NE(std::ranges::find(second->revisions, second->working_copy, &Revision::oid), second->revisions.end());
}

TEST(RepositoryEngine, ReplacesAndRemovesUndescribedEmptyWorkingChanges)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.revisions.empty();
    });
    ASSERT_NE(opened, nullptr);

    engine.Enqueue(NewChange{});
    const auto first = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && !snapshot.working_copy.empty();
    });
    ASSERT_NE(first, nullptr);
    const std::string replaced = first->working_copy;
    const auto first_change = std::ranges::find(first->revisions, replaced, &Revision::oid);
    ASSERT_NE(first_change, first->revisions.end());
    ASSERT_TRUE(first_change->empty);
    ASSERT_TRUE(first_change->description.empty());

    engine.Enqueue(NewChange{{}, {"@"}, {}, {}, false});
    const auto child = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > first->generation && snapshot.working_copy != replaced;
    });
    ASSERT_NE(child, nullptr);
    engine.Enqueue(Abandon{{replaced}, true, false, {}});
    const auto replacement = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > child->generation;
    });
    ASSERT_NE(replacement, nullptr);
    EXPECT_NE(replacement->working_copy, replaced);
    const auto replacement_change =
        std::ranges::find(replacement->revisions, replacement->working_copy, &Revision::oid);
    ASSERT_NE(replacement_change, replacement->revisions.end());
    EXPECT_TRUE(replacement_change->empty);
    EXPECT_TRUE(replacement_change->description.empty());
    EXPECT_EQ(replacement_change->parents, first_change->parents);
    EXPECT_TRUE(std::ranges::none_of(replacement->revisions,
        [&](const Revision& revision) { return revision.oid == replaced; }));

    ASSERT_FALSE(replacement_change->parents.empty());
    const std::string parent = replacement_change->parents.front();
    const std::string removed = replacement->working_copy;
    engine.Enqueue(Abandon{{removed}, true, false, {}});
    const auto abandoned = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > replacement->generation && snapshot.working_copy != removed;
    });
    ASSERT_NE(abandoned, nullptr);
    engine.Enqueue(Edit{parent});
    const auto edited = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > abandoned->generation && snapshot.working_copy == parent;
    });
    ASSERT_NE(edited, nullptr);
    EXPECT_TRUE(std::ranges::none_of(edited->revisions,
        [&](const Revision& revision) { return revision.oid == removed; }));
}

TEST(RepositoryEngine, PublishesProgressiveBoundedHistoryView)
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
    const std::string current = snapshot->working_copy.empty() ? snapshot->head : snapshot->working_copy;
    const auto old_ref = std::ranges::find_if(snapshot->refs, [](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_LOCAL_TAG && ref.name == "old";
    });
    ASSERT_NE(old_ref, snapshot->refs.end());
    engine.Enqueue(RebuildHistory{HistoryQuery{{}, {"old"}, {}, {}, snapshot->repository_generation}});

    bool saw_skeleton = false;
    std::shared_ptr<const HistoryView> detail;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
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
    const auto current_item = std::ranges::find(detail->items, current, &HistoryItem::id);
    const auto old_item = std::ranges::find(detail->items, old_ref->target, &HistoryItem::id);
    const auto collapsed_item = std::ranges::find_if(detail->items, [](const HistoryItem& item) {
        return item.kind == HistoryItemKind::CollapsedRegion;
    });
    ASSERT_NE(current_item, detail->items.end());
    ASSERT_NE(old_item, detail->items.end());
    ASSERT_NE(collapsed_item, detail->items.end());
    EXPECT_LT(current_item - detail->items.begin(), old_item - detail->items.begin());
    std::ptrdiff_t last_region_child = -1;
    for (auto item = detail->items.begin(); item != detail->items.end(); ++item)
        if (std::ranges::find(item->parents, collapsed_item->id) != item->parents.end())
            last_region_child = std::max(last_region_child, item - detail->items.begin());
    ASSERT_GE(last_region_child, 0);
    EXPECT_TRUE(std::ranges::all_of(detail->items.begin() + last_region_child + 1,
        collapsed_item, [](const HistoryItem& item) {
            return item.kind == HistoryItemKind::CollapsedRegion;
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
    EXPECT_LT(connected.size(), detail->items.size());

    // A ref/ID hit is already a complete direct search result. Do not launch
    // the repository-wide description scan (which would also match the older
    // commit named "old-description").
    engine.Enqueue(RebuildHistory{HistoryQuery{{}, {"old"}, {}, "old", snapshot->repository_generation}});
    std::shared_ptr<const HistoryView> direct_preview;
    std::shared_ptr<const HistoryView> direct_search;
    const auto direct_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < direct_deadline && direct_search == nullptr)
    {
        for (const Event& event : engine.PollEvents())
            if (const auto* ready = std::get_if<HistoryReady>(&event);
                ready != nullptr && ready->view->request > detail->request)
            {
                if (direct_preview == nullptr) direct_preview = ready->view;
                if (!ready->view->skeleton) direct_search = ready->view;
            }
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_NE(direct_preview, nullptr);
    EXPECT_TRUE(direct_preview->skeleton);
    ASSERT_FALSE(direct_preview->items.empty());
    EXPECT_FALSE(direct_preview->items.front().search_match);
    const auto preview_match = std::ranges::find_if(direct_preview->items,
        [](const HistoryItem& item) { return item.search_match; });
    ASSERT_NE(preview_match, direct_preview->items.end());
    EXPECT_GT(preview_match - direct_preview->items.begin(), 0);
    EXPECT_TRUE(std::ranges::all_of(direct_preview->items,
        [](const HistoryItem& item) { return item.revision.pushed; }));
    EXPECT_EQ(std::ranges::count_if(direct_preview->items,
        [](const HistoryItem& item) { return item.search_match; }), 1);
    std::unordered_set<std::string> preview_ids;
    for (const HistoryItem& item : direct_preview->items) preview_ids.insert(item.id);
    for (const HistoryItem& item : direct_preview->items)
        for (const std::string& parent : item.parents) EXPECT_TRUE(preview_ids.contains(parent));
    ASSERT_NE(direct_search, nullptr);
    EXPECT_EQ(std::ranges::count_if(direct_search->items,
        [](const HistoryItem& item) { return item.search_match; }), 1);

    engine.Enqueue(RebuildHistory{HistoryQuery{{}, {"old"}, {}, "history-0", snapshot->repository_generation}});
    std::shared_ptr<const HistoryView> searched;
    bool saw_partial_search = false;
    const auto search_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < search_deadline && searched == nullptr)
    {
        for (const Event& event : engine.PollEvents())
            if (const auto* ready = std::get_if<HistoryReady>(&event);
                ready != nullptr && !ready->view->skeleton && ready->view->request > direct_search->request)
            {
                const bool has_match = std::ranges::any_of(ready->view->items, [](const HistoryItem& item) {
                    return item.kind == HistoryItemKind::Commit && item.search_match
                        && item.revision.description.find("history-0") != std::string::npos;
                });
                if (has_match) searched = ready->view;
                else saw_partial_search = true;
            }
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_NE(searched, nullptr);
    EXPECT_TRUE(saw_partial_search);
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

TEST(RepositoryEngine, OrdersAvailableHistoryByDateWithoutBreakingTopology)
{
    TemporaryRepository repository;
    const auto git = [&](const std::string& arguments) {
        return std::system(("git -C " + Quote(repository.path) + " " + arguments + " >/dev/null 2>&1").c_str());
    };
    const auto commit = [&](std::string_view date, std::string_view description) {
        const std::string environment = "GIT_AUTHOR_DATE='" + std::string(date)
            + "' GIT_COMMITTER_DATE='" + std::string(date) + "' ";
        return std::system((environment + "git -C " + Quote(repository.path)
            + " commit --allow-empty -m '" + std::string(description) + "' >/dev/null 2>&1").c_str());
    };

    ASSERT_EQ(git("checkout -b branch-a"), 0);
    ASSERT_EQ(commit("2000-01-01T00:00:00Z", "a-parent"), 0);
    ASSERT_EQ(commit("2025-01-01T00:00:00Z", "a-tip"), 0);
    ASSERT_EQ(git("checkout main"), 0);
    ASSERT_EQ(git("checkout -b branch-b"), 0);
    ASSERT_EQ(commit("2023-01-01T00:00:00Z", "b-parent"), 0);
    ASSERT_EQ(commit("2024-01-01T00:00:00Z", "b-tip"), 0);

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto snapshot = WaitForSnapshot(engine, [](const RepoSnapshot& value) { return !value.root.empty(); });
    ASSERT_NE(snapshot, nullptr);
    engine.Enqueue(RebuildHistory{HistoryQuery{{"branch-a", "branch-b"}, {}, {}, {},
        snapshot->repository_generation}});

    std::shared_ptr<const HistoryView> history;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline && history == nullptr)
    {
        for (const Event& event : engine.PollEvents())
            if (const auto* ready = std::get_if<HistoryReady>(&event);
                ready != nullptr && !ready->view->skeleton
                && ready->view->repository_generation == snapshot->repository_generation
                && std::ranges::any_of(ready->view->items, [](const HistoryItem& item) {
                    return item.kind == HistoryItemKind::Commit
                        && item.revision.description.starts_with("a-tip");
                }))
                history = ready->view;
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_NE(history, nullptr);
    const auto position = [&](std::string_view description) {
        return std::ranges::find_if(history->items, [&](const HistoryItem& item) {
            return item.kind == HistoryItemKind::Commit && item.revision.description.starts_with(description);
        });
    };
    const auto a_tip = position("a-tip");
    const auto b_tip = position("b-tip");
    const auto b_parent = position("b-parent");
    const auto a_parent = position("a-parent");
    const auto base = position("base");
    ASSERT_NE(a_tip, history->items.end());
    ASSERT_NE(b_tip, history->items.end());
    ASSERT_NE(b_parent, history->items.end());
    ASSERT_NE(a_parent, history->items.end());
    ASSERT_NE(base, history->items.end());
    EXPECT_LT(a_tip, b_tip);
    EXPECT_LT(b_tip, b_parent);
    EXPECT_LT(b_parent, a_parent);
    EXPECT_LT(a_parent, base);
}

TEST(RepositoryEngine, CollapsesAndTogglesMergeHistory)
{
    TemporaryRepository repository;
    const auto git = [&](const std::string& arguments) {
        return std::system(("git -C " + Quote(repository.path) + " " + arguments
            + " >/dev/null 2>&1").c_str());
    };
    ASSERT_EQ(git("checkout -b feature"), 0);
    ASSERT_EQ(git("commit --allow-empty -m feature-one"), 0);
    ASSERT_EQ(git("commit --allow-empty -m feature-two"), 0);
    ASSERT_EQ(git("checkout main"), 0);
    ASSERT_EQ(git("commit --allow-empty -m main-change"), 0);
    ASSERT_EQ(git("merge --no-ff feature -m merge-feature"), 0);
    ASSERT_EQ(git("branch -D feature"), 0);

    ScopedTraceCapture trace;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto snapshot = WaitForSnapshot(engine, [](const RepoSnapshot& value) { return !value.root.empty(); });
    ASSERT_NE(snapshot, nullptr);
    engine.Enqueue(RebuildHistory{HistoryQuery{{}, {}, {}, {}, snapshot->repository_generation}});

    const auto wait_for_history = [&](std::uint64_t after) {
        std::shared_ptr<const HistoryView> result;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline && result == nullptr)
        {
            for (const Event& event : engine.PollEvents())
                if (const auto* ready = std::get_if<HistoryReady>(&event);
                    ready != nullptr && !ready->view->skeleton && ready->view->request > after)
                    result = ready->view;
            std::this_thread::sleep_for(10ms);
        }
        return result;
    };
    const auto find_description = [](const HistoryView& view, std::string_view description) {
        return std::ranges::find_if(view.items, [&](const HistoryItem& item) {
            return item.kind == HistoryItemKind::Commit
                && item.revision.description.starts_with(description);
        });
    };

    const auto collapsed = wait_for_history(0);
    ASSERT_NE(collapsed, nullptr);
    const auto collapsed_merge = find_description(*collapsed, "merge-feature");
    ASSERT_NE(collapsed_merge, collapsed->items.end());
    ASSERT_EQ(collapsed_merge->revision.parents.size(), 2U);
    EXPECT_EQ(collapsed_merge->parents.size(), 1U);
    EXPECT_EQ(find_description(*collapsed, "feature-two"), collapsed->items.end());

    engine.Enqueue(ExpandHistoryRegion{collapsed_merge->id, true});
    const auto expanded = wait_for_history(collapsed->request);
    ASSERT_NE(expanded, nullptr);
    const auto expanded_merge = find_description(*expanded, "merge-feature");
    ASSERT_NE(expanded_merge, expanded->items.end());
    EXPECT_EQ(expanded_merge->parents.size(), 2U);
    EXPECT_NE(find_description(*expanded, "feature-two"), expanded->items.end());

    engine.Enqueue(ExpandHistoryRegion{expanded_merge->id, true});
    const auto recollapsed = wait_for_history(expanded->request);
    ASSERT_NE(recollapsed, nullptr);
    const auto recollapsed_merge = find_description(*recollapsed, "merge-feature");
    ASSERT_NE(recollapsed_merge, recollapsed->items.end());
    EXPECT_EQ(recollapsed_merge->parents.size(), 1U);
    EXPECT_EQ(find_description(*recollapsed, "feature-two"), recollapsed->items.end());

    std::string diagnostics;
    const auto trace_deadline = std::chrono::steady_clock::now() + 1s;
    do
    {
        diagnostics = trace.Text();
        std::size_t completed_histories = 0;
        for (std::size_t position = 0;
            (position = diagnostics.find("kind=history request=", position)) != std::string::npos;
            ++position)
        {
            const std::size_t line_end = diagnostics.find('\n', position);
            if (diagnostics.find("outcome=success", position) < line_end) ++completed_histories;
        }
        if (completed_histories >= 3) break;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < trace_deadline);
    EXPECT_NE(diagnostics.find("repository task queued task_id="), std::string::npos);
    EXPECT_NE(diagnostics.find("kind=history request="), std::string::npos);
    EXPECT_NE(diagnostics.find("repository_generation="), std::string::npos);
    EXPECT_NE(diagnostics.find("stage=cached-metadata-loading"), std::string::npos);
    EXPECT_NE(diagnostics.find("stage=head-selection"), std::string::npos);
    EXPECT_NE(diagnostics.find("stage=bounded-reachability"), std::string::npos);
    EXPECT_NE(diagnostics.find("cache=hit candidates="), std::string::npos);
    EXPECT_NE(diagnostics.find("stage=base-history-materialization"), std::string::npos);
    EXPECT_NE(diagnostics.find("stage=merge-branch-expansion"), std::string::npos);
    EXPECT_NE(diagnostics.find("action=collapse-merge active_expansions=0"), std::string::npos);
    EXPECT_NE(diagnostics.find("stage=materialized-revision-hydration"), std::string::npos);
    EXPECT_NE(diagnostics.find("cache_misses="), std::string::npos);
    EXPECT_NE(diagnostics.find("added_commits="), std::string::npos);
    EXPECT_NE(diagnostics.find("stage=topological-ordering"), std::string::npos);
    EXPECT_NE(diagnostics.find("stage=publication"), std::string::npos);
    EXPECT_NE(diagnostics.find("outcome=success total_ms="), std::string::npos);
    EXPECT_EQ(diagnostics.find(repository.path.string()), std::string::npos);
}

TEST(RepositoryEngine, ReportsBackgroundRepositoryActivity)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});

    std::uint64_t topology = 0;
    bool saw_detail = false;
    bool saw_scan_activity = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline && !saw_detail)
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
            else if (const auto* started = std::get_if<BackgroundActivityStarted>(&event);
                started != nullptr && started->name == "Scanning working copy")
                saw_scan_activity = true;
        }
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_NE(topology, 0U);
    EXPECT_TRUE(saw_detail);
    EXPECT_FALSE(saw_scan_activity);

    engine.Enqueue(Refresh{false, {}});
    bool metadata_started = false;
    bool metadata_finished = false;
    std::uint64_t metadata_activity = 0;
    const auto metadata_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < metadata_deadline && !metadata_finished)
    {
        for (const Event& event : engine.PollEvents())
        {
            if (const auto* started = std::get_if<BackgroundActivityStarted>(&event);
                started != nullptr && started->name == "Refreshing repository metadata")
            {
                metadata_started = true;
                metadata_activity = started->id;
            }
            else if (const auto* finished = std::get_if<BackgroundActivityFinished>(&event);
                finished != nullptr && metadata_started && finished->id == metadata_activity)
                metadata_finished = true;
        }
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(metadata_started);
    EXPECT_TRUE(metadata_finished);

    engine.Enqueue(Refresh{true, {}, true});
    bool foreground_finished = false;
    bool foreground_reported_as_background = false;
    const auto refresh_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < refresh_deadline && !foreground_finished)
    {
        for (const Event& event : engine.PollEvents())
        {
            if (std::holds_alternative<BackgroundActivityStarted>(event))
                foreground_reported_as_background = true;
            if (const auto* finished = std::get_if<OperationFinished>(&event);
                finished != nullptr && finished->name == "refresh")
                foreground_finished = true;
        }
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(foreground_finished);
    EXPECT_FALSE(foreground_reported_as_background);
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

TEST(RepositoryEngine, CommitsWholeWorkingTreeWithoutLosingDiskContents)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForRawSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.root.empty(); });
    ASSERT_NE(opened, nullptr);
    std::ofstream(repository.path / "tracked.txt") << "changed\n";
    std::ofstream(repository.path / "untracked.txt") << "new\n";
    engine.Enqueue(Commit{"whole working tree"});
    const auto committed = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && snapshot.head != opened->head;
    });
    ASSERT_NE(committed, nullptr);
    EXPECT_EQ(committed->head, committed->working_copy);
    EXPECT_EQ(committed->worktree_state, RepoSnapshot::WorktreeState::Ready);
    EXPECT_TRUE(committed->status.empty());
    std::ifstream tracked(repository.path / "tracked.txt");
    std::ifstream untracked(repository.path / "untracked.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(tracked), {}), "changed\n");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(untracked), {}), "new\n");

    engine.Enqueue(LoadDiff{committed->working_copy, {}, true});
    const auto diff = WaitForDiff(engine);
    ASSERT_TRUE(diff.has_value());
    EXPECT_NE(std::ranges::find(diff->files, "tracked.txt", &StatusEntry::path), diff->files.end());
    EXPECT_NE(std::ranges::find(diff->files, "untracked.txt", &StatusEntry::path), diff->files.end());
}

TEST(RepositoryEngine, CommitsUnbornWorkingTreeWithoutLosingDiskContents)
{
    TemporaryEmptyRepository repository;
    std::ofstream(repository.path / "first.txt") << "initial contents\n";
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForRawSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.root.empty(); });
    ASSERT_NE(opened, nullptr);
    EXPECT_TRUE(opened->head.empty());
    EXPECT_TRUE(opened->working_copy.empty());
    EXPECT_TRUE(opened->status.empty());
    EXPECT_EQ(opened->worktree_state, RepoSnapshot::WorktreeState::Unscanned);
    const HistoryItem working_tree = MakeWorkingTreeHistoryItem(opened->repository_generation, opened->head);
    EXPECT_EQ(working_tree.kind, HistoryItemKind::WorkingTree);
    EXPECT_TRUE(working_tree.parents.empty());

    engine.Enqueue(LoadDiff{working_tree.id, "first.txt"});
    const auto worktree_diff = WaitForDiff(engine);
    ASSERT_TRUE(worktree_diff.has_value());
    EXPECT_EQ(worktree_diff->after, "initial contents\n");

    engine.Enqueue(Commit{"initial commit"});
    const auto committed = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && !snapshot.head.empty();
    });
    ASSERT_NE(committed, nullptr);
    EXPECT_FALSE(committed->working_copy.empty());
    std::ifstream file(repository.path / "first.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "initial contents\n");
    engine.Enqueue(LoadDiff{committed->working_copy, "first.txt"});
    const auto commit_diff = WaitForDiff(engine);
    ASSERT_TRUE(commit_diff.has_value());
    EXPECT_EQ(commit_diff->after, "initial contents\n");
}

TEST(RepositoryEngine, LeavesDirtyGitWorkingTreeUnscannedUntilRequested)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt") << "modified\n";
    std::ofstream(repository.path / "untracked.txt") << "new\n";

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForRawSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.root.empty() && snapshot.worktree_state == RepoSnapshot::WorktreeState::Unscanned;
    });
    ASSERT_NE(opened, nullptr);
    EXPECT_TRUE(opened->status.empty());
    const std::string original_working_copy = opened->working_copy;
    const std::string original_head = opened->head;
    ASSERT_FALSE(original_head.empty());

    const std::string working_tree_id = MakeWorkingTreeHistoryItem(
        opened->repository_generation, opened->working_copy.empty() ? opened->head : opened->working_copy).id;
    engine.Enqueue(LoadDiff{working_tree_id, {}, true});
    const auto worktree_diff = WaitForDiff(engine);
    ASSERT_TRUE(worktree_diff.has_value());
    EXPECT_NE(std::ranges::find(worktree_diff->files, "tracked.txt", &StatusEntry::path), worktree_diff->files.end());
    EXPECT_NE(std::ranges::find(worktree_diff->files, "untracked.txt", &StatusEntry::path), worktree_diff->files.end());

    engine.Enqueue(Refresh{true, {}, true});
    const auto refreshed = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && snapshot.status.size() == 2
            && snapshot.worktree_state == RepoSnapshot::WorktreeState::Ready;
    });
    ASSERT_NE(refreshed, nullptr);
    EXPECT_EQ(refreshed->working_copy, original_working_copy);
    EXPECT_EQ(refreshed->head, original_head);
    const auto modified = std::ranges::find(refreshed->status, "tracked.txt", &StatusEntry::path);
    const auto added = std::ranges::find(refreshed->status, "untracked.txt", &StatusEntry::path);
    ASSERT_NE(modified, refreshed->status.end());
    ASSERT_NE(added, refreshed->status.end());
    EXPECT_EQ(modified->status, GIT_DELTA_MODIFIED);
    EXPECT_EQ(added->status, GIT_DELTA_UNTRACKED);
    std::ifstream tracked(repository.path / "tracked.txt");
    std::ifstream untracked(repository.path / "untracked.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(tracked), {}), "modified\n");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(untracked), {}), "new\n");
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
    const auto opened = WaitForRawSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.root.empty(); });
    ASSERT_NE(opened, nullptr);
    engine.Enqueue(NewChange{});
    const auto working = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && !snapshot.working_copy.empty();
    });
    ASSERT_NE(working, nullptr);

    std::ofstream(repository.path / "oversized.txt") << "xx";
    engine.Enqueue(Refresh{true, {}, true});
    const auto untracked_snapshot = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > working->generation
            && std::ranges::any_of(snapshot.status, [](const StatusEntry& entry) {
                   return entry.path == "oversized.txt" && entry.status == GIT_DELTA_UNTRACKED;
               });
    });
    ASSERT_NE(untracked_snapshot, nullptr);

    engine.Enqueue(LoadDiff{MakeWorkingTreeHistoryItem(untracked_snapshot->repository_generation,
                                untracked_snapshot->working_copy.empty() ? untracked_snapshot->head
                                                                         : untracked_snapshot->working_copy)
                                .id,
        "oversized.txt"});
    const auto diff = WaitForDiff(engine);
    ASSERT_TRUE(diff.has_value());
    const auto untracked = std::ranges::find(diff->files, "oversized.txt", &StatusEntry::path);
    ASSERT_NE(untracked, diff->files.end());
    EXPECT_EQ(untracked->status, GIT_DELTA_UNTRACKED);

    engine.Enqueue(TrackPaths{{"oversized.txt"}});
    const auto tracked = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > untracked_snapshot->generation;
    });
    ASSERT_NE(tracked, nullptr);
    EXPECT_TRUE(tracked->status.empty());
    ASSERT_FALSE(tracked->working_copy.empty());
    engine.Enqueue(Commit{"capture oversized file"});
    const auto committed = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > tracked->generation && snapshot.working_copy != tracked->working_copy;
    });
    ASSERT_NE(committed, nullptr);
    engine.Enqueue(LoadDiff{committed->working_copy, "oversized.txt"});
    const auto captured = WaitForDiff(engine);
    ASSERT_TRUE(captured.has_value());
    ASSERT_EQ(captured->files.size(), 1U);
    EXPECT_EQ(captured->files.front().status, GIT_DELTA_ADDED);
    EXPECT_EQ(captured->after, "xx");
    std::ifstream file(repository.path / "oversized.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "xx");
}

TEST(RepositoryEngine, KeepsRenamedWorktreeFileAsDeletionAndUntrackedFile)
{
    TemporaryRepository repository;
    std::filesystem::rename(repository.path / "tracked.txt", repository.path / "renamed.txt");

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForRawSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.root.empty() && snapshot.worktree_state == RepoSnapshot::WorktreeState::Unscanned;
    });
    ASSERT_NE(opened, nullptr);
    engine.Enqueue(Refresh{true, {}, true});
    const auto scanned = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation
            && snapshot.worktree_state == RepoSnapshot::WorktreeState::Ready;
    });
    ASSERT_NE(scanned, nullptr);

    engine.Enqueue(LoadDiff{MakeWorkingTreeHistoryItem(scanned->repository_generation,
                                scanned->working_copy.empty() ? scanned->head : scanned->working_copy)
                                .id,
        "renamed.txt"});
    const auto diff = WaitForDiff(engine);
    ASSERT_TRUE(diff.has_value());
    ASSERT_EQ(diff->files.size(), 2U);
    const auto removed = std::ranges::find(diff->files, "tracked.txt", &StatusEntry::path);
    const auto added = std::ranges::find(diff->files, "renamed.txt", &StatusEntry::path);
    ASSERT_NE(removed, diff->files.end());
    ASSERT_NE(added, diff->files.end());
    EXPECT_EQ(removed->status, GIT_DELTA_DELETED);
    EXPECT_EQ(added->status, GIT_DELTA_UNTRACKED);
    EXPECT_EQ(diff->after, "base\n");
}

TEST(RepositoryEngine, MovesOnlySelectedWorkingTreeFileAcrossActiveCommit)
{
    TemporaryRepository repository;
    repository.AppendEmptyCommits(1);
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForRawSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.root.empty(); });
    ASSERT_NE(opened, nullptr);
    ASSERT_FALSE(opened->head.empty());
    std::ofstream(repository.path / "tracked.txt") << "changed\n";
    std::ofstream(repository.path / "other.txt") << "keep pending\n";
    const std::string working_tree_id = MakeWorkingTreeHistoryItem(opened->repository_generation, opened->head).id;

    engine.Enqueue(MoveFiles{working_tree_id, opened->head, {"tracked.txt"}});
    const auto moved = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && snapshot.head != opened->head;
    });
    ASSERT_NE(moved, nullptr);
    const std::string active = moved->working_copy.empty() ? moved->head : moved->working_copy;
    engine.Enqueue(LoadDiff{active, "tracked.txt"});
    const auto commit_diff = WaitForDiff(engine);
    ASSERT_TRUE(commit_diff.has_value());
    EXPECT_EQ(commit_diff->before, "base\n");
    EXPECT_EQ(commit_diff->after, "changed\n");
    engine.Enqueue(LoadDiff{MakeWorkingTreeHistoryItem(moved->repository_generation, active).id, {}, true});
    const auto worktree_diff = WaitForDiff(engine);
    ASSERT_TRUE(worktree_diff.has_value());
    EXPECT_EQ(std::ranges::find(worktree_diff->files, "tracked.txt", &StatusEntry::path),
        worktree_diff->files.end());
    EXPECT_NE(std::ranges::find(worktree_diff->files, "other.txt", &StatusEntry::path),
        worktree_diff->files.end());

    engine.Enqueue(MoveFiles{active, MakeWorkingTreeHistoryItem(moved->repository_generation, active).id,
        {"tracked.txt"}});
    const auto restored = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > moved->generation && snapshot.head != moved->head;
    });
    ASSERT_NE(restored, nullptr);
    const std::string restored_active = restored->working_copy.empty() ? restored->head : restored->working_copy;
    engine.Enqueue(LoadDiff{MakeWorkingTreeHistoryItem(restored->repository_generation, restored_active).id,
        "tracked.txt"});
    const auto restored_diff = WaitForDiff(engine);
    ASSERT_TRUE(restored_diff.has_value());
    EXPECT_EQ(restored_diff->before, "base\n");
    EXPECT_EQ(restored_diff->after, "changed\n");
    std::ifstream tracked(repository.path / "tracked.txt");
    std::ifstream other(repository.path / "other.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(tracked), {}), "changed\n");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(other), {}), "keep pending\n");
}

TEST(RepositoryEngine, MovesOnlySelectedWorkingTreeLinesAcrossActiveCommit)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForRawSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.root.empty(); });
    ASSERT_NE(opened, nullptr);
    ASSERT_FALSE(opened->head.empty());
    std::ofstream(repository.path / "tracked.txt") << "base\nfirst\nsecond\n";
    const std::string working_tree_id = MakeWorkingTreeHistoryItem(opened->repository_generation, opened->head).id;
    engine.Enqueue(LoadDiff{working_tree_id, "tracked.txt"});
    const auto initial_diff = WaitForDiff(engine);
    ASSERT_TRUE(initial_diff.has_value());
    const auto selected = std::ranges::find_if(initial_diff->lines,
        [](const DiffLine& line) { return line.kind == DiffLineKind::Addition && line.new_line == 1; });
    ASSERT_NE(selected, initial_diff->lines.end());

    engine.Enqueue(MoveDiffLines{working_tree_id, opened->head, "tracked.txt", {*selected}});
    const auto moved = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && snapshot.head != opened->head;
    });
    ASSERT_NE(moved, nullptr);
    const std::string active = moved->working_copy.empty() ? moved->head : moved->working_copy;
    engine.Enqueue(LoadDiff{active, "tracked.txt"});
    const auto commit_diff = WaitForDiff(engine);
    ASSERT_TRUE(commit_diff.has_value());
    EXPECT_EQ(commit_diff->after, "base\nfirst\n");
    engine.Enqueue(LoadDiff{MakeWorkingTreeHistoryItem(moved->repository_generation, active).id, "tracked.txt"});
    const auto remaining_diff = WaitForDiff(engine);
    ASSERT_TRUE(remaining_diff.has_value());
    EXPECT_EQ(remaining_diff->before, "base\nfirst\n");
    EXPECT_EQ(remaining_diff->after, "base\nfirst\nsecond\n");

    const auto moved_line = std::ranges::find_if(commit_diff->lines,
        [](const DiffLine& line) { return line.kind == DiffLineKind::Addition && line.new_line == 1; });
    ASSERT_NE(moved_line, commit_diff->lines.end());
    engine.Enqueue(MoveDiffLines{active, MakeWorkingTreeHistoryItem(moved->repository_generation, active).id,
        "tracked.txt", {*moved_line}});
    const auto restored = WaitForRawSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > moved->generation && snapshot.head != moved->head;
    });
    ASSERT_NE(restored, nullptr);
    const std::string restored_active = restored->working_copy.empty() ? restored->head : restored->working_copy;
    engine.Enqueue(LoadDiff{MakeWorkingTreeHistoryItem(restored->repository_generation, restored_active).id,
        "tracked.txt"});
    const auto restored_diff = WaitForDiff(engine);
    ASSERT_TRUE(restored_diff.has_value());
    EXPECT_EQ(restored_diff->before, "base\n");
    EXPECT_EQ(restored_diff->after, "base\nfirst\nsecond\n");
    std::ifstream tracked(repository.path / "tracked.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(tracked), {}), "base\nfirst\nsecond\n");
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

    engine.Enqueue(Abandon{{"main"}, false, false, {{"main", "origin"}}});
    const auto rejected = WaitForTerminal(engine, "abandon");
    EXPECT_FALSE(rejected.finished);
    EXPECT_NE(rejected.message.find("root revision"), std::string::npos);
    CheckGit(git_repository_open_bare(&raw_remote, remote.path.string().c_str()));
    bare.reset(raw_remote);
    raw_main = nullptr;
    EXPECT_EQ(git_reference_lookup(&raw_main, bare.get(), "refs/heads/main"), GIT_OK);
    git_reference_free(raw_main);
    bare.reset();

    engine.Enqueue(RemoteBranchDelete{"main", "origin"});
    EXPECT_TRUE(WaitForTerminal(engine, "delete remote branch").finished);
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

TEST(RepositoryEngine, FetchShowsRemoteCommitsAndPullMovesLocalBranch)
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

    const auto branch_target = [](const RepoSnapshot& snapshot, gg_named_ref_kind kind) {
        const auto ref = std::ranges::find_if(snapshot.refs, [&](const NamedRef& candidate) {
            return candidate.kind == kind && candidate.name == "main"
                && (kind != GG_NAMED_REF_REMOTE_BRANCH || candidate.remote == "origin");
        });
        return ref == snapshot.refs.end() ? std::string{} : ref->target;
    };

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.revisions.empty();
    });
    ASSERT_NE(opened, nullptr);
    const std::string local_before = branch_target(*opened, GG_NAMED_REF_LOCAL_BRANCH);
    ASSERT_FALSE(local_before.empty());
    EXPECT_TRUE(std::ranges::none_of(
        opened->revisions, [](const Revision& revision) { return revision.description == "remote"; }));

    engine.Enqueue(Fetch{"origin", false});
    const auto fetched = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        const std::string remote_target = branch_target(snapshot, GG_NAMED_REF_REMOTE_BRANCH);
        return snapshot.generation > opened->generation && !remote_target.empty()
            && std::ranges::any_of(snapshot.revisions,
                [&](const Revision& revision) { return revision.oid == remote_target; });
    });
    ASSERT_NE(fetched, nullptr);
    EXPECT_EQ(branch_target(*fetched, GG_NAMED_REF_LOCAL_BRANCH), local_before);
    const std::string remote_after_fetch = branch_target(*fetched, GG_NAMED_REF_REMOTE_BRANCH);
    EXPECT_NE(remote_after_fetch, local_before);
    const auto fetched_remote = std::ranges::find_if(fetched->refs, [](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_REMOTE_BRANCH && ref.name == "main" && ref.remote == "origin";
    });
    ASSERT_NE(fetched_remote, fetched->refs.end());
    EXPECT_TRUE(fetched_remote->desync_known);
    EXPECT_EQ(fetched_remote->remote_commits, 1U);
    EXPECT_EQ(fetched_remote->local_commits, 0U);

    engine.Enqueue(Tag{GG_TAG_SET, {"after-fetch"}, local_before});
    const auto synchronized = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > fetched->generation
            && std::ranges::any_of(snapshot.refs, [](const NamedRef& ref) {
                return ref.kind == GG_NAMED_REF_LOCAL_TAG && ref.name == "after-fetch";
            });
    });
    ASSERT_NE(synchronized, nullptr);
    EXPECT_EQ(branch_target(*synchronized, GG_NAMED_REF_LOCAL_BRANCH), local_before);

    engine.Enqueue(Fetch{"origin", true});
    const auto pulled = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > synchronized->generation
            && branch_target(snapshot, GG_NAMED_REF_LOCAL_BRANCH) == remote_after_fetch;
    });
    ASSERT_NE(pulled, nullptr);
}

TEST(RepositoryEngine, ForcePushesDivergedBranch)
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

TEST(RepositoryEngine, ReconcilesDivergedBranchAndPushesNormally)
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
            return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "main";
        });
        const auto remote_ref = std::ranges::find_if(snapshot.refs, [](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_REMOTE_BRANCH && ref.name == "main" && ref.remote == "origin";
        });
        return local == snapshot.refs.end() || remote_ref == snapshot.refs.end()
            ? BranchRelation::Unavailable
            : ClassifyBranchRelation(snapshot, local->target, remote_ref->target);
    };
    const auto tips = [](const RepoSnapshot& snapshot) {
        std::pair<std::string, std::string> result;
        for (const NamedRef& ref : snapshot.refs)
        {
            if (ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "main")
                result.first = ref.target;
            if (ref.kind == GG_NAMED_REF_REMOTE_BRANCH && ref.name == "main" && ref.remote == "origin")
                result.second = ref.target;
        }
        return result;
    };

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return relation(snapshot) == BranchRelation::LocalAhead;
    });
    ASSERT_NE(opened, nullptr);
    engine.Enqueue(Fetch{"origin", true});
    const auto diverged = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && relation(snapshot) == BranchRelation::Diverged;
    });
    ASSERT_NE(diverged, nullptr);
    const auto [local_tip, remote_tip] = tips(*diverged);
    ASSERT_FALSE(local_tip.empty());
    ASSERT_FALSE(remote_tip.empty());

    engine.Enqueue(Rebase{local_tip, remote_tip, true});
    const auto reconciled = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > diverged->generation && relation(snapshot) == BranchRelation::LocalAhead;
    });
    ASSERT_NE(reconciled, nullptr);
    ASSERT_TRUE(reconciled->can_undo);

    engine.Enqueue(Undo{});
    const auto undone = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > reconciled->generation && relation(snapshot) == BranchRelation::Diverged;
    });
    ASSERT_NE(undone, nullptr);
    engine.Enqueue(Redo{});
    ASSERT_NE(WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > undone->generation && relation(snapshot) == BranchRelation::LocalAhead;
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
            if (ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "main")
                result.first = ref.target;
            if (ref.kind == GG_NAMED_REF_REMOTE_BRANCH && ref.name == "main" && ref.remote == "origin")
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
            && ClassifyBranchRelation(snapshot, local, remote_ref) == BranchRelation::Diverged;
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
    engine.Enqueue(Refresh{true, {}, true});
    const auto alternate_changed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > alternate->generation && !snapshot.status.empty();
    });
    ASSERT_NE(alternate_changed, nullptr);
    engine.Enqueue(Amend{"@", {}});
    const auto alternate_committed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > alternate_changed->generation
            && snapshot.working_copy != alternate_changed->working_copy;
    });
    ASSERT_NE(alternate_committed, nullptr);
    const std::string alternate_id = alternate_committed->working_copy;

    engine.Enqueue(Edit{conflicted_source});
    const auto conflict_checked_out = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= alternate_committed->generation)
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
    engine.Enqueue(Refresh{true, {}, true});
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

    // Rewriting the active commit's tree would discard the uncommitted
    // partial resolution, so the rebase is refused until it is undone.
    engine.Enqueue(Rebase{conflicted_source, alternate_id});
    const TerminalEvent dirty_rebase = WaitForTerminal(engine, "rebase");
    EXPECT_FALSE(dirty_rebase.finished);
    EXPECT_NE(dirty_rebase.message.find("uncommitted changes"), std::string::npos) << dirty_rebase.message;
    std::ofstream(repository.path / "tracked.txt", std::ios::binary | std::ios::trunc) << markers;
    engine.Enqueue(Rebase{conflicted_source, alternate_id});
    const auto rebased = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= graph_preserved->generation)
            return false;
        const auto revision = FindRevision(snapshot, conflicted_source);
        return revision != snapshot.revisions.end() && revision->conflicted
            && snapshot.working_copy == revision->oid;
    });
    ASSERT_NE(rebased, nullptr);
    // Mutations leave Working tree status unscanned until an explicit Refresh.
    EXPECT_EQ(rebased->worktree_state, RepoSnapshot::WorktreeState::Unscanned);
    engine.Enqueue(Refresh{true, {}, true});
    const auto conflict_rebased = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > rebased->generation
            && snapshot.worktree_state == RepoSnapshot::WorktreeState::Ready
            && snapshot.working_copy == rebased->working_copy
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

    engine.Enqueue(ResolveConflict{rebased_source_id, "tracked.txt",
        "<<<<<<< ours\npartial resolution\n=======\ndestination\n>>>>>>> theirs\n", true});
    const auto partial_resolution = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= resolution_child->generation)
            return false;
        const auto revision = FindRevision(snapshot, rebased_source_id);
        return revision != snapshot.revisions.end() && revision->conflicted;
    });
    ASSERT_NE(partial_resolution, nullptr);
    engine.Enqueue(LoadDiff{rebased_source_id, "tracked.txt"});
    const auto partial_resolution_diff = WaitForDiff(engine);
    ASSERT_TRUE(partial_resolution_diff.has_value());
    EXPECT_NE(partial_resolution_diff->after.find("partial resolution"), std::string::npos);

    engine.Enqueue(ResolveConflict{rebased_source_id, "tracked.txt", "resolved\n", true});
    const auto resolved = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= resolution_child->generation)
            return false;
        const auto revision = FindRevision(snapshot, rebased_source_id);
        return revision != snapshot.revisions.end() && !revision->conflicted;
    });
    ASSERT_NE(resolved, nullptr);
    engine.Enqueue(Refresh{true, {}, true});
    const auto resolved_status = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > resolved->generation
            && snapshot.worktree_state == RepoSnapshot::WorktreeState::Ready;
    });
    ASSERT_NE(resolved_status, nullptr);
    EXPECT_TRUE(std::ranges::none_of(resolved_status->status, [](const StatusEntry& entry) { return entry.conflicted; }));
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
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.revisions.empty()
            && std::ranges::any_of(snapshot.workspaces, [](const Workspace& workspace) {
                   return workspace.current && workspace.managed;
               });
    });
    ASSERT_NE(opened, nullptr);
    EXPECT_EQ(std::filesystem::weakly_canonical(opened->root), std::filesystem::weakly_canonical(worktree.path));
    ASSERT_EQ(opened->workspaces.size(), 2U);
    const auto primary = std::ranges::find_if(opened->workspaces, [&](const Workspace& workspace) {
        return !workspace.stale
            && std::filesystem::weakly_canonical(workspace.root)
                == std::filesystem::weakly_canonical(repository.path);
    });
    ASSERT_NE(primary, opened->workspaces.end());
    EXPECT_FALSE(primary->managed);
    EXPECT_TRUE(primary->primary);
    EXPECT_FALSE(primary->current);
    EXPECT_FALSE(primary->working_copy.empty());
    const auto current = std::ranges::find(opened->workspaces, true, &Workspace::current);
    ASSERT_NE(current, opened->workspaces.end());
    EXPECT_FALSE(current->primary);

    engine.Enqueue(WorkspaceRemove{
        current->name, current->root, primary->root, true});
    const TerminalEvent removed = WaitForTerminal(engine, "remove workspace");
    EXPECT_TRUE(removed.finished) << removed.message;
    EXPECT_FALSE(std::filesystem::exists(worktree.path));
}

TEST(RepositoryEngine, RemovesNonCurrentLinkedWorktree)
{
    TemporaryRepository repository;
    RemovePath worktree{repository.path.string() + "-remove-worktree"};
    const std::string command =
        "git -C " + Quote(repository.path) + " worktree add --detach " + Quote(worktree.path) + " >/dev/null 2>&1";
    ASSERT_EQ(std::system(command.c_str()), 0);

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return snapshot.workspaces.size() == 2;
    });
    ASSERT_NE(opened, nullptr);
    const auto linked = std::ranges::find(opened->workspaces, false, &Workspace::current);
    ASSERT_NE(linked, opened->workspaces.end());
    ASSERT_FALSE(linked->primary);
    engine.Enqueue(WorkspaceRemove{linked->name, linked->root, {}, false});
    const TerminalEvent removed = WaitForTerminal(engine, "remove workspace");
    EXPECT_TRUE(removed.finished) << removed.message;
    EXPECT_FALSE(std::filesystem::exists(worktree.path));
}

TEST(RepositoryEngine, ReopensCurrentWorkspaceWhenRemovalIsRefused)
{
    TemporaryRepository repository;
    RemovePath worktree{repository.path.string() + "-refused-worktree"};
    const std::string command =
        "git -C " + Quote(repository.path) + " worktree add --detach " + Quote(worktree.path) + " >/dev/null 2>&1";
    ASSERT_EQ(std::system(command.c_str()), 0);

    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{worktree.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return std::ranges::any_of(snapshot.workspaces, [](const Workspace& workspace) {
            return workspace.current && workspace.managed;
        });
    });
    ASSERT_NE(opened, nullptr);
    const auto current = std::ranges::find(opened->workspaces, true, &Workspace::current);
    const auto primary = std::ranges::find(opened->workspaces, true, &Workspace::primary);
    ASSERT_NE(current, opened->workspaces.end());
    ASSERT_NE(primary, opened->workspaces.end());

    std::ofstream(worktree.path / "tracked.txt") << "keep\n";
    engine.Enqueue(WorkspaceRemove{current->name, current->root, primary->root, true});
    const TerminalEvent refused = WaitForTerminal(engine, "remove workspace");
    EXPECT_FALSE(refused.finished);
    EXPECT_FALSE(refused.message.empty());
    EXPECT_TRUE(std::filesystem::exists(worktree.path / "tracked.txt"));
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

TEST(RepositoryEngine, LoadsBlameWithoutSyntheticTerminalLine)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto snapshot = WaitForSnapshot(engine, [](const RepoSnapshot& value) {
        return !value.revisions.empty();
    });
    ASSERT_NE(snapshot, nullptr);
    const Revision& root = snapshot->revisions.back();

    engine.Enqueue(LoadBlame{root.oid, "tracked.txt"});
    const auto blame = WaitForBlame(engine);
    ASSERT_TRUE(blame.has_value());
    EXPECT_EQ(blame->revision, root.oid);
    EXPECT_EQ(blame->path, "tracked.txt");
    EXPECT_EQ(blame->viewed_revision.oid, root.oid);
    ASSERT_EQ(blame->lines.size(), 1U);
    EXPECT_EQ(blame->lines.front().line, 1U);
    EXPECT_EQ(blame->lines.front().contents, "base");
    EXPECT_EQ(blame->lines.front().revision, root.oid);
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
    const auto reopened = WaitForRawSnapshot(engine, [](const RepoSnapshot& value) {
        return !value.root.empty() && value.worktree_state == RepoSnapshot::WorktreeState::Unscanned;
    });
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(std::filesystem::weakly_canonical(reopened->root), std::filesystem::weakly_canonical(repository.path));
    EXPECT_TRUE(reopened->status.empty());
    const std::string active = reopened->working_copy.empty() ? reopened->head : reopened->working_copy;
    engine.Enqueue(LoadDiff{MakeWorkingTreeHistoryItem(reopened->repository_generation, active).id, "tracked.txt"});
    const auto reopened_diff = WaitForDiff(engine);
    ASSERT_TRUE(reopened_diff.has_value());
    EXPECT_EQ(reopened_diff->revision,
        MakeWorkingTreeHistoryItem(reopened->repository_generation, active).id);
    EXPECT_EQ(reopened_diff->after, "changed while closed\n");
}

TEST(RepositoryEngine, RenamesBranchesWithoutOverwriting)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& value) { return !value.revisions.empty(); });
    ASSERT_NE(opened, nullptr);
    const std::string revision = opened->revisions.back().oid;

    engine.Enqueue(Branch{GG_BRANCH_CREATE, {"taken"}, revision, {}});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& value) {
        return std::ranges::any_of(value.refs, [](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "taken";
        });
    }), nullptr);
    engine.Enqueue(Branch{GG_BRANCH_RENAME, {"main"}, {}, "renamed"});
    const auto renamed = WaitForSnapshot(engine, [](const RepoSnapshot& value) {
        return std::ranges::any_of(value.refs, [](const NamedRef& ref) {
                   return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "renamed";
               })
            && std::ranges::none_of(value.refs, [](const NamedRef& ref) {
                return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "main";
            });
    });
    ASSERT_NE(renamed, nullptr);

    engine.Enqueue(Branch{GG_BRANCH_RENAME, {"renamed"}, {}, "taken"});
    const TerminalEvent conflict = WaitForTerminal(engine, "branch");
    EXPECT_FALSE(conflict.finished);
    EXPECT_NE(conflict.message.find("already exists"), std::string::npos);
}

TEST(RepositoryEngine, MovesBranchesBackwardsOnlyWhenAllowed)
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
        return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "main";
    });
    ASSERT_NE(main, opened->refs.end());
    const auto tip = std::ranges::find(opened->revisions, main->target, &Revision::oid);
    ASSERT_NE(tip, opened->revisions.end());
    ASSERT_FALSE(tip->parents.empty());
    const std::string parent = tip->parents.front();

    engine.Enqueue(Branch{GG_BRANCH_MOVE, {"main"}, parent, {}});
    const TerminalEvent rejected = WaitForTerminal(engine, "branch");
    EXPECT_FALSE(rejected.finished);
    EXPECT_NE(rejected.message.find("refusing to move branch"), std::string::npos);

    engine.Enqueue(Branch{GG_BRANCH_MOVE, {"main"}, parent, {}, true});
    const auto moved = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return std::ranges::any_of(snapshot.refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "main" && ref.target == parent;
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
    const auto opened = WaitForRawSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.root.empty(); });
    ASSERT_NE(opened, nullptr);
    ASSERT_FALSE(opened->head.empty());
    const std::string active = opened->working_copy.empty() ? opened->head : opened->working_copy;
    const std::string working_tree_id =
        MakeWorkingTreeHistoryItem(opened->repository_generation, active).id;

    engine.Enqueue(LoadDiff{working_tree_id, "tracked.txt", false,
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

    engine.Enqueue(LoadDiff{working_tree_id, "tracked.txt", false,
        DiffOptions{.whitespace_mode = DiffWhitespaceMode::IgnoreAllWhitespace, .context_lines = 0}});
    const auto filtered = WaitForDiff(engine);
    ASSERT_TRUE(filtered.has_value());
    EXPECT_EQ(filtered->before, "line03\n");
    EXPECT_EQ(filtered->after, "changed\n");

    engine.Enqueue(LoadDiff{working_tree_id, "tracked.txt", false,
        DiffOptions{.whitespace_mode = DiffWhitespaceMode::Normal, .context_lines = 0}, active, true});
    const auto compared = WaitForDiff(engine);
    ASSERT_TRUE(compared.has_value());
    EXPECT_TRUE(compared->file_comparison);
    EXPECT_EQ(compared->before, "line03\nline08\n");
    EXPECT_EQ(compared->after, "changed\nline 08\n");

    engine.Enqueue(LoadDiff{working_tree_id, "tracked.txt", false,
        DiffOptions{.whitespace_mode = DiffWhitespaceMode::Normal, .context_lines = -1}, active, true});
    const auto full = WaitForDiff(engine);
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(full->before,
        "line01\nline02\nline03\nline04\nline05\nline06\nline07\nline08\nline09\nline10\n");
    EXPECT_EQ(full->after,
        "line01\nline02\nchanged\nline04\nline05\nline06\nline07\nline 08\nline09\nline10\n");
}

TEST(RepositoryEngine, MovesLinesOnlyIntoChosenChildAndUndoesAtomically)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt") << "one\ntwo\nthree\n";
    const std::string commit = "git -C " + Quote(repository.path) + " add tracked.txt && git -C "
        + Quote(repository.path) + " commit -m lines >/dev/null 2>&1";
    ASSERT_EQ(std::system(commit.c_str()), 0);
    std::ofstream(repository.path / "tracked.txt") << "ONE\ntwo\nTHREE\n";
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    engine.Enqueue(Commit{"source lines"});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.working_copy.empty();
    });
    ASSERT_NE(opened, nullptr);
    const std::string source = opened->working_copy;
    engine.Enqueue(NewChange{"chosen child", {source}, {}, {}, false});
    const auto chosen = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        const auto working = FindRevision(snapshot, snapshot.working_copy);
        return snapshot.generation > opened->generation && working != snapshot.revisions.end()
            && working->description == "chosen child";
    });
    ASSERT_NE(chosen, nullptr);
    // Pin the selected side branch as a history head. Undo clears identity
    // aliases under the V4 operation format; the branch must restore exactly.
    engine.Enqueue(Branch{GG_BRANCH_CREATE, {"chosen-child"}, chosen->working_copy, {}});
    const auto branched = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > chosen->generation
            && std::ranges::any_of(snapshot.refs, [&](const NamedRef& ref) {
                   return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "chosen-child"
                       && ref.target == chosen->working_copy;
               });
    });
    ASSERT_NE(branched, nullptr);
    engine.Enqueue(NewChange{"sibling", {source}, {}, {}, false});
    const auto sibling = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        const auto working = FindRevision(snapshot, snapshot.working_copy);
        return snapshot.generation > branched->generation && working != snapshot.revisions.end()
            && working->description == "sibling";
    });
    ASSERT_NE(sibling, nullptr);
    engine.Enqueue(MoveDiffLines{source, chosen->working_copy, "tracked.txt",
        {{DiffLineKind::Deletion, 0, -1, 0}, {DiffLineKind::Addition, -1, 0, 0}}});
    const auto moved = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > sibling->generation && snapshot.working_copy != sibling->working_copy;
    });
    ASSERT_NE(moved, nullptr);
    const auto rewritten_sibling = FindRevision(*moved, sibling->working_copy);
    const auto rewritten_chosen = FindRevision(*moved, chosen->working_copy);
    ASSERT_NE(rewritten_sibling, moved->revisions.end());
    ASSERT_NE(rewritten_chosen, moved->revisions.end());
    EXPECT_TRUE(std::ranges::any_of(moved->refs, [&](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "chosen-child"
            && ref.target == rewritten_chosen->oid;
    }));
    engine.Enqueue(LoadDiff{rewritten_sibling->oid, "tracked.txt", false,
        DiffOptions{.context_lines = -1}});
    const auto sibling_diff = WaitForDiff(engine);
    ASSERT_TRUE(sibling_diff.has_value());
    EXPECT_EQ(sibling_diff->before, "one\ntwo\nTHREE\n");
    EXPECT_EQ(sibling_diff->after, "one\ntwo\nTHREE\n");
    EXPECT_TRUE(sibling_diff->patch.empty());
    engine.Enqueue(LoadDiff{rewritten_chosen->oid, "tracked.txt", false,
        DiffOptions{.context_lines = -1}});
    const auto chosen_diff = WaitForDiff(engine);
    ASSERT_TRUE(chosen_diff.has_value());
    EXPECT_EQ(chosen_diff->before, "one\ntwo\nTHREE\n");
    EXPECT_EQ(chosen_diff->after, "ONE\ntwo\nTHREE\n");
    engine.Enqueue(Undo{});
    const auto undone = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > moved->generation && snapshot.working_copy == sibling->working_copy;
    });
    ASSERT_NE(undone, nullptr);
    EXPECT_EQ(undone->working_copy, sibling->working_copy);
    EXPECT_NE(std::ranges::find(undone->revisions, source, &Revision::oid), undone->revisions.end());
    EXPECT_NE(std::ranges::find(undone->revisions, chosen->working_copy, &Revision::oid), undone->revisions.end());
    EXPECT_TRUE(std::ranges::any_of(undone->refs, [&](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "chosen-child"
            && ref.target == chosen->working_copy;
    }));
}

TEST(RepositoryEngine, MovesPartialReplacementToParentWithoutReplayingSource)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt") << "one\ntwo";
    const std::string commit = "git -C " + Quote(repository.path) + " add tracked.txt && git -C "
        + Quote(repository.path) + " commit -m lines >/dev/null 2>&1";
    ASSERT_EQ(std::system(commit.c_str()), 0);
    std::ofstream(repository.path / "tracked.txt") << "ONE\ntwo";
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    engine.Enqueue(Commit{"source replacement"});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.working_copy.empty();
    });
    ASSERT_NE(opened, nullptr);
    const auto source = FindRevision(*opened, opened->working_copy);
    ASSERT_NE(source, opened->revisions.end());
    ASSERT_EQ(source->parents.size(), 1U);
    const std::string parent = source->parents.front();
    engine.Enqueue(MoveDiffLines{source->oid, parent, "tracked.txt",
        {{DiffLineKind::Addition, -1, 0, 0}}});
    const auto moved = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation;
    });
    ASSERT_NE(moved, nullptr);
    engine.Enqueue(LoadDiff{moved->working_copy, "tracked.txt", false,
        DiffOptions{.context_lines = -1}});
    const auto source_diff = WaitForDiff(engine);
    ASSERT_TRUE(source_diff.has_value());
    EXPECT_EQ(source_diff->before, "one\nONE\ntwo");
    EXPECT_EQ(source_diff->after, "ONE\ntwo");
    const auto rewritten_source = FindRevision(*moved, source->oid);
    ASSERT_NE(rewritten_source, moved->revisions.end());
    EXPECT_FALSE(rewritten_source->conflicted);
}

TEST(RepositoryEngine, RevertsOneInsertionWhilePreservingAnotherAtItsOriginalPosition)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt") << "one\ntwo\nthree";
    const std::string commit = "git -C " + Quote(repository.path) + " add tracked.txt && git -C "
        + Quote(repository.path) + " commit -m lines >/dev/null 2>&1";
    ASSERT_EQ(std::system(commit.c_str()), 0);
    std::ofstream(repository.path / "tracked.txt") << "one\nA\ntwo\nB\nthree";
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    engine.Enqueue(Commit{"insertions"});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.working_copy.empty();
    });
    ASSERT_NE(opened, nullptr);
    engine.Enqueue(RevertDiffLines{opened->working_copy, "tracked.txt",
        {{DiffLineKind::Addition, -1, 1, 0}}});
    const auto reverted = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation;
    });
    ASSERT_NE(reverted, nullptr);
    engine.Enqueue(LoadDiff{reverted->working_copy, "tracked.txt", false,
        DiffOptions{.context_lines = -1}});
    const auto diff = WaitForDiff(engine);
    ASSERT_TRUE(diff.has_value());
    EXPECT_EQ(diff->before, "one\ntwo\nthree");
    EXPECT_EQ(diff->after, "one\ntwo\nB\nthree");
}

TEST(RepositoryEngine, MovesFileIntoChildThatAlreadyEditsItWithoutLosingContent)
{
    TemporaryRepository repository;
    std::ofstream(repository.path / "tracked.txt") << "source\n";
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    engine.Enqueue(Commit{"source file"});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.working_copy.empty();
    });
    ASSERT_NE(opened, nullptr);
    const std::string source = opened->working_copy;
    engine.Enqueue(NewChange{"child", {source}, {}, {}, false});
    const auto child = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation;
    });
    ASSERT_NE(child, nullptr);
    std::ofstream(repository.path / "tracked.txt") << "destination\n";
    engine.Enqueue(Amend{"@", {}});
    const auto edited = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > child->generation;
    });
    ASSERT_NE(edited, nullptr);
    engine.Enqueue(MoveFiles{source, edited->working_copy, {"tracked.txt"}});
    const auto moved = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > edited->generation;
    });
    ASSERT_NE(moved, nullptr);
    const auto rewritten_source = FindRevision(*moved, source);
    const auto rewritten_child = FindRevision(*moved, edited->working_copy);
    ASSERT_NE(rewritten_source, moved->revisions.end());
    ASSERT_NE(rewritten_child, moved->revisions.end());
    EXPECT_FALSE(rewritten_child->conflicted);
    engine.Enqueue(LoadDiff{rewritten_source->oid, "tracked.txt", false, DiffOptions{.context_lines = -1}});
    const auto source_diff = WaitForDiff(engine);
    ASSERT_TRUE(source_diff.has_value());
    EXPECT_EQ(source_diff->after, "base\n");
    EXPECT_TRUE(source_diff->patch.empty());
    engine.Enqueue(LoadDiff{rewritten_child->oid, "tracked.txt", false, DiffOptions{.context_lines = -1}});
    const auto child_diff = WaitForDiff(engine);
    ASSERT_TRUE(child_diff.has_value());
    EXPECT_EQ(child_diff->before, "base\n");
    EXPECT_EQ(child_diff->after, "destination\n");
}

TEST(RepositoryEngine, MovesBothSidesOfRenamedFileIntoChild)
{
    TemporaryRepository repository;
    std::filesystem::rename(repository.path / "tracked.txt", repository.path / "renamed.txt");
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    engine.Enqueue(Commit{"rename source"});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.working_copy.empty();
    });
    ASSERT_NE(opened, nullptr);
    const std::string source = opened->working_copy;
    engine.Enqueue(NewChange{"child", {source}, {}, {}, false});
    const auto child = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation;
    });
    ASSERT_NE(child, nullptr);
    engine.Enqueue(MoveFiles{source, child->working_copy, {"renamed.txt"}});
    const auto moved = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > child->generation;
    });
    ASSERT_NE(moved, nullptr);
    const auto rewritten_source = FindRevision(*moved, source);
    ASSERT_NE(rewritten_source, moved->revisions.end());
    engine.Enqueue(LoadDiff{rewritten_source->oid, "tracked.txt", false, DiffOptions{.context_lines = -1}});
    const auto source_diff = WaitForDiff(engine);
    ASSERT_TRUE(source_diff.has_value());
    EXPECT_EQ(source_diff->after, "base\n");
    EXPECT_TRUE(source_diff->files.empty());
    engine.Enqueue(LoadDiff{moved->working_copy, "renamed.txt"});
    const auto child_diff = WaitForDiff(engine);
    ASSERT_TRUE(child_diff.has_value());
    ASSERT_EQ(child_diff->files.size(), 1U);
    EXPECT_EQ(child_diff->files.front().old_path, "tracked.txt");
    EXPECT_EQ(child_diff->files.front().path, "renamed.txt");
    EXPECT_EQ(child_diff->files.front().status, GIT_DELTA_RENAMED);
    EXPECT_EQ(child_diff->after, "base\n");
}

TEST(RepositoryEngine, MovesFileIntoMergeChildThroughEitherParentWithoutLosingResolution)
{
    for (const bool source_first : {true, false})
    {
        SCOPED_TRACE(source_first);
        TemporaryRepository repository;
        std::ofstream(repository.path / "tracked.txt") << "source\n";
        RepositoryEngine engine;
        engine.Enqueue(OpenRepository{repository.path.string()});
        engine.Enqueue(Commit{"merge source"});
        const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
            return !snapshot.working_copy.empty();
        });
        ASSERT_NE(opened, nullptr);
        const auto source_revision = FindRevision(*opened, opened->working_copy);
        ASSERT_NE(source_revision, opened->revisions.end());
        ASSERT_EQ(source_revision->parents.size(), 1U);
        const std::string source = source_revision->oid;
        // Materialize both merge parents in the bounded history view instead
        // of relying on an unbranched second parent being expanded.
        engine.Enqueue(Branch{GG_BRANCH_CREATE, {"move-source"}, source, {}});
        const auto source_branched = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
            return snapshot.generation > opened->generation
                && std::ranges::any_of(snapshot.refs, [&](const NamedRef& ref) {
                       return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "move-source"
                           && ref.target == source;
                   });
        });
        ASSERT_NE(source_branched, nullptr);
        engine.Enqueue(NewChange{"other parent", {source_revision->parents.front()}, {}, {}, false});
        const auto other = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
            const auto working = FindRevision(snapshot, snapshot.working_copy);
            return snapshot.generation > source_branched->generation && working != snapshot.revisions.end()
                && working->description == "other parent";
        });
        ASSERT_NE(other, nullptr);
        engine.Enqueue(Branch{GG_BRANCH_CREATE, {"other-parent"}, other->working_copy, {}});
        const auto other_branched = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
            return snapshot.generation > other->generation
                && std::ranges::any_of(snapshot.refs, [&](const NamedRef& ref) {
                       return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "other-parent"
                           && ref.target == other->working_copy;
                   });
        });
        ASSERT_NE(other_branched, nullptr);
        const std::vector<std::string> parents = source_first
            ? std::vector<std::string>{source, other->working_copy}
            : std::vector<std::string>{other->working_copy, source};
        engine.Enqueue(NewChange{"merge child", parents, {}, {}, false});
        const auto child = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
            const auto working = FindRevision(snapshot, snapshot.working_copy);
            return snapshot.generation > other_branched->generation && working != snapshot.revisions.end()
                && working->description == "merge child" && working->parents == parents;
        });
        ASSERT_NE(child, nullptr);
        std::ofstream(repository.path / "tracked.txt") << "resolved destination\n";
        engine.Enqueue(Amend{"@", {}});
        const auto edited = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
            return snapshot.generation > child->generation && snapshot.working_copy != child->working_copy;
        });
        ASSERT_NE(edited, nullptr);
        engine.Enqueue(LoadDiff{edited->working_copy, "tracked.txt", false, DiffOptions{.context_lines = -1}});
        const auto before_move = WaitForDiff(engine);
        ASSERT_TRUE(before_move.has_value());
        ASSERT_EQ(before_move->after, "resolved destination\n");
        engine.Enqueue(MoveFiles{source, edited->working_copy, {"tracked.txt"}});
        const auto moved = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
            const auto rewritten_source = FindRevision(snapshot, source);
            return snapshot.generation > edited->generation && snapshot.working_copy != edited->working_copy
                && rewritten_source != snapshot.revisions.end() && rewritten_source->oid != source;
        });
        ASSERT_NE(moved, nullptr);
        const auto rewritten_child = FindRevision(*moved, edited->working_copy);
        ASSERT_NE(rewritten_child, moved->revisions.end());
        ASSERT_EQ(rewritten_child->parents.size(), 2U);
        EXPECT_FALSE(rewritten_child->conflicted);
        EXPECT_NE(std::ranges::find(moved->revisions, other->working_copy, &Revision::oid), moved->revisions.end());
        EXPECT_TRUE(std::ranges::any_of(moved->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == "other-parent"
                && ref.target == other->working_copy;
        }));
        engine.Enqueue(LoadDiff{rewritten_child->oid, "tracked.txt", false, DiffOptions{.context_lines = -1}});
        const auto child_diff = WaitForDiff(engine);
        ASSERT_TRUE(child_diff.has_value());
        EXPECT_EQ(child_diff->after, "resolved destination\n");
        std::ifstream file(repository.path / "tracked.txt");
        EXPECT_EQ((std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}),
            "resolved destination\n");
    }
}

TEST(RepositoryEngine, RejectsStaleLineSelectionsWhenNewContentUsesTheSameCoordinates)
{
    for (const int action : {0, 1, 2})
    {
        SCOPED_TRACE(action);
        TemporaryRepository repository;
        std::ofstream(repository.path / "tracked.txt") << "displayed\n";
        RepositoryEngine engine;
        engine.Enqueue(OpenRepository{repository.path.string()});
        engine.Enqueue(Commit{"displayed content"});
        const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
            return !snapshot.working_copy.empty();
        });
        ASSERT_NE(opened, nullptr);
        const auto source = FindRevision(*opened, opened->working_copy);
        ASSERT_NE(source, opened->revisions.end());
        ASSERT_EQ(source->parents.size(), 1U);
        const std::string parent = source->parents.front();
        const std::vector<DiffLine> selected{
            {DiffLineKind::Deletion, 0, -1, 0}, {DiffLineKind::Addition, -1, 0, 0}};
        // The displayed diff and this newer diff have exactly the same line
        // kinds and positions; only the bytes have changed. Amend rewrites
        // the displayed commit, leaving its old ID as an alias.
        std::ofstream(repository.path / "tracked.txt") << "new unseen contents\n";
        engine.Enqueue(Amend{"@", {}});
        const auto amended = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
            return snapshot.generation > opened->generation && snapshot.working_copy != source->oid;
        });
        ASSERT_NE(amended, nullptr);
        std::string operation;
        if (action == 0)
        {
            operation = "move diff lines";
            engine.Enqueue(MoveDiffLines{source->oid, parent, "tracked.txt", selected});
        }
        else if (action == 1)
        {
            operation = "revert diff lines";
            engine.Enqueue(RevertDiffLines{source->oid, "tracked.txt", selected});
        }
        else
        {
            operation = "revert file";
            engine.Enqueue(RevertFile{source->oid, "tracked.txt", "tracked.txt", selected});
        }
        const auto rejected = WaitForTerminal(engine, operation);
        EXPECT_FALSE(rejected.finished);
        EXPECT_NE(rejected.message.find("older snapshot"), std::string::npos);
        std::ifstream file(repository.path / "tracked.txt");
        const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        EXPECT_EQ(contents, "new unseen contents\n");
        engine.Enqueue(Refresh{true, {}, true});
        const auto refreshed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
            return snapshot.generation > amended->generation;
        });
        ASSERT_NE(refreshed, nullptr);
        EXPECT_EQ(refreshed->working_copy, amended->working_copy);
        EXPECT_TRUE(refreshed->status.empty());
        const auto current = FindRevision(*refreshed, refreshed->working_copy);
        ASSERT_NE(current, refreshed->revisions.end());
        EXPECT_EQ(current->parents, std::vector<std::string>{parent});
        EXPECT_NE(std::ranges::find(refreshed->revisions, parent, &Revision::oid), refreshed->revisions.end());
        engine.Enqueue(LoadDiff{current->oid, "tracked.txt", false, DiffOptions{.context_lines = -1}});
        const auto current_diff = WaitForDiff(engine);
        ASSERT_TRUE(current_diff.has_value());
        EXPECT_EQ(current_diff->before, "base\n");
        EXPECT_EQ(current_diff->after, "new unseen contents\n");
    }
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
    engine.Enqueue(Commit{"source lines"});
    const auto opened = WaitForSnapshot(engine,
        [](const RepoSnapshot& snapshot) {
            return !snapshot.working_copy.empty() && !snapshot.revisions.empty();
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

    engine.Enqueue(NewChange{"branch sibling", {root_id}, {}, {}, false});
    const auto sibling_snapshot = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > squash_restored->generation
            && std::ranges::any_of(snapshot.revisions, [](const Revision& revision) {
                   return revision.description == "branch sibling";
               });
    });
    ASSERT_NE(sibling_snapshot, nullptr);
    const auto sibling =
        std::ranges::find(sibling_snapshot->revisions, "branch sibling", &Revision::description);
    ASSERT_NE(sibling, sibling_snapshot->revisions.end());
    const std::string sibling_id = sibling->oid;

    engine.Enqueue(Squash{root_id, {}, {}, false, true});
    const auto squashed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        if (snapshot.generation <= sibling_snapshot->generation)
            return false;
        const auto root_alias = FindRevision(snapshot, root_id);
        const auto tip_alias = FindRevision(snapshot, tip_id);
        const auto sibling_alias = FindRevision(snapshot, sibling_id);
        const auto base_alias = FindRevision(snapshot, base->oid);
        return root_alias != snapshot.revisions.end() && tip_alias != snapshot.revisions.end()
            && sibling_alias != snapshot.revisions.end() && base_alias != snapshot.revisions.end()
            && root_alias->oid == tip_alias->oid && tip_alias->oid == sibling_alias->oid
            && sibling_alias->oid == base_alias->oid && base_alias->parents.empty();
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
    engine.Enqueue(Commit{"working lines"});
    const auto opened = WaitForSnapshot(engine,
        [](const RepoSnapshot& snapshot) {
            return !snapshot.working_copy.empty() && !snapshot.revisions.empty();
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
    engine.Enqueue(Amend{"@", {}});
    const auto changed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > selected->generation
            && snapshot.working_copy != selected->working_copy;
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
    engine.Enqueue(Refresh{true, {}, true});
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

TEST(RepositoryEngine, AdoptsExternalGitCommitBeforeEditingTheWorkingTree)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    const auto opened = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); });
    ASSERT_NE(opened, nullptr);
    std::ofstream(repository.path / "tracked.txt") << "committed in ggui\n";
    engine.Enqueue(Commit{"ggui commit"});
    const auto committed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > opened->generation && !snapshot.working_copy.empty()
            && snapshot.working_copy == snapshot.head;
    });
    ASSERT_NE(committed, nullptr);

    // A terminal commit moves HEAD without gg recording an operation.
    std::ofstream(repository.path / "tracked.txt") << "committed by git\n";
    const std::string commit = "git -C " + Quote(repository.path) + " commit -qam external >/dev/null 2>&1";
    ASSERT_EQ(std::system(commit.c_str()), 0);
    engine.Enqueue(Refresh{true, {}, true});
    const auto adopted = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > committed->generation
            && snapshot.worktree_state == RepoSnapshot::WorktreeState::Ready
            && snapshot.head != committed->head;
    });
    ASSERT_NE(adopted, nullptr);
    EXPECT_EQ(adopted->working_copy, adopted->head);
    EXPECT_TRUE(adopted->status.empty());
    std::ifstream external(repository.path / "tracked.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(external), {}), "committed by git\n");

    engine.Enqueue(Edit{committed->working_copy});
    const TerminalEvent edited = WaitForTerminal(engine, "edit");
    EXPECT_TRUE(edited.finished) << edited.message;
    std::ifstream restored(repository.path / "tracked.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), "committed in ggui\n");
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
    engine.Enqueue(Amend{"@", {}});
    const auto changed = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > selected->generation
            && snapshot.working_copy != selected->working_copy;
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
    ASSERT_TRUE(WaitForTerminal(engine, "delete file").finished);
    engine.Enqueue(Refresh{true, {}, true});
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
    ASSERT_NE(WaitForRawSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return !snapshot.root.empty();
    }), nullptr);

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

TEST(RepositoryEngine, UndoRefusesDirtyWorkingTreeWithoutLosingFiles)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); }), nullptr);
    engine.Enqueue(NewChange{"working", {}, {}, {}, false});
    ASSERT_TRUE(WaitForTerminal(engine, "new").finished);

    std::ofstream(repository.path / "tracked.txt") << "pending before undo\n";
    engine.Enqueue(Undo{});
    const TerminalEvent rejected = WaitForTerminal(engine, "undo");
    EXPECT_FALSE(rejected.finished);
    EXPECT_NE(rejected.message.find("uncommitted changes"), std::string::npos);
    const auto contents = [&] {
        std::ifstream file(repository.path / "tracked.txt");
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    EXPECT_EQ(contents(), "pending before undo\n");
}

TEST(RepositoryEngine, MetadataCanClearDescriptionAndPreserveItForAuthorOnlyEdits)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); }), nullptr);
    engine.Enqueue(NewChange{"keep this description", {}, {}, {}, false});
    const auto created = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        const auto current = FindRevision(snapshot, snapshot.working_copy);
        return current != snapshot.revisions.end() && current->description == "keep this description";
    });
    ASSERT_NE(created, nullptr);
    engine.Enqueue(Metaedit{"@", std::nullopt, "New Author <new@example.test>"});
    const auto authored = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        const auto current = FindRevision(snapshot, snapshot.working_copy);
        return snapshot.generation > created->generation && current != snapshot.revisions.end()
            && current->author == "New Author";
    });
    ASSERT_NE(authored, nullptr);
    EXPECT_EQ(FindRevision(*authored, authored->working_copy)->description, "keep this description");
    engine.Enqueue(Metaedit{"@", std::string{}, {}});
    const auto cleared = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        const auto current = FindRevision(snapshot, snapshot.working_copy);
        return snapshot.generation > authored->generation && current != snapshot.revisions.end()
            && current->description.empty();
    });
    ASSERT_NE(cleared, nullptr);
    EXPECT_EQ(FindRevision(*cleared, cleared->working_copy)->author, "New Author");
}

TEST(RepositoryEngine, RestoreWithoutSourceUsesSelectedChangesParent)
{
    TemporaryRepository repository;
    RepositoryEngine engine;
    engine.Enqueue(OpenRepository{repository.path.string()});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.revisions.empty(); }), nullptr);
    engine.Enqueue(NewChange{"restore target", {}, {}, {}, false});
    ASSERT_NE(WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) { return !snapshot.working_copy.empty(); }), nullptr);
    std::ofstream(repository.path / "tracked.txt") << "changed\n";
    engine.Enqueue(Refresh{true, {}, true});
    const auto changed = WaitForSnapshot(engine, [](const RepoSnapshot& snapshot) {
        return std::ranges::any_of(snapshot.status, [](const StatusEntry& entry) { return entry.path == "tracked.txt"; });
    });
    ASSERT_NE(changed, nullptr);
    // Restore rewrites commits, so the Working tree edit must be amended first.
    engine.Enqueue(Amend{"@", {}});
    const auto amended = WaitForSnapshot(engine, [&](const RepoSnapshot& snapshot) {
        return snapshot.generation > changed->generation && snapshot.working_copy != changed->working_copy;
    });
    ASSERT_NE(amended, nullptr);
    engine.Enqueue(Restore{{}, amended->working_copy, {"tracked.txt"}});
    ASSERT_TRUE(WaitForTerminal(engine, "restore").finished);
    std::ifstream restored(repository.path / "tracked.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), "base\n");
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
        {MoveChange{GG_MOVE_PREVIOUS, 1, true}, "move"},
        {Commit{"commit"}, "commit"},
        {Amend{"missing", "amended"}, "amend"},
        {TrackPaths{{"untracked.txt"}, true}, "track paths"},
        {ChmodPaths{{"tracked.txt"}, true}, "chmod"},
        {UntrackPaths{{"tracked.txt"}}, "untrack paths"},
        {Rebase{"missing-source", "missing-destination"}, "rebase"},
        {Duplicate{"missing", true}, "duplicate"},
        {Reorder{"missing-source", "missing-target", GG_REORDER_AFTER}, "reorder"},
        {Split{"missing", "selected", {"tracked.txt"}}, "split"},
        {Squash{"missing-source", "missing-destination", "combined"}, "squash"},
        {Abandon{{"missing"}, true, true, {}}, "abandon"},
        {RemoteBranchDelete{"missing", "missing"}, "delete remote branch"},
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
        {Branch{GG_BRANCH_RENAME, {"missing"}, "missing", "renamed"}, "branch"},
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

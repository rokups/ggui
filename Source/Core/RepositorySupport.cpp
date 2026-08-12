// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Ggui::RepositoryInternal
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

std::string BlobText(git_repository* repository, git_tree* tree, const char* path, bool& binary)
{
    if (tree == nullptr || path == nullptr || *path == '\0')
        return {};
    git_tree_entry* raw_entry = nullptr;
    const int entry_result = git_tree_entry_bypath(&raw_entry, tree, path);
    if (entry_result == GIT_ENOTFOUND)
        return {};
    Check(entry_result, "load diff path");
    std::unique_ptr<git_tree_entry, decltype(&git_tree_entry_free)> entry(raw_entry, git_tree_entry_free);
    if (git_tree_entry_type(entry.get()) != GIT_OBJECT_BLOB)
        return {};
    git_blob* raw_blob = nullptr;
    Check(git_blob_lookup(&raw_blob, repository, git_tree_entry_id(entry.get())), "load diff contents");
    std::unique_ptr<git_blob, decltype(&git_blob_free)> blob(raw_blob, git_blob_free);
    if (git_blob_is_binary(blob.get()))
    {
        binary = true;
        return {};
    }
    const auto* contents = static_cast<const char*>(git_blob_rawcontent(blob.get()));
    return contents == nullptr ? std::string{} : std::string(contents, git_blob_rawsize(blob.get()));
}

DiffLine DiffLineFromRaw(const git_diff_line& line, int hunk)
{
    const DiffLineKind kind = line.origin == GIT_DIFF_LINE_ADDITION ? DiffLineKind::Addition
        : line.origin == GIT_DIFF_LINE_DELETION                     ? DiffLineKind::Deletion
                                                                    : DiffLineKind::Context;
    return {kind, line.old_lineno > 0 ? line.old_lineno - 1 : -1,
        line.new_lineno > 0 ? line.new_lineno - 1 : -1, hunk};
}

bool SameChangedLine(const DiffLine& first, const DiffLine& second)
{
    return first.kind == second.kind && first.kind != DiffLineKind::Context
        && first.old_line == second.old_line && first.new_line == second.new_line;
}

bool SameInverseLine(const DiffLine& forward, const DiffLine& reverse)
{
    return (forward.kind == DiffLineKind::Addition && reverse.kind == DiffLineKind::Deletion
               && forward.new_line == reverse.old_line)
        || (forward.kind == DiffLineKind::Deletion && reverse.kind == DiffLineKind::Addition
            && forward.old_line == reverse.new_line);
}

std::vector<std::string_view> TextLines(std::string_view text)
{
    std::vector<std::string_view> result;
    for (std::size_t begin = 0; begin < text.size();)
    {
        const std::size_t newline = text.find('\n', begin);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline + 1;
        result.push_back(text.substr(begin, end - begin));
        begin = end;
    }
    return result;
}

bool CommitIsEmpty(git_repository* repository, const git_oid& oid)
{
    git_commit* raw_commit = nullptr;
    Check(git_commit_lookup(&raw_commit, repository, &oid), "load revision");
    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
    if (git_commit_parentcount(commit.get()) == 0)
        return false;
    git_commit* raw_parent = nullptr;
    Check(git_commit_parent(&raw_parent, commit.get(), 0), "load revision parent");
    std::unique_ptr<git_commit, decltype(&git_commit_free)> parent(raw_parent, git_commit_free);
    return git_oid_equal(git_commit_tree_id(commit.get()), git_commit_tree_id(parent.get())) != 0;
}

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

void RepositoryWatcher::Watch(
    const std::filesystem::path& worktree, const std::filesystem::path& common_directory)
{
    Clear();
    _common_directory = std::filesystem::weakly_canonical(common_directory);
    auto watcher = std::make_unique<efsw::FileWatcher>();
    if (watcher->addWatch(worktree.string(), this, true) < 0)
        throw std::runtime_error("watch repository: " + efsw::Errors::Log::getLastErrorLog()); // GCOV_EXCL_LINE
    const auto relative = _common_directory.lexically_relative(std::filesystem::weakly_canonical(worktree));
    if (relative.empty() || *relative.begin() == "..")
    {
        if (watcher->addWatch(common_directory.string(), this, true) < 0)
            throw std::runtime_error("watch Git directory: " + efsw::Errors::Log::getLastErrorLog()); // GCOV_EXCL_LINE
    }
    watcher->watch();
    _watcher = std::move(watcher);
}

void RepositoryWatcher::Clear()
{
    _watcher.reset();
    _worktree_changed = false;
    _metadata_changed = false;
    _common_directory.clear();
}

RepositoryWatcher::Changes RepositoryWatcher::ConsumeChanges()
{
    return {_worktree_changed.exchange(false), _metadata_changed.exchange(false)};
}

void RepositoryWatcher::handleFileAction(efsw::WatchID, const std::string& directory,
    const std::string& filename, efsw::Action, const std::string&)
{
    const auto relative = (std::filesystem::path(directory) / filename)
                              .lexically_normal()
                              .lexically_relative(_common_directory);
    const bool metadata = relative.empty() || (!relative.is_absolute() && *relative.begin() != "..");
    if (metadata)
        _metadata_changed = true;
    else
        _worktree_changed = true;
    _wake.notify_one();
}

} // namespace Ggui::RepositoryInternal

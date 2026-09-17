// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <git2/index.h>
#include <git2/sys/errors.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
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
    const std::filesystem::path& worktree, const std::filesystem::path& common_directory,
    git_repository* repository)
{
    Clear();
    _common_directory = std::filesystem::weakly_canonical(common_directory);
    _worktree = std::filesystem::weakly_canonical(worktree);
    auto watcher = std::make_unique<efsw::FileWatcher>();
    const auto add_watch = [&](const std::filesystem::path& directory, bool recursive,
                               std::string_view action) {
        if (watcher->addWatch(directory.string(), this, recursive) < 0)
            throw std::runtime_error(std::string(action) + ": " + efsw::Errors::Log::getLastErrorLog());
    };
    const auto add_metadata_watches = [&] {
        // Direct Git files (HEAD, index, config, packed-refs, etc.) are
        // enough for the root watch. Ref/log/worktree trees are watched
        // recursively; object packs never need to trigger a refresh by
        // themselves and can be enormous in a large repository.
        add_watch(_common_directory, false, "watch Git directory");
        for (const std::string_view child : {"refs", "logs", "gg", "worktrees"})
        {
            const std::filesystem::path candidate = _common_directory / child;
            std::error_code error;
            if (std::filesystem::is_directory(candidate, error) && !error)
                add_watch(candidate, true, "watch Git metadata");
        }
    };
    const auto relative = _common_directory.lexically_relative(_worktree);
    const bool same_directory = _common_directory == _worktree;
    const bool common_inside_worktree = !same_directory && !relative.empty()
        && !relative.is_absolute() && *relative.begin() != "..";
    _split_worktree = common_inside_worktree;
    _excluded_worktree_child = common_inside_worktree
        ? (*relative.begin()).generic_string() : std::string{};
    if (same_directory)
    {
        // Bare repositories have no worktree to watch.
        add_metadata_watches();
    }
    else if (common_inside_worktree)
    {
        // A recursive root watch would descend into .git. Watch root files
        // non-recursively, then add recursive watches for each real worktree
        // child except the common Git directory. Git-ignored top-level
        // directories do not affect status and can contain very large build
        // trees, so leave them unwatched unless they contain tracked files.
        add_watch(_worktree, false, "watch repository");
        std::unordered_set<std::string> tracked_children;
        if (repository != nullptr)
        {
            git_index* raw_index = nullptr;
            if (git_repository_index(&raw_index, repository) == GIT_OK)
            {
                std::unique_ptr<git_index, decltype(&git_index_free)> index(raw_index, git_index_free);
                for (std::size_t position = 0; position < git_index_entrycount(index.get()); ++position)
                {
                    const git_index_entry* entry = git_index_get_byindex(index.get(), position);
                    if (entry == nullptr || entry->path == nullptr)
                        continue;
                    const std::string path(entry->path);
                    const std::size_t separator = path.find('/');
                    tracked_children.insert(path.substr(0, separator));
                }
            }
            else
                git_error_clear();
        }
        const auto ignored_without_tracked_files = [&](const std::filesystem::path& directory) {
            if (repository == nullptr)
                return false;
            const std::string relative = directory.lexically_relative(_worktree).generic_string();
            if (relative.empty() || tracked_children.contains(relative))
                return false;
            int ignored = 0;
            if (git_ignore_path_is_ignored(&ignored, repository, relative.c_str()) != GIT_OK)
            {
                git_error_clear();
                return false;
            }
            return ignored != 0;
        };
        std::error_code error;
        for (std::filesystem::directory_iterator entry(_worktree, error), end;
             !error && entry != end; entry.increment(error))
        {
            const std::filesystem::path child = entry->path();
            if (child.filename().generic_string() == _excluded_worktree_child)
                continue;
            std::error_code child_error;
            if (std::filesystem::is_directory(child, child_error) && !child_error)
            {
                if (ignored_without_tracked_files(child))
                {
                    _skipped_worktree_directories.push_back(child);
                    continue;
                }
                add_watch(child, true, "watch repository");
            }
        }
        if (error)
            throw std::runtime_error("watch repository: " + error.message()); // GCOV_EXCL_LINE
        add_metadata_watches();
    }
    else
    {
        // Linked worktrees keep the common Git directory outside the
        // checkout, so the worktree can remain recursive without including
        // Git's object database.
        add_watch(_worktree, true, "watch repository");
        add_metadata_watches();
    }
    watcher->watch();
    _watcher = std::move(watcher);
}

void RepositoryWatcher::Clear()
{
    _watcher.reset();
    _worktree_changed = false;
    _metadata_changed = false;
    _full_scan = false;
    _common_directory.clear();
    _worktree.clear();
    _split_worktree = false;
    _excluded_worktree_child.clear();
    std::lock_guard lock(_changes_mutex);
    _changed_paths.clear();
    _pending_watches.clear();
    _skipped_worktree_directories.clear();
}

RepositoryWatcher::Changes RepositoryWatcher::ConsumeChanges()
{
    Changes result;
    result.worktree = _worktree_changed.exchange(false);
    result.metadata = _metadata_changed.exchange(false);
    result.full_scan = _full_scan.exchange(false);
    std::vector<std::filesystem::path> pending_watches;
    {
        std::lock_guard lock(_changes_mutex);
        result.paths.swap(_changed_paths);
        pending_watches.swap(_pending_watches);
    }
    for (const std::filesystem::path& path : pending_watches)
        if (_watcher != nullptr)
            (void)_watcher->addWatch(path.string(), this, true);
    return result;
}

void RepositoryWatcher::handleFileAction(efsw::WatchID, const std::string& directory,
    const std::string& filename, efsw::Action action, const std::string& old_filename)
{
    const std::filesystem::path absolute = (std::filesystem::path(directory) / filename).lexically_normal();
    const auto common_relative = absolute.lexically_relative(_common_directory);
    const bool metadata = common_relative.empty()
        || (!common_relative.is_absolute() && *common_relative.begin() != "..");
    if (metadata)
    {
        _metadata_changed = true;
        const bool index_changed = absolute.filename() == "index" && absolute.parent_path() == _common_directory;
        if (index_changed)
        {
            // Re-enable directories that were skipped while ignored: an
            // external index update may have added a tracked file there.
            std::lock_guard lock(_changes_mutex);
            for (const std::filesystem::path& skipped : _skipped_worktree_directories)
                if (std::ranges::find(_pending_watches, skipped) == _pending_watches.end())
                    _pending_watches.push_back(skipped);
        }
        if (action == efsw::Actions::Add && absolute.parent_path() == _common_directory)
        {
            const std::string name = absolute.filename().generic_string();
            if (name == "refs" || name == "logs" || name == "gg" || name == "worktrees")
            {
                std::error_code error;
                if (std::filesystem::is_directory(absolute, error) && !error)
                {
                    std::lock_guard lock(_changes_mutex);
                    if (std::ranges::find(_pending_watches, absolute) == _pending_watches.end())
                        _pending_watches.push_back(absolute);
                }
            }
        }
    }
    else
    {
        _worktree_changed = true;
        if (_split_worktree && action == efsw::Actions::Add
            && absolute.parent_path() == _worktree
            && absolute.filename().generic_string() != _excluded_worktree_child)
        {
            std::error_code error;
            if (std::filesystem::is_directory(absolute, error) && !error)
            {
                std::lock_guard lock(_changes_mutex);
                if (std::ranges::find(_pending_watches, absolute) == _pending_watches.end())
                    _pending_watches.push_back(absolute);
            }
        }
        const auto remember = [&](const std::filesystem::path& value)
        {
            const auto relative = value.lexically_normal().lexically_relative(_worktree);
            if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            {
                _full_scan = true;
                return;
            }
            std::error_code error;
            if (std::filesystem::is_directory(value, error))
            {
                _full_scan = true;
                return;
            }
            std::lock_guard lock(_changes_mutex);
            const std::string path = relative.generic_string();
            if (std::ranges::find(_changed_paths, path) == _changed_paths.end())
            {
                if (_changed_paths.size() >= 4096)
                    _full_scan = true;
                else
                    _changed_paths.push_back(path);
            }
        };
        // An ignore-file edit can change the status of many paths that did
        // not themselves emit filesystem events, so it cannot be reconciled
        // with a single-path status query.
        if (absolute.filename() == ".gitignore" || absolute.filename() == ".gitattributes"
            || absolute.filename() == ".gitmodules")
        {
            _full_scan = true;
            if (absolute.filename() == ".gitignore")
            {
                std::lock_guard lock(_changes_mutex);
                for (const std::filesystem::path& skipped : _skipped_worktree_directories)
                    if (std::ranges::find(_pending_watches, skipped) == _pending_watches.end())
                        _pending_watches.push_back(skipped);
            }
        }
        remember(absolute);
        if (!old_filename.empty())
            remember(std::filesystem::path(directory) / old_filename);
    }
    _wake.notify_one();
}

} // namespace Ggui::RepositoryInternal

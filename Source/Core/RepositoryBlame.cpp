// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <git2/blame.h>
#if __has_include(<git2-experimental/sys/errors.h>)
#include <git2-experimental/sys/errors.h>
#else
#include <git2/sys/errors.h>
#endif

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace Ggui
{
using namespace RepositoryInternal;

namespace
{
struct BlameDeleter
{
    void operator()(git_blame* value) const { git_blame_free(value); }
};
using BlamePtr = std::unique_ptr<git_blame, BlameDeleter>;

std::string CommitSummary(git_repository* repository, const git_oid& oid)
{
    git_commit* raw_commit = nullptr;
    if (git_commit_lookup(&raw_commit, repository, &oid) != GIT_OK)
    {
        // Blame data can still be useful when an origin object has been
        // pruned; treat the optional extended metadata as unavailable.
        git_error_clear();
        return {};
    }
    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
    const char* summary = git_commit_summary(commit.get());
    return summary == nullptr ? "" : summary;
}

std::string FirstParent(git_repository* repository, const git_oid& oid)
{
    git_commit* raw_commit = nullptr;
    if (git_commit_lookup(&raw_commit, repository, &oid) != GIT_OK)
    {
        git_error_clear();
        return {};
    }
    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
    if (git_commit_parentcount(commit.get()) == 0)
        return {};
    return OidString(*git_commit_parent_id(commit.get(), 0));
}

std::string SignatureName(const git_signature* signature)
{
    return signature == nullptr || signature->name == nullptr ? "" : signature->name;
}

std::string SignatureEmail(const git_signature* signature)
{
    return signature == nullptr || signature->email == nullptr ? "" : signature->email;
}

bool IsWorkingTreeRevision(std::string_view revision)
{
    return revision.starts_with("working-tree:");
}

// The commit working-tree edits are relative to: gg's @, or HEAD when gg has
// no workspace. Zero while HEAD is unborn.
git_oid ActiveCommit(git_repository* repository, gg_repository* gg_repository)
{
    git_oid oid{};
    if (gg_repository_working_copy(&oid, gg_repository) == GIT_OK)
        return oid;
    git_error_clear();
    git_reference* raw_head = nullptr;
    if (git_repository_head(&raw_head, repository) != GIT_OK)
    {
        git_error_clear();
        return {};
    }
    std::unique_ptr<git_reference, decltype(&git_reference_free)> head(raw_head, git_reference_free);
    if (const git_oid* target = git_reference_target(head.get()); target != nullptr)
        oid = *target;
    return oid;
}

// Splits file contents into lines without their newline, like libgit2's
// blame lines. A trailing newline does not start another line.
std::vector<std::string_view> ContentLines(std::string_view contents)
{
    std::vector<std::string_view> result;
    std::size_t begin = 0;
    while (begin < contents.size())
    {
        const std::size_t end = std::min(contents.find('\n', begin), contents.size());
        result.push_back(contents.substr(begin, end - begin));
        begin = end + 1;
    }
    return result;
}
} // namespace

void RepositoryEngine::Impl::LoadBlameFile(const Ggui::LoadBlame& command, git_repository* repository,
    gg_repository* gg_repository, std::uint64_t snapshot_generation, std::uint64_t request_generation,
    std::uint64_t request_session)
{
    const auto current = [&] {
        return request_session == session.load() && request_generation == inspector_request.load()
            && snapshot_generation == generation.load();
    };
    if (!current())
        return;

    // The working tree is blamed as @ plus its uncommitted edits: lines that
    // differ from @ are reported as uncommitted rather than refusing the file.
    const bool working_tree = IsWorkingTreeRevision(command.revision);
    git_oid newest{};
    if (working_tree)
        newest = ActiveCommit(repository, gg_repository);
    else
        Check(gg_repository_resolve(&newest, gg_repository, command.revision.c_str()), "resolve blame revision");
    if (!current())
        return;

    // Keep commit metadata with the blame result. The history panel may only
    // materialize a bounded window, while the blame view must still identify
    // and walk the exact file snapshot requested by the user.
    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(nullptr, git_commit_free);
    if (!git_oid_is_zero(&newest))
    {
        git_commit* raw_commit = nullptr;
        Check(git_commit_lookup(&raw_commit, repository, &newest), "load blame revision");
        commit.reset(raw_commit);
    }

    BlamePtr blame;
    if (commit != nullptr)
    {
        git_blame_options options = GIT_BLAME_OPTIONS_INIT;
        options.newest_commit = newest;
        git_blame* raw_blame = nullptr;
        const int status = git_blame_file(&raw_blame, repository, command.path.c_str(), &options);
        // A file that is new in the working tree has no committed lines.
        if (working_tree && status == GIT_ENOTFOUND)
            git_error_clear();
        else
            Check(status, "load blame");
        blame.reset(raw_blame);
    }

    std::string worktree_contents;
    std::vector<std::string_view> worktree_lines;
    BlamePtr worktree_blame;
    if (working_tree)
    {
        bool binary = false;
        worktree_contents = ReadWorktreeFile(repository, command.path, binary);
        if (binary)
            throw std::runtime_error("binary files cannot be blamed");
        worktree_lines = ContentLines(worktree_contents);
        if (blame != nullptr && !worktree_contents.empty())
        {
            git_blame* raw_blame = nullptr;
            Check(git_blame_buffer(&raw_blame, blame.get(), worktree_contents.data(), worktree_contents.size()),
                "blame working-tree file");
            worktree_blame.reset(raw_blame);
        }
    }

    BlameResult result;
    result.generation = snapshot_generation;
    result.revision = command.revision;
    result.path = command.path;
    result.working_tree = working_tree;
    if (commit != nullptr)
    {
        result.viewed_revision.oid = OidString(newest);
        const git_signature* signature = git_commit_author(commit.get());
        result.viewed_revision.author = SignatureName(signature);
        result.viewed_revision.author_email = SignatureEmail(signature);
        result.viewed_revision.timestamp = signature == nullptr ? 0 : signature->when.time;
        const char* message = git_commit_message(commit.get());
        if (message != nullptr)
            result.viewed_revision.description = message;
        result.viewed_revision.parents.reserve(git_commit_parentcount(commit.get()));
        for (unsigned int parent = 0; parent < git_commit_parentcount(commit.get()); ++parent)
            result.viewed_revision.parents.push_back(OidString(*git_commit_parent_id(commit.get(), parent)));
    }
    const std::size_t line_count = working_tree ? worktree_lines.size() : git_blame_linecount(blame.get());
    result.lines.reserve(line_count);
    std::unordered_map<std::string, std::string> previous_summaries;
    std::unordered_map<std::string, std::string> blame_before_revisions;
    for (std::size_t index = 0; index < line_count; ++index)
    {
        const std::size_t line_number = index + 1;
        if (working_tree)
        {
            // Buffer blame keeps hunks but not line text; lines that are not
            // in @ have no hunk or a hunk without a commit.
            const git_blame_hunk* hunk = worktree_blame == nullptr ? nullptr
                : git_blame_hunk_byline(worktree_blame.get(), line_number);
            if (hunk == nullptr || git_oid_is_zero(&hunk->final_commit_id) != 0)
            {
                BlameLine value;
                value.line = value.original_line = line_number;
                value.contents = worktree_lines[index];
                value.uncommitted = true;
                // Before an uncommitted edit, the file is the one in @.
                if (commit != nullptr && blame != nullptr)
                {
                    value.blame_before_revision = result.viewed_revision.oid;
                    value.blame_before_path = command.path;
                }
                result.lines.push_back(std::move(value));
                continue;
            }
        }
        const git_blame_hunk* hunk = git_blame_hunk_byline(
            working_tree ? worktree_blame.get() : blame.get(), line_number);
        const git_blame_line* line = working_tree ? nullptr : git_blame_line_byindex(blame.get(), line_number);
        if (hunk == nullptr || (!working_tree && line == nullptr))
        {
            // libgit2 can expose the terminal empty line after a trailing
            // newline in git_blame_linecount(), even though that line is not
            // represented by a blame hunk. It is not source content and
            // should not turn an otherwise valid blame request into an error.
            if (line_number == line_count)
                break;
            throw std::runtime_error("load blame line");
        }

        BlameLine value;
        // Keep both line numbers in the same 1-based convention used by
        // libgit2 and by Git's blame output.
        value.line = line_number;
        value.original_line = hunk->orig_start_line_number + (line_number - hunk->final_start_line_number);
        value.revision = OidString(hunk->final_commit_id);
        value.author = SignatureName(hunk->final_signature);
        value.author_email = SignatureEmail(hunk->final_signature);
        value.timestamp = hunk->final_signature == nullptr ? 0 : hunk->final_signature->when.time;
        value.summary = hunk->summary == nullptr ? "" : hunk->summary;
        if (working_tree)
            value.contents = worktree_lines[index];
        else
            value.contents.assign(line->ptr == nullptr ? "" : line->ptr, line->len);
        value.boundary = hunk->boundary != 0;
        value.previous_line = hunk->orig_start_line_number
            + (line_number - hunk->final_start_line_number);
        if (git_oid_is_zero(&hunk->orig_commit_id) == 0)
            value.previous_revision = OidString(hunk->orig_commit_id);
        value.previous_path = hunk->orig_path == nullptr ? "" : hunk->orig_path;
        value.previous_author = SignatureName(hunk->orig_signature);
        value.previous_author_email = SignatureEmail(hunk->orig_signature);
        value.previous_timestamp = hunk->orig_signature == nullptr ? 0 : hunk->orig_signature->when.time;
        if (!value.previous_revision.empty() && value.previous_revision != value.revision)
        {
            const auto [found, inserted] = previous_summaries.try_emplace(value.previous_revision);
            if (inserted)
                found->second = CommitSummary(repository, hunk->orig_commit_id);
            value.previous_summary = found->second;
        }

        // `orig_commit_id` is useful source/copy information, but it is not
        // the commit immediately before the change owning this line. Popular
        // blame views offer a per-range "view blame before this change"
        // action, so retain the first parent of the final (blamed) commit as
        // an explicit navigation target. Cache it because a hunk commonly
        // contains many lines.
        const std::string final_revision = value.revision;
        const auto [before, inserted] = blame_before_revisions.try_emplace(final_revision);
        if (inserted && !final_revision.empty())
            before->second = FirstParent(repository, hunk->final_commit_id);
        value.blame_before_revision = before->second;
        value.blame_before_path = command.path;
        result.lines.push_back(std::move(value));
    }

    if (current())
        Post(BlameReady{std::move(result)});
}

} // namespace Ggui

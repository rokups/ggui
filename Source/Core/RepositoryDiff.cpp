// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <git2/sys/errors.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Ggui
{
using namespace RepositoryInternal;

void RepositoryEngine::Impl::LoadFile(const LoadFileContent& command)
{
    git_oid oid{};
    Check(gg_repository_resolve(&oid, gg, command.revision.c_str()), "resolve file revision");
    git_commit* raw_commit = nullptr;
    Check(git_commit_lookup(&raw_commit, git.get(), &oid), "load file revision");
    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
    git_tree* raw_tree = nullptr;
    Check(git_commit_tree(&raw_tree, commit.get()), "load file revision tree");
    std::unique_ptr<git_tree, decltype(&git_tree_free)> tree(raw_tree, git_tree_free);
    git_tree_entry* raw_entry = nullptr;
    const int found = git_tree_entry_bypath(&raw_entry, tree.get(), command.path.c_str());
    if (found == GIT_ENOTFOUND)
    {
        git_error_clear();
        throw std::runtime_error("file does not exist in the selected change");
    }
    Check(found, "load file entry");
    std::unique_ptr<git_tree_entry, decltype(&git_tree_entry_free)> entry(raw_entry, git_tree_entry_free);
    if (git_tree_entry_type(entry.get()) != GIT_OBJECT_BLOB)
        throw std::runtime_error("selected path is not a file");
    git_blob* raw_blob = nullptr;
    Check(git_blob_lookup(&raw_blob, git.get(), git_tree_entry_id(entry.get())), "load file contents");
    std::unique_ptr<git_blob, decltype(&git_blob_free)> blob(raw_blob, git_blob_free);
    const char* contents = static_cast<const char*>(git_blob_rawcontent(blob.get()));
    Post(FileContentReady{command.revision, command.path,
        std::string(contents == nullptr ? "" : contents, static_cast<std::size_t>(git_blob_rawsize(blob.get())))});
}

void RepositoryEngine::Impl::LoadPatch(const LoadDiff& command)
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
    std::unique_ptr<git_tree, decltype(&git_tree_free)> comparison_tree(nullptr, git_tree_free);
    git_tree* content_old_tree = old_tree.get();
    git_tree* content_new_tree = new_tree.get();
    if (!command.compare_to.empty())
    {
        git_oid compare_oid{};
        Check(gg_repository_resolve(&compare_oid, gg, command.compare_to.c_str()), "resolve comparison revision");
        git_commit* raw_compare = nullptr;
        Check(git_commit_lookup(&raw_compare, git.get(), &compare_oid), "load comparison revision");
        std::unique_ptr<git_commit, decltype(&git_commit_free)> compare(raw_compare, git_commit_free);
        git_tree* raw_compare_tree = nullptr;
        Check(git_commit_tree(&raw_compare_tree, compare.get()), "load comparison tree");
        comparison_tree.reset(raw_compare_tree);
        content_old_tree = new_tree.get();
        content_new_tree = comparison_tree.get();
    }
    git_diff_find_options find_options{};
    Check(git_diff_find_options_init(&find_options, GIT_DIFF_FIND_OPTIONS_VERSION),
        "initialize rename detection");
    find_options.flags = GIT_DIFF_FIND_RENAMES;
    const auto create_diff = [&](git_tree* old_value, git_tree* new_value) {
        git_diff* raw_diff = nullptr;
        Check(git_diff_tree_to_tree(&raw_diff, git.get(), old_value, new_value, nullptr), "create diff");
        std::unique_ptr<git_diff, decltype(&git_diff_free)> value(raw_diff, git_diff_free);
        Check(git_diff_find_similar(value.get(), &find_options), "find renamed files");
        return value;
    };
    std::unique_ptr<git_diff, decltype(&git_diff_free)> diff =
        create_diff(content_old_tree, content_new_tree);
    std::unique_ptr<git_diff, decltype(&git_diff_free)> status_diff(nullptr, git_diff_free);
    git_diff* files_diff = diff.get();
    if (command.file_comparison)
    {
        status_diff = create_diff(old_tree.get(), new_tree.get());
        files_diff = status_diff.get();
    }
    DiffResult result{generation, command.revision, command.path, {}, {}, false, {}, command.compare_to,
        command.file_comparison};
    result.options = command.options;
    for (size_t index = 0; index < git_diff_num_deltas(files_diff); ++index)
    {
        const git_diff_delta* delta = git_diff_get_delta(files_diff, index);
        const char* old_path = delta->old_file.path == nullptr ? "" : delta->old_file.path;
        const char* new_path = delta->new_file.path == nullptr ? old_path : delta->new_file.path;
        result.files.push_back({old_path, new_path, delta->status, false});
    }
    git_oid working_copy{};
    if (command.compare_to.empty() && !command.file_comparison
        && gg_repository_working_copy(&working_copy, gg) == GIT_OK
        && git_oid_equal(&oid, &working_copy) != 0)
    {
        gg_status_options options = GG_STATUS_OPTIONS_INIT;
        Status status;
        Check(gg_repository_status(&status.value, gg, &options), "load untracked files");
        for (size_t index = 0; index < status.value.entry_count; ++index)
        {
            const gg_status_entry& entry = status.value.entries[index];
            const std::string_view path = entry.new_path == nullptr ? "" : entry.new_path;
            if (entry.status != GIT_DELTA_UNTRACKED || path.empty()
                || std::ranges::any_of(result.files, [&](const StatusEntry& file) { return file.path == path; }))
                continue;
            result.files.push_back({{}, std::string(path), GIT_DELTA_UNTRACKED, false});
            if (result.path == path)
                result.selected_status = GIT_DELTA_UNTRACKED;
        }
    }
    if (command.file_comparison)
        result.selected_status = GIT_DELTA_UNMODIFIED;
    if (command.fallback_to_first
        && std::ranges::none_of(result.files, [&](const StatusEntry& file) { return file.path == result.path; }))
        result.path = result.files.empty() ? "" : result.files.front().path;
    if (!result.path.empty())
    {
        const git_diff_delta* selected_delta = nullptr;
        size_t selected_index = git_diff_num_deltas(diff.get());
        for (size_t index = 0; index < git_diff_num_deltas(diff.get()); ++index)
        {
            const git_diff_delta* candidate = git_diff_get_delta(diff.get(), index);
            const std::string_view candidate_old =
                candidate->old_file.path == nullptr ? "" : candidate->old_file.path;
            const std::string_view candidate_new =
                candidate->new_file.path == nullptr ? "" : candidate->new_file.path;
            if (candidate_old == result.path || candidate_new == result.path)
            {
                selected_delta = candidate;
                selected_index = index;
                break;
            }
        }
        const char* old_path = selected_delta == nullptr || selected_delta->old_file.path == nullptr
            ? result.path.c_str()
            : selected_delta->old_file.path;
        const char* new_path = selected_delta == nullptr || selected_delta->new_file.path == nullptr
            ? result.path.c_str()
            : selected_delta->new_file.path;
        result.before = BlobText(git.get(), content_old_tree, old_path, result.binary);
        result.after = BlobText(git.get(), content_new_tree, new_path, result.binary);
        if (selected_delta != nullptr)
        {
            result.selected_status = selected_delta->status;
            if (!git_oid_is_zero(&selected_delta->old_file.id))
                result.old_oid = OidString(selected_delta->old_file.id);
            if (!git_oid_is_zero(&selected_delta->new_file.id))
                result.new_oid = OidString(selected_delta->new_file.id);
            result.old_mode = selected_delta->old_file.mode;
            result.new_mode = selected_delta->new_file.mode;

            git_patch* raw_selected_patch = nullptr;
            Check(git_patch_from_diff(&raw_selected_patch, diff.get(), selected_index), "load selected file patch");
            std::unique_ptr<git_patch, decltype(&git_patch_free)> selected_patch(raw_selected_patch, git_patch_free);
            git_buf patch = GIT_BUF_INIT;
            Check(git_patch_to_buf(&patch, selected_patch.get()), "format selected file patch");
            result.patch.assign(patch.ptr == nullptr ? "" : patch.ptr, patch.size);
            git_buf_dispose(&patch);
        }

        git_diff* display_diff = diff.get();
        std::unique_ptr<git_diff, decltype(&git_diff_free)> filtered_diff(nullptr, git_diff_free);
        if (command.options.context_lines < 0 || command.options.whitespace_mode != DiffWhitespaceMode::Normal
            || command.options.context_lines != 3)
        {
            git_diff_options options = GIT_DIFF_OPTIONS_INIT;
            options.context_lines = command.options.context_lines < 0
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(command.options.context_lines);
            if (command.options.whitespace_mode == DiffWhitespaceMode::IgnoreWhitespace)
                options.flags |= GIT_DIFF_IGNORE_WHITESPACE_CHANGE;
            else if (command.options.whitespace_mode == DiffWhitespaceMode::IgnoreAllWhitespace)
                options.flags |= GIT_DIFF_IGNORE_WHITESPACE;
            git_diff* raw_filtered = nullptr;
            Check(git_diff_tree_to_tree(&raw_filtered, git.get(), content_old_tree, content_new_tree, &options),
                "create filtered diff");
            filtered_diff.reset(raw_filtered);
            Check(git_diff_find_similar(filtered_diff.get(), &find_options), "find filtered renamed files");
            display_diff = filtered_diff.get();
        }

        const bool submodule = (result.old_mode & 0170000U) == 0160000U || (result.new_mode & 0170000U) == 0160000U;
        if (!result.binary && !submodule)
        {
            size_t display_index = git_diff_num_deltas(display_diff);
            for (size_t index = 0; index < git_diff_num_deltas(display_diff); ++index)
            {
                const git_diff_delta* candidate = git_diff_get_delta(display_diff, index);
                const std::string_view candidate_old =
                    candidate->old_file.path == nullptr ? "" : candidate->old_file.path;
                const std::string_view candidate_new =
                    candidate->new_file.path == nullptr ? "" : candidate->new_file.path;
                if (candidate_old == result.path || candidate_new == result.path)
                {
                    display_index = index;
                    break;
                }
            }

            std::string display_before;
            std::string display_after;
            if (display_index < git_diff_num_deltas(display_diff))
            {
                git_patch* raw_file_patch = nullptr;
                Check(git_patch_from_diff(&raw_file_patch, display_diff, display_index), "load file patch");
                std::unique_ptr<git_patch, decltype(&git_patch_free)> file_patch(raw_file_patch, git_patch_free);
                if (file_patch != nullptr)
                {
                    for (size_t hunk = 0; hunk < git_patch_num_hunks(file_patch.get()); ++hunk)
                    {
                        const git_diff_hunk* raw_hunk = nullptr;
                        size_t lines = 0;
                        Check(git_patch_get_hunk(&raw_hunk, &lines, file_patch.get(), hunk), "load diff hunk");
                        for (size_t line = 0; line < lines; ++line)
                        {
                            const git_diff_line* raw_line = nullptr;
                            Check(git_patch_get_line_in_hunk(&raw_line, file_patch.get(), hunk, line),
                                "load diff line");
                            if (raw_line->origin != GIT_DIFF_LINE_CONTEXT
                                && raw_line->origin != GIT_DIFF_LINE_ADDITION
                                && raw_line->origin != GIT_DIFF_LINE_DELETION)
                                continue;
                            result.lines.push_back(DiffLineFromRaw(*raw_line, static_cast<int>(hunk)));
                            if (raw_line->origin != GIT_DIFF_LINE_ADDITION)
                                display_before.append(raw_line->content, raw_line->content_len);
                            if (raw_line->origin != GIT_DIFF_LINE_DELETION)
                                display_after.append(raw_line->content, raw_line->content_len);
                        }
                    }
                }
            }
            if (display_before.ends_with('\n') && display_after.ends_with('\n'))
                result.lines.push_back({});
            if (command.options.context_lines >= 0)
            {
                if (result.selected_status != GIT_DELTA_ADDED && result.selected_status != GIT_DELTA_UNTRACKED
                    && result.selected_status != GIT_DELTA_DELETED)
                {
                    result.full_before = result.before;
                    result.full_after = result.after;
                }
                if (display_before.empty() && display_after.empty())
                    display_before = display_after = result.after.empty() ? result.before : result.after;
                result.before = std::move(display_before);
                result.after = std::move(display_after);
            }
        }
    }
    Post(DiffReady{std::move(result)});
}

} // namespace Ggui

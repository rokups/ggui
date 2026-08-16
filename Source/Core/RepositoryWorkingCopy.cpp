// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Ggui
{
using namespace RepositoryInternal;

void RepositoryEngine::Impl::ApplyPatchText(const ApplyPatch& command)
{
    Sync();
    std::string text = command.text;
    if (text.empty() && !command.path.empty())
    {
        std::ifstream input(command.path, std::ios::binary);
        if (!input)
            throw std::runtime_error("could not open patch file");
        text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    if (text.empty())
        throw std::runtime_error("patch is empty");

    git_diff* raw_patch = nullptr;
    Check(git_diff_from_buffer(&raw_patch, text.data(), text.size(), nullptr), "parse patch");
    std::unique_ptr<git_diff, decltype(&git_diff_free)> patch(raw_patch, git_diff_free);
    Check(git_apply(git.get(), patch.get(), GIT_APPLY_LOCATION_WORKDIR, nullptr), "apply patch");
    Sync();
    PublishSnapshot();
}

void RepositoryEngine::Impl::ResolveConflictFile(const ResolveConflict& command)
{
    Sync();
    const std::filesystem::path path = std::filesystem::path(command.path).lexically_normal();
    if (command.revision.empty() || path.empty() || path.is_absolute()
        || std::ranges::any_of(path, [](const std::filesystem::path& part) { return part == ".."; }))
        throw std::runtime_error("conflict resolution requires a revision and repository-relative path");

    git_oid revision_oid{};
    Check(gg_repository_resolve(&revision_oid, gg, command.revision.c_str()), "resolve conflicted revision");
    Conflicts conflicts;
    Check(gg_repository_conflicts(&conflicts.value, gg, &revision_oid), "load revision conflicts");
    bool conflicted = false;
    for (size_t index = 0; index < conflicts.value.count; ++index)
        conflicted |= conflicts.value.items[index].path != nullptr && command.path == conflicts.value.items[index].path;
    if (!conflicted)
        throw std::runtime_error("selected file is no longer conflicted");

    git_commit* raw_revision = nullptr;
    Check(git_commit_lookup(&raw_revision, git.get(), &revision_oid), "load conflicted revision");
    std::unique_ptr<git_commit, decltype(&git_commit_free)> revision(raw_revision, git_commit_free);
    git_tree* raw_tree = nullptr;
    Check(git_commit_tree(&raw_tree, revision.get()), "load conflicted revision tree");
    std::unique_ptr<git_tree, decltype(&git_tree_free)> tree(raw_tree, git_tree_free);
    git_index* raw_index = nullptr;
    git_index_options index_options = GIT_INDEX_OPTIONS_INIT;
    index_options.oid_type = git_repository_oid_type(git.get());
    Check(git_index_new(&raw_index, &index_options), "create conflict resolution index");
    std::unique_ptr<git_index, decltype(&git_index_free)> index(raw_index, git_index_free);
    Check(git_index_read_tree(index.get(), tree.get()), "load conflict resolution tree");

    if (command.present)
    {
        const git_index_entry* existing = git_index_get_bypath(index.get(), command.path.c_str(), 0);
        git_oid blob_oid{};
        Check(git_blob_create_frombuffer(
                  &blob_oid, git.get(), command.contents.data(), command.contents.size()),
            "write resolved file");
        git_index_entry entry{};
        entry.id = blob_oid;
        entry.mode = GIT_FILEMODE_BLOB;
        if (existing != nullptr)
            entry.mode = existing->mode;
        entry.path = command.path.c_str();
        Check(git_index_add(index.get(), &entry), "add resolved file");
    }
    else
    {
        const int removed = git_index_remove_bypath(index.get(), command.path.c_str());
        if (removed != GIT_ENOTFOUND)
            Check(removed, "remove resolved file");
    }

    git_oid tree_oid{};
    Check(git_index_write_tree_to(&tree_oid, index.get(), git.get()), "write conflict resolution tree");
    git_tree* raw_resolved_tree = nullptr;
    Check(git_tree_lookup(&raw_resolved_tree, git.get(), &tree_oid), "load conflict resolution tree");
    std::unique_ptr<git_tree, decltype(&git_tree_free)> resolved_tree(raw_resolved_tree, git_tree_free);
    git_oid source_oid{};
    Check(git_commit_create(&source_oid, git.get(), nullptr, git_commit_author(revision.get()),
              git_commit_committer(revision.get()), nullptr, "ggui conflict resolution", resolved_tree.get(), 0,
              nullptr),
        "create conflict resolution source");

    const std::vector<std::string> paths{command.path};
    const StringArray filesets(paths);
    const std::string source = OidString(source_oid);
    gg_restore_options options = GG_RESTORE_OPTIONS_INIT;
    options.from = source.c_str();
    options.into = command.revision.c_str();
    options.filesets = filesets.Get();
    Mutation mutation;
    gg_operation_options operation = OperationOptions();
    Check(gg_repository_restore(&mutation.value, gg, &options, &operation), "restore conflict resolution");
    PublishSnapshot();
}

void RepositoryEngine::Impl::RevertFileChange(const RevertFile& command)
{
    Sync();
    if (command.source.empty() || command.path.empty())
        throw std::runtime_error("revert file requires a source change and path");

    git_oid source_oid{};
    Check(gg_repository_resolve(&source_oid, gg, command.source.c_str()), "resolve file revert source");
    git_commit* raw_source = nullptr;
    Check(git_commit_lookup(&raw_source, git.get(), &source_oid), "load file revert source");
    std::unique_ptr<git_commit, decltype(&git_commit_free)> source(raw_source, git_commit_free);
    git_tree* raw_source_tree = nullptr;
    Check(git_commit_tree(&raw_source_tree, source.get()), "load file revert source tree");
    std::unique_ptr<git_tree, decltype(&git_tree_free)> source_tree(raw_source_tree, git_tree_free);
    git_tree* raw_parent_tree = nullptr;
    if (git_commit_parentcount(source.get()) != 0)
    {
        git_commit* raw_parent = nullptr;
        Check(git_commit_parent(&raw_parent, source.get(), 0), "load file revert parent");
        std::unique_ptr<git_commit, decltype(&git_commit_free)> parent(raw_parent, git_commit_free);
        Check(git_commit_tree(&raw_parent_tree, parent.get()), "load file revert parent tree");
    }
    std::unique_ptr<git_tree, decltype(&git_tree_free)> parent_tree(raw_parent_tree, git_tree_free);

    std::vector<std::string> paths{command.path};
    if (!command.old_path.empty() && command.old_path != command.path)
        paths.push_back(command.old_path);
    std::vector<char*> pathspec;
    for (std::string& path : paths)
        pathspec.push_back(path.data());
    git_diff_options options = GIT_DIFF_OPTIONS_INIT;
    options.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH;
    if (!command.lines.empty())
        options.context_lines = 0;
    options.pathspec = {pathspec.data(), pathspec.size()};
    git_diff* raw_diff = nullptr;
    Check(git_diff_tree_to_tree(
              &raw_diff, git.get(), source_tree.get(), parent_tree.get(), &options),
        "create inverse file diff");
    std::unique_ptr<git_diff, decltype(&git_diff_free)> diff(raw_diff, git_diff_free);
    if (git_diff_num_deltas(diff.get()) == 0)
        throw std::runtime_error("selected file has no changes in the source change");
    std::vector<bool> selected_hunks;
    if (!command.lines.empty())
    {
        for (std::size_t delta = 0; delta < git_diff_num_deltas(diff.get()); ++delta)
        {
            git_patch* raw_patch = nullptr;
            Check(git_patch_from_diff(&raw_patch, diff.get(), delta), "load inverse file patch");
            std::unique_ptr<git_patch, decltype(&git_patch_free)> patch(raw_patch, git_patch_free);
            for (std::size_t hunk = 0; hunk < git_patch_num_hunks(patch.get()); ++hunk)
            {
                const git_diff_hunk* raw_hunk = nullptr;
                std::size_t count = 0;
                Check(git_patch_get_hunk(&raw_hunk, &count, patch.get(), hunk), "load inverse file hunk");
                bool selected = false;
                for (std::size_t line = 0; line < count && !selected; ++line)
                {
                    const git_diff_line* raw_line = nullptr;
                    Check(git_patch_get_line_in_hunk(&raw_line, patch.get(), hunk, line),
                        "load inverse file line");
                    const DiffLine reverse = DiffLineFromRaw(*raw_line, static_cast<int>(hunk));
                    selected = std::ranges::any_of(command.lines,
                        [&](const DiffLine& forward) { return SameInverseLine(forward, reverse); });
                }
                selected_hunks.push_back(selected);
            }
        }
        if (std::ranges::none_of(selected_hunks, [](bool selected) { return selected; }))
            throw std::runtime_error("selected hunk no longer matches the source change");
    }
    struct HunkSelection
    {
        const std::vector<bool>& selected;
        std::size_t index = 0;
    } selection{selected_hunks};
    git_apply_options apply_options = GIT_APPLY_OPTIONS_INIT;
    if (!selected_hunks.empty())
    {
        apply_options.hunk_cb = [](const git_diff_hunk*, void* payload) {
            HunkSelection& value = *static_cast<HunkSelection*>(payload);
            return value.index < value.selected.size() && value.selected[value.index++] ? 0 : 1;
        };
        apply_options.payload = &selection;
    }
    Check(git_apply(git.get(), diff.get(), GIT_APPLY_LOCATION_WORKDIR,
              selected_hunks.empty() ? nullptr : &apply_options),
        "apply inverse file diff");
    Sync();
    PublishSnapshot();
}

void RepositoryEngine::Impl::DeleteWorkingFile(const DeleteFile& command)
{
    Sync();
    const std::filesystem::path relative = std::filesystem::path(command.path).lexically_normal();
    if (relative.empty() || relative.is_absolute()
        || std::ranges::any_of(relative, [](const std::filesystem::path& part) { return part == ".."; }))
        throw std::runtime_error("delete file requires a repository-relative path");
    const char* workdir = git_repository_workdir(git.get());
    if (workdir == nullptr)
        throw std::runtime_error("repository has no working directory"); // GCOV_EXCL_LINE: bare repos are rejected
    const std::filesystem::path target = std::filesystem::path(workdir) / relative;
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(target, error);
    if (error || status.type() == std::filesystem::file_type::not_found)
        throw std::runtime_error("working-copy file does not exist");
    if (std::filesystem::is_directory(status))
        throw std::runtime_error("working-copy path is a directory");
    if (!std::filesystem::remove(target, error) || error)
        throw std::runtime_error("could not delete working-copy file"); // GCOV_EXCL_LINE: filesystem race/failure
    Sync();
    PublishSnapshot();
}

void RepositoryEngine::Impl::MoveDiffSelection(const MoveDiffLines& command, bool revert)
{
    Sync();
    if (command.path.empty() || command.lines.empty())
        throw std::runtime_error("no changed lines selected");

    git_oid source_oid{};
    git_oid destination_oid{};
    Check(gg_repository_resolve(&source_oid, gg, command.source.c_str()), "resolve line move source");
    if (revert)
    {
        git_oid working_copy_oid{};
        Check(gg_repository_resolve(&working_copy_oid, gg, "@"), "resolve working copy");
        if (git_oid_equal(&source_oid, &working_copy_oid) == 0)
            throw std::runtime_error("lines can only be reverted from the working copy");
    }
    else
        Check(gg_repository_resolve(&destination_oid, gg, command.destination.c_str()),
            "resolve line move destination");

    git_commit* raw_source = nullptr;
    git_commit* raw_destination = nullptr;
    Check(git_commit_lookup(&raw_source, git.get(), &source_oid), "load line move source");
    if (!revert)
        Check(git_commit_lookup(&raw_destination, git.get(), &destination_oid), "load line move destination");
    std::unique_ptr<git_commit, decltype(&git_commit_free)> source(raw_source, git_commit_free);
    std::unique_ptr<git_commit, decltype(&git_commit_free)> destination(raw_destination, git_commit_free);
    if (!revert && git_commit_parentcount(source.get()) != 1)
        throw std::runtime_error("line move source must have exactly one parent");

    const git_oid* source_parent_oid = git_commit_parentcount(source.get()) == 0
        ? nullptr
        : git_commit_parent_id(source.get(), 0);
    const bool move_to_parent = !revert && git_oid_equal(source_parent_oid, &destination_oid) != 0;
    const bool move_to_child = !revert && git_commit_parentcount(destination.get()) == 1
        && git_oid_equal(git_commit_parent_id(destination.get(), 0), &source_oid) != 0;
    if (!revert && !move_to_parent && !move_to_child)
        throw std::runtime_error("line moves require an adjacent parent or child");

    git_commit* raw_source_parent = nullptr;
    if (source_parent_oid != nullptr)
        Check(git_commit_parent(&raw_source_parent, source.get(), 0), "load line move parent");
    std::unique_ptr<git_commit, decltype(&git_commit_free)> source_parent(raw_source_parent, git_commit_free);
    git_tree* raw_before_tree = nullptr;
    git_tree* raw_after_tree = nullptr;
    if (source_parent != nullptr)
        Check(git_commit_tree(&raw_before_tree, source_parent.get()), "load line move parent tree");
    Check(git_commit_tree(&raw_after_tree, source.get()), "load line move source tree");
    std::unique_ptr<git_tree, decltype(&git_tree_free)> before_tree(raw_before_tree, git_tree_free);
    std::unique_ptr<git_tree, decltype(&git_tree_free)> after_tree(raw_after_tree, git_tree_free);

    git_diff_options diff_options{};
    Check(git_diff_options_init(&diff_options, GIT_DIFF_OPTIONS_VERSION), "initialize line move diff");
    diff_options.context_lines = 0;
    git_diff* raw_diff = nullptr;
    Check(git_diff_tree_to_tree(
              &raw_diff, git.get(), before_tree.get(), after_tree.get(), &diff_options),
        "create line move diff");
    std::unique_ptr<git_diff, decltype(&git_diff_free)> diff(raw_diff, git_diff_free);
    std::size_t delta_index = git_diff_num_deltas(diff.get());
    const git_diff_delta* delta = nullptr;
    for (std::size_t index = 0; index < git_diff_num_deltas(diff.get()); ++index)
    {
        const git_diff_delta* candidate = git_diff_get_delta(diff.get(), index);
        const std::string_view old_path = candidate->old_file.path == nullptr ? "" : candidate->old_file.path;
        const std::string_view new_path = candidate->new_file.path == nullptr ? old_path : candidate->new_file.path;
        if (old_path == command.path || new_path == command.path)
        {
            delta = candidate;
            delta_index = index;
            break;
        }
    }
    if (delta == nullptr || delta->status == GIT_DELTA_RENAMED
        || (delta->old_file.path != nullptr && delta->new_file.path != nullptr
            && std::string_view(delta->old_file.path) != delta->new_file.path))
        throw std::runtime_error("line moves require an unrenamed text file");

    bool binary = false;
    const std::string before = BlobText(git.get(), before_tree.get(), command.path.c_str(), binary);
    const std::string after = BlobText(git.get(), after_tree.get(), command.path.c_str(), binary);
    if (binary)
        throw std::runtime_error("binary files cannot be moved by line");

    git_patch* raw_patch = nullptr;
    Check(git_patch_from_diff(&raw_patch, diff.get(), delta_index), "load line move patch");
    std::unique_ptr<git_patch, decltype(&git_patch_free)> patch(raw_patch, git_patch_free);
    std::vector<DiffLine> changed_lines;
    for (std::size_t hunk = 0; hunk < git_patch_num_hunks(patch.get()); ++hunk)
    {
        const git_diff_hunk* raw_hunk = nullptr;
        std::size_t count = 0;
        Check(git_patch_get_hunk(&raw_hunk, &count, patch.get(), hunk), "load line move hunk");
        for (std::size_t line = 0; line < count; ++line)
        {
            const git_diff_line* raw_line = nullptr;
            Check(git_patch_get_line_in_hunk(&raw_line, patch.get(), hunk, line), "load line move line");
            if (raw_line->origin == GIT_DIFF_LINE_ADDITION || raw_line->origin == GIT_DIFF_LINE_DELETION)
                changed_lines.push_back(DiffLineFromRaw(*raw_line, static_cast<int>(hunk)));
        }
    }
    if (changed_lines.empty()
        || std::ranges::any_of(command.lines, [&](const DiffLine& requested) {
               return std::ranges::none_of(changed_lines,
                   [&](const DiffLine& actual) { return SameChangedLine(requested, actual); });
           }))
        throw std::runtime_error("selected lines no longer match the source change");

    const auto selected = [&](const git_diff_line& line) {
        const DiffLine actual = DiffLineFromRaw(line, 0);
        return std::ranges::any_of(
            command.lines, [&](const DiffLine& requested) { return SameChangedLine(requested, actual); });
    };
    const std::vector<std::string_view> before_lines = TextLines(before);
    std::size_t before_index = 0;
    std::size_t applied = 0;
    std::string partial;
    for (std::size_t hunk = 0; hunk < git_patch_num_hunks(patch.get()); ++hunk)
    {
        const git_diff_hunk* raw_hunk = nullptr;
        std::size_t count = 0;
        Check(git_patch_get_hunk(&raw_hunk, &count, patch.get(), hunk), "load line move hunk");
        const std::size_t hunk_start = raw_hunk->old_start > 0
            ? static_cast<std::size_t>(raw_hunk->old_start - 1)
            : 0;
        while (before_index < hunk_start && before_index < before_lines.size())
            partial.append(before_lines[before_index++]);
        for (std::size_t line = 0; line < count; ++line)
        {
            const git_diff_line* raw_line = nullptr;
            Check(git_patch_get_line_in_hunk(&raw_line, patch.get(), hunk, line), "load line move line");
            if (raw_line->origin == GIT_DIFF_LINE_CONTEXT)
            {
                partial.append(raw_line->content, raw_line->content_len);
                ++before_index;
                continue;
            }
            if (raw_line->origin != GIT_DIFF_LINE_ADDITION && raw_line->origin != GIT_DIFF_LINE_DELETION)
                continue;
            const bool apply = move_to_parent ? selected(*raw_line) : !selected(*raw_line);
            applied += apply ? 1 : 0;
            if (raw_line->origin == GIT_DIFF_LINE_DELETION)
            {
                if (!apply)
                    partial.append(raw_line->content, raw_line->content_len);
                ++before_index;
            }
            else if (apply)
                partial.append(raw_line->content, raw_line->content_len);
        }
    }
    while (before_index < before_lines.size())
        partial.append(before_lines[before_index++]);

    const bool old_exists = delta->old_file.mode != 0;
    const bool new_exists = delta->new_file.mode != 0;
    const bool partial_exists = applied == 0 ? old_exists
        : applied == changed_lines.size()       ? new_exists
                                                : true;
    const auto partial_mode = static_cast<git_filemode_t>(applied == changed_lines.size() ? delta->new_file.mode
        : old_exists ? delta->old_file.mode
                     : delta->new_file.mode);

    git_commit* target_commit = move_to_parent ? destination.get() : source.get();
    git_tree* raw_target_tree = nullptr;
    Check(git_commit_tree(&raw_target_tree, target_commit), "load line move target tree");
    std::unique_ptr<git_tree, decltype(&git_tree_free)> target_tree(raw_target_tree, git_tree_free);
    git_index* raw_index = nullptr;
    git_index_options index_options = GIT_INDEX_OPTIONS_INIT;
    index_options.oid_type = git_repository_oid_type(git.get());
    Check(git_index_new(&raw_index, &index_options), "create line move index");
    std::unique_ptr<git_index, decltype(&git_index_free)> index(raw_index, git_index_free);
    Check(git_index_read_tree(index.get(), target_tree.get()), "prepare line move tree");
    if (partial_exists)
    {
        git_oid blob{};
        Check(git_blob_create_from_buffer(&blob, git.get(), partial.data(), partial.size()),
            "write line move contents");
        git_index_entry entry{};
        entry.mode = partial_mode;
        entry.id = blob;
        entry.path = command.path.c_str();
        Check(git_index_add(index.get(), &entry), "update line move path");
    }
    else
    {
        const int removed = git_index_remove_bypath(index.get(), command.path.c_str());
        if (removed != GIT_ENOTFOUND)
            Check(removed, "remove line move path");
    }
    git_oid tree_oid{};
    Check(git_index_write_tree_to(&tree_oid, index.get(), git.get()), "write line move tree");
    git_tree* raw_partial_tree = nullptr;
    Check(git_tree_lookup(&raw_partial_tree, git.get(), &tree_oid), "load line move tree");
    std::unique_ptr<git_tree, decltype(&git_tree_free)> partial_tree(raw_partial_tree, git_tree_free);
    git_oid synthetic_oid{};
    // gg restore accepts revisions, so use an unreachable carrier commit that normal Git GC can prune.
    Check(git_commit_create(&synthetic_oid, git.get(), nullptr, git_commit_author(target_commit),
              git_commit_committer(target_commit), nullptr, "ggui partial line move", partial_tree.get(), 0,
              nullptr),
        "create line move source");

    const std::string synthetic = OidString(synthetic_oid);
    const std::string into = move_to_parent ? command.destination : command.source;
    const std::vector<std::string> paths{command.path};
    const StringArray path(paths);
    gg_restore_options options = GG_RESTORE_OPTIONS_INIT;
    options.filesets = path.Get();
    options.from = synthetic.c_str();
    options.into = into.c_str();
    options.restore_descendants = move_to_child ? 1 : 0;
    Mutate(revert ? "revert diff lines" : "move diff lines", [&](auto* out, auto* operation) {
        return gg_repository_restore(out, gg, &options, operation);
    });
}

} // namespace Ggui

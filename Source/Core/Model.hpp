// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <gg/gg.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Ggui
{

struct Revision
{
    std::string oid;
    std::vector<std::string> parents;
    std::vector<std::string> aliases;
    std::string description;
    std::string author;
    std::int64_t timestamp = 0;
    bool working_copy = false;
    bool conflicted = false;
    bool pushed = false;
    bool empty = false;
    std::string author_email{};
};

struct NamedRef
{
    std::string name;
    std::string remote;
    std::string target;
    gg_named_ref_kind kind = GG_NAMED_REF_LOCAL_BOOKMARK;
    bool tracked = false;
    bool conflicted = false;
};

struct StatusEntry
{
    std::string old_path;
    std::string path;
    git_delta_t status = GIT_DELTA_UNMODIFIED;
    bool conflicted = false;
};

struct Operation
{
    std::string oid;
    std::string description;
    std::int64_t timestamp = 0;
};

struct Workspace
{
    std::string name;
    std::string root;
    std::string working_copy;
    bool stale = false;
};

struct Remote
{
    std::string name;
    std::string fetch_url;
    std::string push_url;
};

struct Conflict
{
    std::string path;
    std::size_t removes = 0;
    std::size_t adds = 0;
};

struct RepoSnapshot
{
    std::uint64_t generation = 0;
    std::string root;
    std::string working_copy;
    std::vector<Revision> revisions;
    std::vector<NamedRef> refs;
    std::vector<StatusEntry> status;
    std::vector<Operation> operations;
    std::vector<Workspace> workspaces;
    std::vector<Remote> remotes;
    std::vector<Conflict> conflicts;
    bool can_undo = false;
    bool can_redo = false;
};

enum class DiffWhitespaceMode
{
    Normal,
    IgnoreWhitespace,
    IgnoreAllWhitespace,
};

enum class DiffLineKind
{
    Context,
    Addition,
    Deletion,
};

struct DiffLine
{
    DiffLineKind kind = DiffLineKind::Context;
    int old_line = -1;
    int new_line = -1;
    int hunk = -1;
};

struct DiffOptions
{
    DiffWhitespaceMode whitespace_mode = DiffWhitespaceMode::Normal;
    int context_lines = 3;
};

struct DiffResult
{
    DiffResult() = default;
    DiffResult(std::uint64_t generation, std::string revision, std::string path, std::string before, std::string after,
        bool binary, std::vector<StatusEntry> files, std::string compare_to = {}, bool file_comparison = false)
        : generation(generation)
        , revision(std::move(revision))
        , compare_to(std::move(compare_to))
        , file_comparison(file_comparison)
        , path(std::move(path))
        , before(std::move(before))
        , after(std::move(after))
        , binary(binary)
        , files(std::move(files))
    {
        for (const StatusEntry& file : this->files)
            if (file.path == this->path || file.old_path == this->path)
            {
                selected_status = file.status;
                break;
            }
    }

    std::uint64_t generation = 0;
    std::string revision;
    std::string compare_to;
    bool file_comparison = false;
    std::string path;
    std::string before;
    std::string after;
    bool binary = false;
    git_delta_t selected_status = GIT_DELTA_UNMODIFIED;
    std::vector<StatusEntry> files;
    std::vector<DiffLine> lines{};
    std::string patch;
    std::string old_oid;
    std::string new_oid;
    unsigned int old_mode = 0;
    unsigned int new_mode = 0;
    DiffOptions options;
};

} // namespace Ggui

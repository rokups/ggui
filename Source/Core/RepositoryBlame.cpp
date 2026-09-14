// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <git2/blame.h>

#include <memory>
#include <stdexcept>
#include <string>

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

std::string SignatureName(const git_signature* signature)
{
    return signature == nullptr || signature->name == nullptr ? "" : signature->name;
}

std::string SignatureEmail(const git_signature* signature)
{
    return signature == nullptr || signature->email == nullptr ? "" : signature->email;
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

    git_oid newest{};
    Check(gg_repository_resolve(&newest, gg_repository, command.revision.c_str()), "resolve blame revision");
    if (!current())
        return;

    git_blame_options options = GIT_BLAME_OPTIONS_INIT;
    options.newest_commit = newest;
    git_blame* raw_blame = nullptr;
    Check(git_blame_file(&raw_blame, repository, command.path.c_str(), &options), "load blame");
    BlamePtr blame(raw_blame);

    BlameResult result;
    result.generation = snapshot_generation;
    result.revision = command.revision;
    result.path = command.path;
    result.lines.reserve(git_blame_linecount(blame.get()));
    for (std::size_t index = 0; index < git_blame_linecount(blame.get()); ++index)
    {
        const std::size_t line_number = index + 1;
        const git_blame_hunk* hunk = git_blame_hunk_byline(blame.get(), line_number);
        const git_blame_line* line = git_blame_line_byindex(blame.get(), line_number);
        if (hunk == nullptr || line == nullptr)
            throw std::runtime_error("load blame line");

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
        value.contents.assign(line->ptr == nullptr ? "" : line->ptr, line->len);
        value.boundary = hunk->boundary != 0;
        result.lines.push_back(std::move(value));
    }

    if (current())
        Post(BlameReady{std::move(result)});
}

} // namespace Ggui

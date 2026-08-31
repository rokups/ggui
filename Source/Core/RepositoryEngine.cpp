// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Ggui
{
using namespace RepositoryInternal;

RepositoryEngine::RepositoryEngine() : _impl(std::make_unique<Impl>()) {}
RepositoryEngine::~RepositoryEngine() = default;

void RepositoryEngine::Enqueue(Command command)
{
#ifdef GGUI_TESTING
    if (_impl->test_commands_suppressed)
        return;
#endif
    if (auto* rebuild = std::get_if<RebuildHistory>(&command))
    {
        _impl->RequestHistory(std::move(rebuild->query));
        return;
    }
    if (auto* expand = std::get_if<ExpandHistoryRegion>(&command))
    {
        HistoryQuery query;
        {
            std::lock_guard lock(_impl->history_mutex);
            query = _impl->active_history_query;
        }
        _impl->RequestHistory(std::move(query), std::move(expand->id));
        return;
    }
    if (std::holds_alternative<LoadDiff>(command) || std::holds_alternative<LoadFileContent>(command))
    {
        const std::uint64_t request = ++_impl->inspector_request;
        {
            std::lock_guard lock(_impl->inspector_mutex);
            // Reads are latest-wins. Running reads are allowed to finish, but
            // their generation check prevents them from publishing stale UI.
            _impl->inspector_requests.clear();
            RepositoryEngine::Impl::InspectorRequest queued;
            queued.session = _impl->session.load();
            queued.request = request;
            if (auto* diff = std::get_if<LoadDiff>(&command))
                queued.command = std::move(*diff);
            else
                queued.command = std::move(std::get<LoadFileContent>(command));
            _impl->inspector_requests.push_back(std::move(queued));
        }
        _impl->inspector_cv.notify_one();
        return;
    }
    {
        std::lock_guard lock(_impl->queue_mutex);
        if (std::holds_alternative<LoadDiff>(command))
            std::erase_if(_impl->commands,
                [](const Command& queued) { return std::holds_alternative<LoadDiff>(queued); });
        else if (std::holds_alternative<CloseRepository>(command))
            std::erase_if(_impl->commands, [](const Command& queued) {
                return std::holds_alternative<LoadDiff>(queued) || std::holds_alternative<Refresh>(queued)
                    || std::holds_alternative<RebuildHistory>(queued)
                    || std::holds_alternative<ExpandHistoryRegion>(queued);
            });
        _impl->commands.push_back(std::move(command));
    }
    _impl->queue_cv.notify_one();
}

std::vector<Event> RepositoryEngine::PollEvents()
{
    std::lock_guard lock(_impl->event_mutex);
    std::vector<Event> result;
    result.swap(_impl->events);
    return result;
}

BookmarkRelation ClassifyBookmarkRelation(
    const RepoSnapshot& snapshot, std::string_view local, std::string_view remote)
{
    git_repository* raw = nullptr;
    if (git_repository_open_ext(&raw, snapshot.root.c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) != GIT_OK)
        return BookmarkRelation::Unavailable;
    RepositoryInternal::GitRepositoryPtr repository(raw);
    git_oid local_oid{};
    git_oid remote_oid{};
    const git_oid_t type = git_repository_oid_type(repository.get());
    if (git_oid_fromstr(&local_oid, std::string(local).c_str(), type) != GIT_OK
        || git_oid_fromstr(&remote_oid, std::string(remote).c_str(), type) != GIT_OK)
        return BookmarkRelation::Unavailable;
    if (git_oid_equal(&local_oid, &remote_oid) != 0)
        return BookmarkRelation::Synchronized;
    const int local_is_ancestor = git_graph_descendant_of(repository.get(), &remote_oid, &local_oid);
    const int remote_is_ancestor = git_graph_descendant_of(repository.get(), &local_oid, &remote_oid);
    if (local_is_ancestor < 0 || remote_is_ancestor < 0) return BookmarkRelation::Unavailable;
    if (local_is_ancestor != 0)
        return BookmarkRelation::RemoteAhead;
    if (remote_is_ancestor != 0)
        return BookmarkRelation::LocalAhead;
    return BookmarkRelation::Diverged;
}

void RepositoryEngine::Cancel()
{
    _impl->cancel_requested = true;
    _impl->credential_cv.notify_all();
}

void RepositoryEngine::SubmitCredential(CredentialResponse response)
{
    {
        std::lock_guard lock(_impl->credential_mutex);
        _impl->credential_response = std::move(response);
        _impl->credential_cancelled = false;
    }
    _impl->credential_cv.notify_all();
}

void RepositoryEngine::CancelCredential()
{
    {
        std::lock_guard lock(_impl->credential_mutex);
        _impl->credential_cancelled = true;
    }
    _impl->credential_cv.notify_all();
}

#ifdef GGUI_TESTING
void RepositoryEngine::ResetCredentialStateForTest(bool ssh_agent_attempted, int credential_attempts)
{
    std::lock_guard lock(_impl->credential_mutex);
    _impl->credential_response.reset();
    _impl->credential_cancelled = false;
    _impl->ssh_agent_attempted = ssh_agent_attempted;
    _impl->credential_attempts = credential_attempts;
    _impl->cancel_requested = false;
}

int RepositoryEngine::AcquireCredentialForTest(
    git_credential** out, const char* url, const char* username, unsigned int allowed_types)
{
    return _impl->CredentialCallback(out, url, username, allowed_types, _impl.get());
}

int RepositoryEngine::TransferProgressForTest(const git_indexer_progress& progress)
{
    return _impl->TransferProgress(&progress, _impl.get());
}

void RepositoryEngine::DispatchMutationForTest(const Command& command)
{
    _impl->DispatchMutation(command);
}

void RepositoryEngine::SetCommandsSuppressedForTest(bool suppressed)
{
    _impl->test_commands_suppressed = suppressed;
    if (!suppressed)
        return;
    {
        std::lock_guard lock(_impl->queue_mutex);
        _impl->commands.clear();
    }
    {
        std::lock_guard lock(_impl->event_mutex);
        _impl->events.clear();
    }
}

std::vector<Conflict> RepositoryEngine::ConvertConflictsForTest()
{
    char path[] = "conflicted.txt";
    gg_conflict values[2]{};
    values[0].path = path;
    values[0].remove_count = 2;
    values[0].add_count = 3;
    values[1].path = nullptr;
    std::vector<Conflict> result;
    AppendConflicts(result, {values, 2});
    return result;
}

void RepositoryEngine::FinishCloneForTest(
    const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
    FinishClone(temporary, destination);
}
#endif

std::string ShortId(const std::string& value, std::size_t length)
{
    return value.substr(0, std::min(length, value.size()));
}

std::vector<std::size_t> UniquePrefixLengths(const std::vector<std::string>& values, std::size_t minimum)
{
    std::vector<std::size_t> order(values.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::ranges::sort(order, {}, [&](std::size_t index) { return values[index]; });
    std::vector<std::size_t> result(values.size());
    const auto common = [&](std::size_t left, std::size_t right) {
        const std::string& first = values[order[left]];
        const std::string& second = values[order[right]];
        return static_cast<std::size_t>(std::ranges::mismatch(first, second).in1 - first.begin());
    };
    for (std::size_t first = 0; first < order.size();)
    {
        std::size_t last = first + 1;
        while (last < order.size() && values[order[last]] == values[order[first]])
            ++last;
        std::size_t required = 1;
        if (first != 0)
            required = std::max(required, common(first - 1, first) + 1);
        if (last != order.size())
            required = std::max(required, common(last - 1, last) + 1);
        for (std::size_t position = first; position < last; ++position)
            result[order[position]] =
                std::min(values[order[position]].size(), std::max(minimum, required));
        first = last;
    }
    return result;
}

void MarkPushedRevisions(
    std::vector<Revision>& revisions, const std::vector<NamedRef>& refs, git_repository* repository)
{
    std::unordered_map<std::string, Revision*> by_oid;
    by_oid.reserve(revisions.size());
    for (Revision& revision : revisions)
    {
        revision.pushed = false;
        by_oid.emplace(revision.oid, &revision);
    }
    std::vector<std::string> pending;
    for (const NamedRef& ref : refs)
        if (ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK || ref.kind == GG_NAMED_REF_REMOTE_TAG)
            pending.push_back(ref.target);
    if (repository != nullptr)
    {
        git_revwalk* raw_walk = nullptr;
        Check(git_revwalk_new(&raw_walk, repository), "create pushed revision walk");
        std::unique_ptr<git_revwalk, decltype(&git_revwalk_free)> walk(raw_walk, git_revwalk_free);
        for (const std::string& target : pending)
        {
            git_oid oid{};
            if (git_oid_fromstr(&oid, target.c_str(), git_repository_oid_type(repository)) == GIT_OK)
                Check(git_revwalk_push(walk.get(), &oid), "walk pushed revisions");
        }
        git_oid oid{};
        int result = GIT_OK;
        while ((result = git_revwalk_next(&oid, walk.get())) == GIT_OK)
        {
            const auto found = by_oid.find(OidString(oid));
            if (found != by_oid.end())
                found->second->pushed = true;
        }
        if (result != GIT_ITEROVER)
            Check(result, "walk pushed revisions");
        return;
    }
    while (!pending.empty())
    {
        const auto found = by_oid.find(pending.back());
        pending.pop_back();
        if (found == by_oid.end() || found->second->pushed)
            continue;
        found->second->pushed = true;
        pending.insert(pending.end(), found->second->parents.begin(), found->second->parents.end());
    }
}

std::string FirstLine(const std::string& value)
{
    return value.substr(0, value.find('\n'));
}

} // namespace Ggui

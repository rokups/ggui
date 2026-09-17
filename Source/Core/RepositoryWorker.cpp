// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "RepositoryEngineInternal.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef DeleteFile
#undef DeleteFile
#endif
#endif

namespace Ggui
{
using namespace RepositoryInternal;

namespace
{
struct GgRepositoryDeleter
{
    void operator()(gg_repository* value) const { gg_repository_free(value); }
};
using GgRepositoryPtr = std::unique_ptr<gg_repository, GgRepositoryDeleter>;

struct RepositoryReadContext
{
    void Ensure(std::string_view requested_path, std::uint64_t requested_session,
        std::uint64_t requested_generation, bool attach_gg)
    {
        if (repository == nullptr || path != requested_path || session != requested_session)
        {
            gg.reset();
            repository.reset();
            std::string opened_path(requested_path);
            git_repository* raw = nullptr;
            Check(git_repository_open_ext(&raw, opened_path.c_str(),
                GIT_REPOSITORY_OPEN_CROSS_FS, nullptr), "open reader repository");
            repository.reset(raw);
            path = std::move(opened_path);
            session = requested_session;
            repository_generation = 0;
        }
        if (attach_gg && (gg == nullptr || repository_generation != requested_generation))
        {
            gg.reset();
            gg_repository* raw = nullptr;
            Check(gg_repository_attach(&raw, repository.get()), "attach reader repository");
            gg.reset(raw);
            repository_generation = requested_generation;
        }
    }

    void Reset()
    {
        gg.reset();
        repository.reset();
        path.clear();
        session = 0;
        repository_generation = 0;
    }

    std::string path;
    std::uint64_t session = 0;
    std::uint64_t repository_generation = 0;
    GitRepositoryPtr repository;
    GgRepositoryPtr gg;
};

class BackgroundScheduling
{
public:
    BackgroundScheduling()
    {
#ifdef _WIN32
        _enabled = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0;
#endif
    }

    ~BackgroundScheduling()
    {
#ifdef _WIN32
        if (_enabled) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
#endif
    }

    BackgroundScheduling(const BackgroundScheduling&) = delete;
    BackgroundScheduling& operator=(const BackgroundScheduling&) = delete;

private:
#ifdef _WIN32
    bool _enabled = false;
#endif
};

Revision ConvertRevision(const gg_revision& source, std::string_view working_copy)
{
    Revision value;
    value.oid = OidString(source.oid);
    for (size_t parent = 0; parent < source.parents.count; ++parent)
        value.parents.push_back(OidString(source.parents.ids[parent]));
    for (size_t alias = 0; alias < source.aliases.count; ++alias)
        value.aliases.push_back(OidString(source.aliases.ids[alias]));
    value.description = source.description == nullptr ? "" : source.description;
    value.author = source.author == nullptr || source.author->name == nullptr ? "" : source.author->name;
    value.author_email = source.author == nullptr || source.author->email == nullptr ? "" : source.author->email;
    value.timestamp = source.committer == nullptr ? 0 : source.committer->when.time;
    value.working_copy = value.oid == working_copy;
    value.conflicted = source.has_conflicts != 0;
    value.empty = source.empty != 0;
    return value;
}

bool ContainsInsensitiveText(std::string_view text, std::string_view query)
{
    return std::ranges::search(text, query, [](char left, char right) {
        return std::tolower(static_cast<unsigned char>(left))
            == std::tolower(static_cast<unsigned char>(right));
    }).begin() != text.end();
}

std::vector<std::size_t> HistoryTopologicalOrder(
    const std::vector<HistoryItem>& items, const std::vector<std::string>& preferred_heads)
{
    std::unordered_map<std::string_view, std::size_t> indexes;
    for (std::size_t index = 0; index < items.size(); ++index) indexes.emplace(items[index].id, index);
    std::vector<std::size_t> children(items.size());
    for (std::size_t index = 0; index < items.size(); ++index)
        for (const std::string& parent : items[index].parents)
            if (const auto found = indexes.find(parent); found != indexes.end())
                ++children[found->second];

    std::vector<std::size_t> priorities(items.size(), std::numeric_limits<std::size_t>::max());
    for (std::size_t priority = 0; priority < preferred_heads.size(); ++priority)
        if (const auto found = indexes.find(preferred_heads[priority]); found != indexes.end())
            priorities[found->second] = priority;
    const auto timestamp = [&](std::size_t index) {
        return items[index].kind == HistoryItemKind::Commit
            ? items[index].revision.timestamp : std::numeric_limits<std::int64_t>::min();
    };
    const auto newer = [&](std::size_t left, std::size_t right) {
        const bool left_region = items[left].kind == HistoryItemKind::CollapsedRegion;
        const bool right_region = items[right].kind == HistoryItemKind::CollapsedRegion;
        if (left_region != right_region) return left_region;
        if (timestamp(left) != timestamp(right)) return timestamp(left) > timestamp(right);
        if (priorities[left] != priorities[right]) return priorities[left] < priorities[right];
        return items[left].id < items[right].id;
    };

    std::vector<std::size_t> ready;
    for (std::size_t index = 0; index < items.size(); ++index)
        if (children[index] == 0) ready.push_back(index);
    std::vector<std::size_t> result;
    result.reserve(items.size());
    // Kahn's algorithm enforces children-before-parents topology. A collapsed
    // parent is emitted as soon as all of its visible children have appeared,
    // so branch terminators stay beside their branch instead of collecting at
    // the bottom. Date chooses among the remaining unconstrained commits.
    while (!ready.empty())
    {
        const auto best = std::ranges::min_element(ready, newer);
        const std::size_t index = *best;
        ready.erase(best);
        result.push_back(index);
        for (const std::string& parent : items[index].parents)
            if (const auto found = indexes.find(parent);
                found != indexes.end() && --children[found->second] == 0)
        {
            ready.push_back(found->second);
        }
    }
    if (result.size() != items.size()) throw std::runtime_error("history graph is not acyclic");
    return result;
}
} // namespace

RepositoryEngine::Impl::BackgroundActivityGuard::BackgroundActivityGuard(Impl& owner, std::string name)
    : owner(owner), id(++owner.background_activity_id)
{
    owner.Post(BackgroundActivityStarted{id, std::move(name)});
}

RepositoryEngine::Impl::BackgroundActivityGuard::~BackgroundActivityGuard()
{
    owner.Post(BackgroundActivityFinished{id});
}

void RepositoryEngine::Impl::Execute(const Command& command)
{
    const std::string name = CommandName(command);
    const auto* refresh = std::get_if<Refresh>(&command);
    const bool quiet = (refresh != nullptr && !refresh->foreground) || std::holds_alternative<RebuildHistory>(command)
        || std::holds_alternative<ExpandHistoryRegion>(command)
        || std::holds_alternative<LoadDiff>(command)
        || std::holds_alternative<LoadFileContent>(command)
        || std::holds_alternative<LoadBlame>(command);
    if (!quiet)
        Post(OperationStarted{name});
    std::optional<BackgroundActivityGuard> background_activity;
    std::optional<BackgroundScheduling> background_scheduling;
    if (refresh != nullptr && !refresh->foreground)
    {
        background_activity.emplace(*this,
            refresh->snapshot_working_copy ? "Scanning working copy" : "Refreshing repository metadata");
        background_scheduling.emplace();
    }
    if (!std::holds_alternative<LoadDiff>(command) && !std::holds_alternative<LoadFileContent>(command)
        && !std::holds_alternative<LoadBlame>(command)
        && !std::holds_alternative<RebuildHistory>(command)
        && !std::holds_alternative<ExpandHistoryRegion>(command) && !std::holds_alternative<Refresh>(command))
    {
        ++inspector_request;
        std::lock_guard lock(inspector_mutex);
        inspector_requests.clear();
    }
    cancel_requested = false;
    if (std::holds_alternative<OpenRepository>(command) || std::holds_alternative<InitRepository>(command)
        || std::holds_alternative<CloneRepository>(command))
        Close();
    try
    {
        if (const auto* value = std::get_if<OpenRepository>(&command))
            OpenPath(value->path);
        else if (std::holds_alternative<CloseRepository>(command))
        {
            Close();
            {
                std::lock_guard lock(queue_mutex);
                std::erase_if(commands, [](const Command& queued) {
                    return std::holds_alternative<LoadDiff>(queued) || std::holds_alternative<LoadFileContent>(queued)
                        || std::holds_alternative<LoadBlame>(queued)
                        || std::holds_alternative<Refresh>(queued)
                        || std::holds_alternative<RebuildHistory>(queued)
                        || std::holds_alternative<ExpandHistoryRegion>(queued);
                });
            }
            {
                std::lock_guard lock(event_mutex);
                std::erase_if(events, [](const Event& queued) {
                    return std::holds_alternative<SnapshotReady>(queued) || std::holds_alternative<DiffReady>(queued)
                        || std::holds_alternative<FileContentReady>(queued)
                        || std::holds_alternative<BlameReady>(queued);
                });
            }
        }
        else if (const auto* value = std::get_if<InitRepository>(&command))
            InitPath(value->path);
        else if (const auto* value = std::get_if<CloneRepository>(&command))
            ClonePath(*value);
        else if (const auto* value = std::get_if<Refresh>(&command))
        {
            const bool initial_reconciliation = value->snapshot_working_copy && !worktree_ready;
            if (value->snapshot_working_copy)
                Sync(false, value->paths);
            // A watcher refresh already tells us which files changed. Reuse
            // the cached status for metadata-only refreshes and update only
            // those paths for incremental worktree refreshes. An empty path
            // list deliberately retains the full scan for F5 and directory
            // events that the watcher could not narrow down.
            PublishSnapshot(value->snapshot_working_copy, false, value->paths);
            if (initial_reconciliation)
            {
                // Drain delayed notifications caused by our own initial index,
                // object, and gg-ref writes. The watcher was intentionally
                // armed first, so real worktree events remain represented by
                // the just-completed reconciliation.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                const RepositoryWatcher::Changes replay = watcher.ConsumeChanges();
                if (replay.worktree && Sync(false, replay.full_scan ? std::vector<std::string>{} : replay.paths))
                {
                    PublishSnapshot(true, false, replay.full_scan ? std::vector<std::string>{}
                                                                    : replay.paths);
                }
            }
        }
        else if (std::holds_alternative<RebuildHistory>(command)
            || std::holds_alternative<ExpandHistoryRegion>(command))
            return; // Interactive history reads are dispatched by Enqueue().
        else if (const auto* value = std::get_if<Fetch>(&command))
            FetchRemote(*value);
        else if (const auto* value = std::get_if<Push>(&command))
            PushBookmark(*value);
        else if (std::holds_alternative<LoadDiff>(command)
            || std::holds_alternative<LoadFileContent>(command)
            || std::holds_alternative<LoadBlame>(command))
            return; // Interactive reads are dispatched by Enqueue().
        else if (const auto* value = std::get_if<ApplyPatch>(&command))
            ApplyPatchText(*value);
        else if (const auto* value = std::get_if<ResolveConflict>(&command))
            ResolveConflictFile(*value);
        else if (const auto* value = std::get_if<RevertFile>(&command))
            RevertFileChange(*value);
        else if (const auto* value = std::get_if<DeleteFile>(&command))
            DeleteWorkingFile(*value);
        else
            DispatchMutation(command);
        if (!quiet)
            Post(OperationFinished{name});
    }
    catch (const std::exception& error)
    {
        spdlog::error("{} failed: {}", name, error.what());
        Post(ErrorEvent{name, error.what()});
    }
}

void RepositoryEngine::Impl::RequestHistory(HistoryQuery query, std::string expand, bool toggle)
{
    std::string path;
    {
        std::lock_guard lock(snapshot_mutex);
        if (latest_snapshot == nullptr || query.repository_generation != latest_snapshot->repository_generation) return;
        path = latest_snapshot->root;
    }
    const std::uint64_t request = ++history_request_version;
    {
        std::lock_guard lock(history_mutex);
        if (repository_path != path || repository_path_session != session.load()) return;
        if (expand.empty())
        {
            if (query.repository_generation != active_history_query.repository_generation)
                expanded_history_regions.clear();
            active_history_query = query;
        }
        else if (const auto found = std::ranges::find(expanded_history_regions, expand);
            found == expanded_history_regions.end())
            expanded_history_regions.push_back(expand);
        else if (toggle)
            expanded_history_regions.erase(found);
        history_request = HistoryRequest{active_history_query, std::move(expand), std::move(path),
            repository_path_session, request};
    }
    history_cv.notify_one();
}

void RepositoryEngine::Impl::RunHistory()
{
    std::string cached_search_path;
    std::string cached_search_text;
    std::uint64_t cached_search_generation = 0;
    std::vector<git_oid> cached_description_matches;
    std::string cached_revision_path;
    std::uint64_t cached_revision_generation = 0;
    std::unordered_map<std::string, Revision> cached_revisions;
    std::unique_ptr<NamedRefs> cached_named_refs;
    std::unique_ptr<References> cached_references;
    std::unique_ptr<Workspaces> cached_workspaces;
    std::vector<git_oid> cached_collapsed_materialized;
    RepositoryReadContext context;
    while (true)
    {
        HistoryRequest request;
        std::vector<std::string> expansions;
        {
            std::unique_lock lock(history_mutex);
            history_cv.wait(lock, [this, &context] {
                return stopping || history_request.has_value()
                    || (context.repository != nullptr && context.session != session.load());
            });
            if (stopping) break;
            if (!history_request.has_value())
            {
                context.Reset();
                cached_search_path.clear();
                cached_search_text.clear();
                cached_search_generation = 0;
                cached_description_matches.clear();
                cached_revision_path.clear();
                cached_revision_generation = 0;
                cached_revisions.clear();
                cached_named_refs.reset();
                cached_references.reset();
                cached_workspaces.reset();
                cached_collapsed_materialized.clear();
                continue;
            }
            request = std::move(*history_request);
            history_request.reset();
            expansions = expanded_history_regions;
        }
        const auto stale = [&] {
            return stopping.load() || request.session != session.load()
                || request.request != history_request_version.load();
        };
        try
        {
            context.Ensure(request.path, request.session, request.query.repository_generation, true);
            GitRepositoryPtr& repository = context.repository;
            GgRepositoryPtr& history_gg = context.gg;
            if (cached_revision_path != request.path
                || cached_revision_generation != request.query.repository_generation)
            {
                cached_revision_path = request.path;
                cached_revision_generation = request.query.repository_generation;
                cached_revisions.clear();
                cached_named_refs.reset();
                cached_references.reset();
                cached_workspaces.reset();
                cached_collapsed_materialized.clear();
            }

            if (cached_named_refs == nullptr)
            {
                cached_named_refs = std::make_unique<NamedRefs>();
                cached_references = std::make_unique<References>();
                cached_workspaces = std::make_unique<Workspaces>();
                Check(gg_repository_named_refs(&cached_named_refs->value, history_gg.get()), "load history refs");
                Check(gg_repository_references(&cached_references->value, history_gg.get()),
                    "load history references");
                Check(gg_repository_workspaces(&cached_workspaces->value, history_gg.get()),
                    "load history workspaces");
            }
            const NamedRefs& named = *cached_named_refs;
            const References& references = *cached_references;
            const Workspaces& workspaces = *cached_workspaces;
            const auto selected = [](const std::vector<std::string>& values, std::string_view value) {
                return std::ranges::find(values, value) != values.end();
            };
            std::vector<git_oid> heads;
            std::vector<git_oid> selected_bookmarks;
            std::vector<git_oid> unselected_bookmarks;
            std::vector<git_oid> remote_tips;
            std::vector<git_oid> conservatively_locked_heads;
            std::vector<git_oid> primary_heads;
            const auto add = [](std::vector<git_oid>& values, const git_oid& oid) {
                if (std::ranges::none_of(values, [&](const git_oid& value) { return git_oid_equal(&value, &oid) != 0; }))
                    values.push_back(oid);
            };
            struct BoundedReachability
            {
                bool matched = false;
                bool complete = true;
            };
            std::size_t implicit_lookup_budget = 256;
            const auto descends_from_any = [&](const git_oid& candidate,
                                               const std::vector<git_oid>& ancestors) {
                BoundedReachability result;
                if (ancestors.empty()) return result;
                std::deque<git_oid> pending{candidate};
                std::unordered_set<std::string> visited;
                while (!pending.empty() && implicit_lookup_budget > 0 && !stale())
                {
                    const git_oid oid = pending.front();
                    pending.pop_front();
                    if (!visited.insert(OidString(oid)).second) continue;
                    --implicit_lookup_budget;
                    if (std::ranges::any_of(ancestors,
                            [&](const git_oid& ancestor) { return git_oid_equal(&oid, &ancestor) != 0; }))
                    {
                        result.matched = true;
                        return result;
                    }
                    git_commit* raw_commit = nullptr;
                    const int lookup = git_commit_lookup(&raw_commit, repository.get(), &oid);
                    if (lookup == GIT_ENOTFOUND) continue;
                    Check(lookup, "inspect bounded bookmark descendants");
                    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
                    for (unsigned int index = 0; index < git_commit_parentcount(commit.get()); ++index)
                        pending.push_back(*git_commit_parent_id(commit.get(), index));
                }
                result.complete = pending.empty();
                return result;
            };
            git_oid working{};
            std::string working_copy;
            if (gg_repository_working_copy(&working, history_gg.get()) == GIT_OK)
            {
                add(heads, working);
                add(primary_heads, working);
                working_copy = OidString(working);
            }
            git_reference* raw_head = nullptr;
            if (git_repository_head(&raw_head, repository.get()) == GIT_OK)
            {
                std::unique_ptr<git_reference, decltype(&git_reference_free)> head(raw_head, git_reference_free);
                if (const git_oid* target = git_reference_target(head.get()))
                {
                    add(heads, *target);
                    add(primary_heads, *target);
                }
            }
            for (std::size_t index = 0; index < named.value.count; ++index)
            {
                const gg_named_ref& ref = named.value.items[index];
                const std::string_view name = ref.name == nullptr ? "" : ref.name;
                const std::string_view remote = ref.remote == nullptr ? "" : ref.remote;
                const bool chosen_remote = request.query.remotes.empty() || selected(request.query.remotes, remote);
                if (ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK) add(remote_tips, ref.target);
                if (ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK)
                {
                    const bool chosen = request.query.bookmarks.empty() || selected(request.query.bookmarks, name);
                    add(chosen ? selected_bookmarks : unselected_bookmarks, ref.target);
                    if (chosen) add(heads, ref.target);
                }
                else if (ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && chosen_remote
                    && (request.query.bookmarks.empty() || selected(request.query.bookmarks, name)))
                {
                    add(selected_bookmarks, ref.target);
                    add(heads, ref.target);
                }
                else if ((ref.kind == GG_NAMED_REF_LOCAL_TAG
                            || (ref.kind == GG_NAMED_REF_REMOTE_TAG && chosen_remote))
                    && selected(request.query.tags, name))
                {
                    add(heads, ref.target);
                    add(conservatively_locked_heads, ref.target);
                }
            }
            // Preserve the selected-bookmark filtering for other workspaces,
            // including native worktrees which do not have a gg reference.
            for (std::size_t index = 0; index < workspaces.value.count; ++index)
            {
                const git_oid& candidate = workspaces.value.items[index].working_copy;
                const BoundedReachability selected_result = descends_from_any(candidate, selected_bookmarks);
                const BoundedReachability unselected_result = descends_from_any(candidate, unselected_bookmarks);
                const bool claimed = unselected_result.matched
                    || (!unselected_result.complete && !unselected_bookmarks.empty());
                if (selected_result.matched && !claimed) add(heads, candidate);
            }
            // Alias, workspace, and explicit visible-head refs retain local
            // unnamed changes. Resolve their actual DAG heads so switching the
            // working copy cannot make a sibling head disappear. Tags are not
            // visible heads and remain governed by their panel selection.
            std::unordered_set<std::string> unnamed_targets;
            for (std::size_t index = 0; index < references.value.count; ++index)
            {
                const gg_reference& ref = references.value.items[index];
                const std::string_view name = ref.name == nullptr ? "" : ref.name;
                if (name.starts_with("refs/gg/aliases/")
                    || name.starts_with("refs/gg/visible-heads/")
                    || name.starts_with("refs/gg/workspaces/"))
                    unnamed_targets.insert(OidString(ref.target));
            }
            Oids local_heads;
            Check(gg_repository_resolve_set(&local_heads.value, history_gg.get(), "visible_heads()"),
                "resolve local history heads");
            for (std::size_t index = 0; index < local_heads.value.count; ++index)
            {
                const git_oid& candidate = local_heads.value.ids[index];
                if (!unnamed_targets.contains(OidString(candidate))) continue;
                const BoundedReachability unselected_result = descends_from_any(candidate, unselected_bookmarks);
                const bool claimed = unselected_result.matched
                    || (!unselected_result.complete && !unselected_bookmarks.empty());
                if (!claimed) add(heads, candidate);
            }
            if (stale()) continue;

            std::unordered_set<std::string> search_matches;
            if (!request.query.search.empty())
            {
                git_oid resolved{};
                if (gg_repository_resolve(
                        &resolved, history_gg.get(), request.query.search.c_str()) == GIT_OK)
                {
                    add(heads, resolved);
                    search_matches.insert(OidString(resolved));
                }
                git_object* object = nullptr;
                if (git_revparse_single(&object, repository.get(), request.query.search.c_str()) == GIT_OK)
                {
                    std::unique_ptr<git_object, decltype(&git_object_free)> owned(object, git_object_free);
                    git_object* commit_object = nullptr;
                    if (git_object_peel(&commit_object, owned.get(), GIT_OBJECT_COMMIT) == GIT_OK)
                    {
                        std::unique_ptr<git_object, decltype(&git_object_free)> commit(commit_object, git_object_free);
                        add(heads, *git_object_id(commit.get()));
                        search_matches.insert(OidString(*git_object_id(commit.get())));
                    }
                }
                for (std::size_t index = 0; index < named.value.count; ++index)
                {
                    const gg_named_ref& ref = named.value.items[index];
                    const std::string label = ref.remote == nullptr || *ref.remote == '\0'
                        ? (ref.name == nullptr ? "" : std::string(ref.name))
                        : std::string(ref.remote) + "/" + (ref.name == nullptr ? "" : std::string(ref.name));
                    if (search_matches.size() < 50 && ContainsInsensitiveText(label, request.query.search))
                    {
                        add(heads, ref.target);
                        search_matches.insert(OidString(ref.target));
                    }
                }
            }
            const bool direct_search_match = !search_matches.empty();
            const bool reusable_description_search = !request.query.search.empty()
                && cached_search_path == request.path
                && cached_search_generation == request.query.repository_generation
                && cached_search_text == request.query.search;
            if (reusable_description_search)
                for (const git_oid& oid : cached_description_matches)
                {
                    search_matches.insert(OidString(oid));
                    add(heads, oid);
                }

            const auto hydrate = [&](const std::vector<git_oid>& oids) {
                std::vector<git_oid> missing;
                missing.reserve(oids.size());
                for (const git_oid& oid : oids)
                    if (!cached_revisions.contains(OidString(oid)))
                        missing.push_back(oid);
                if (!missing.empty())
                {
                    Revisions values;
                    gg_oid_array input{missing.data(), missing.size()};
                    Check(gg_repository_lookup_revisions(&values.value, history_gg.get(), input),
                        "hydrate history revisions");
                    for (std::size_t index = 0; index < values.value.count; ++index)
                    {
                        Revision revision = ConvertRevision(values.value.items[index], working_copy);
                        cached_revisions.insert_or_assign(revision.oid, std::move(revision));
                    }
                }
                std::unordered_map<std::string, Revision> result;
                result.reserve(oids.size());
                for (const git_oid& oid : oids)
                {
                    const std::string id = OidString(oid);
                    result.emplace(id, cached_revisions.at(id));
                }
                return result;
            };

            auto head_revisions = hydrate(heads);
            std::ranges::sort(heads, [&](const git_oid& left, const git_oid& right) {
                const Revision& left_revision = head_revisions.at(OidString(left));
                const Revision& right_revision = head_revisions.at(OidString(right));
                if (left_revision.timestamp != right_revision.timestamp)
                    return left_revision.timestamp > right_revision.timestamp;
                const bool left_primary = std::ranges::any_of(primary_heads,
                    [&](const git_oid& oid) { return git_oid_equal(&oid, &left) != 0; });
                const bool right_primary = std::ranges::any_of(primary_heads,
                    [&](const git_oid& oid) { return git_oid_equal(&oid, &right) != 0; });
                if (left_primary != right_primary) return left_primary;
                return git_oid_cmp(&left, &right) < 0;
            });

            // Resolving selected refs and IDs is cheap, while proving the
            // relationship between distant heads can require walking a large
            // repository without a commit graph. Publish every head immediately
            // as honest disconnected components, then replace this preview once
            // the worker has established the collapsed connections. Expansions
            // deliberately keep the existing detailed view and scroll anchor.
            if (request.expand.empty() && !stale())
            {
                auto preview = std::make_shared<HistoryView>();
                preview->repository_generation = request.query.repository_generation;
                preview->request = request.request;
                preview->skeleton = true;
                preview->search = request.query.search;
                for (const git_oid& oid : heads)
                {
                    const std::string id = OidString(oid);
                    HistoryItem item;
                    item.id = id;
                    item.kind = HistoryItemKind::Commit;
                    item.revision = head_revisions.at(id);
                    // Reachability is intentionally deferred with the graph
                    // proof. Keep preview commits conservatively locked rather
                    // than enabling a rewrite on incomplete information.
                    item.revision.pushed = true;
                    item.search_match = search_matches.contains(id);
                    preview->items.push_back(std::move(item));
                }
                Post(HistoryReady{std::move(preview)});
            }
            if (stale()) continue;

            const std::unordered_set<std::string> expanded(expansions.begin(), expansions.end());
            std::vector<git_oid> materialized;
            if (!request.expand.empty() && !cached_collapsed_materialized.empty())
                materialized = cached_collapsed_materialized;
            else
            {
                materialized = heads;
                std::deque<git_oid> pending(heads.begin(), heads.end());
                std::unordered_set<std::string> visited;
                std::size_t ordinary = 0;
                while (!pending.empty() && ordinary < 256 && !stale())
                {
                    const git_oid oid = pending.front();
                    pending.pop_front();
                    if (!visited.insert(OidString(oid)).second) continue;
                    git_commit* raw_commit = nullptr;
                    const int lookup = git_commit_lookup(&raw_commit, repository.get(), &oid);
                    if (lookup == GIT_ENOTFOUND) continue;
                    Check(lookup, "traverse bounded history");
                    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
                    if (std::ranges::none_of(materialized,
                            [&](const git_oid& value) { return git_oid_equal(&value, &oid) != 0; }))
                    {
                        materialized.push_back(oid);
                        ++ordinary;
                    }
                    const unsigned int parent_count = git_commit_parentcount(commit.get());
                    const unsigned int visible_parents = parent_count > 1 ? 1U : parent_count;
                    for (unsigned int index = 0; index < visible_parents; ++index)
                        pending.push_back(*git_commit_parent_id(commit.get(), index));
                }
                if (!stale()) cached_collapsed_materialized = materialized;
            }

            std::unordered_set<std::string> present;
            for (const git_oid& oid : materialized) present.insert(OidString(oid));
            const auto expand_from = [&](std::deque<git_oid> nearby, std::size_t limit) {
                std::unordered_set<std::string> local;
                for (std::size_t added = 0; !nearby.empty() && added < limit && !stale();)
                {
                    const git_oid oid = nearby.front();
                    nearby.pop_front();
                    const std::string id = OidString(oid);
                    if (!local.insert(id).second || present.contains(id)) continue;
                    git_commit* raw_commit = nullptr;
                    const int lookup = git_commit_lookup(&raw_commit, repository.get(), &oid);
                    if (lookup == GIT_ENOTFOUND) continue;
                    Check(lookup, "expand history");
                    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
                    materialized.push_back(oid);
                    present.insert(id);
                    ++added;
                    const unsigned int parent_count = git_commit_parentcount(commit.get());
                    const unsigned int visible_parents = parent_count > 1 && !expanded.contains(id)
                        ? 1U : parent_count;
                    for (unsigned int index = 0; index < visible_parents; ++index)
                        nearby.push_back(*git_commit_parent_id(commit.get(), index));
                }
            };
            for (const std::string& region : expansions)
            {
                if (stale()) break;
                if (!region.starts_with("region:"))
                {
                    if (!present.contains(region)) continue;
                    git_oid merge_oid{};
                    if (git_oid_fromstr(&merge_oid, region.c_str(),
                            git_repository_oid_type(repository.get())) != GIT_OK) continue;
                    git_commit* raw_merge = nullptr;
                    const int lookup = git_commit_lookup(&raw_merge, repository.get(), &merge_oid);
                    if (lookup == GIT_ENOTFOUND) continue;
                    Check(lookup, "expand merge history");
                    std::unique_ptr<git_commit, decltype(&git_commit_free)> merge(raw_merge, git_commit_free);
                    std::deque<git_oid> parents;
                    for (unsigned int index = 1; index < git_commit_parentcount(merge.get()); ++index)
                        parents.push_back(*git_commit_parent_id(merge.get(), index));
                    expand_from(std::move(parents), 128);
                    continue;
                }
                const std::size_t begin = std::string_view("region:").size();
                const std::size_t end = region.find(':', begin);
                if (end == std::string::npos) continue;
                git_oid seed{};
                if (git_oid_fromstr(&seed, region.substr(begin, end - begin).c_str(),
                        git_repository_oid_type(repository.get())) != GIT_OK) continue;
                expand_from(std::deque<git_oid>{seed}, 128);
            }

            const auto make_view = [&] {
                auto view = std::make_shared<HistoryView>();
                view->repository_generation = request.query.repository_generation;
                view->request = request.request;
                view->search = request.query.search;
                auto revisions = hydrate(materialized);
                std::unordered_set<std::string> regions;
                for (const git_oid& oid : materialized)
                {
                    const std::string id = OidString(oid);
                    HistoryItem item;
                    item.id = id;
                    item.kind = HistoryItemKind::Commit;
                    item.revision = revisions.at(id);
                    item.search_match = search_matches.contains(id);
                    const bool merge_expanded = item.revision.parents.size() > 1 && expanded.contains(id);
                    const std::size_t visible_parents = item.revision.parents.size() > 1
                            && !merge_expanded ? 1 : item.revision.parents.size();
                    for (std::size_t parent_index = 0; parent_index < visible_parents; ++parent_index)
                    {
                        const std::string& parent = item.revision.parents[parent_index];
                        if (present.contains(parent))
                        {
                            item.parents.push_back(parent);
                            continue;
                        }
                        const std::string region_id = "region:" + parent + ":";
                        item.parents.push_back(region_id);
                        if (regions.insert(region_id).second)
                        {
                            HistoryItem region;
                            region.id = region_id;
                            region.kind = HistoryItemKind::CollapsedRegion;
                            view->items.push_back(std::move(region));
                        }
                    }
                    view->items.push_back(std::move(item));
                }

                std::vector<std::string> preferred_heads;
                for (const git_oid& oid : heads) preferred_heads.push_back(OidString(oid));
                const std::vector<std::size_t> order = HistoryTopologicalOrder(view->items, preferred_heads);
                std::vector<HistoryItem> ordered;
                ordered.reserve(view->items.size());
                for (const std::size_t index : order) ordered.push_back(std::move(view->items[index]));
                view->items = std::move(ordered);

                std::unordered_set<std::string> pushed_items = search_matches;
                for (const git_oid& oid : conservatively_locked_heads)
                    pushed_items.insert(OidString(oid));
                for (const git_oid& tip : remote_tips)
                    if (present.contains(OidString(tip))) pushed_items.insert(OidString(tip));
                for (HistoryItem& item : view->items)
                    if (pushed_items.contains(item.id))
                    {
                        if (item.kind == HistoryItemKind::Commit) item.revision.pushed = true;
                        pushed_items.insert(item.parents.begin(), item.parents.end());
                    }
                return view;
            };

            if (!stale()) Post(HistoryReady{make_view()});

            // Description search is the only operation allowed to scan beyond
            // the bounded graph. It remains latest-wins, stops after 50
            // matches, and publishes useful partial results while walking.
            if (!request.query.search.empty() && !direct_search_match
                && !reusable_description_search && !stale())
            {
                std::vector<git_oid> found_description_matches;
                git_revwalk* raw_walk = nullptr;
                Check(git_revwalk_new(&raw_walk, repository.get()), "create history search");
                std::unique_ptr<git_revwalk, decltype(&git_revwalk_free)> walk(raw_walk, git_revwalk_free);
                git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL);
                Check(git_revwalk_push_glob(walk.get(), "refs/*"), "search repository history");
                auto last_publish = std::chrono::steady_clock::now();
                bool unpublished_matches = false;
                while (search_matches.size() < 50 && !stale())
                {
                    git_oid oid{};
                    const int next = git_revwalk_next(&oid, walk.get());
                    if (next == GIT_ITEROVER) break;
                    Check(next, "search repository history");
                    git_commit* raw_commit = nullptr;
                    if (git_commit_lookup(&raw_commit, repository.get(), &oid) != GIT_OK) continue;
                    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
                    const char* message = git_commit_message(commit.get());
                    if (message != nullptr && ContainsInsensitiveText(message, request.query.search))
                    {
                        const std::string id = OidString(oid);
                        search_matches.insert(id);
                        found_description_matches.push_back(oid);
                        add(materialized, oid);
                        unpublished_matches = true;
                        const auto now = std::chrono::steady_clock::now();
                        if (found_description_matches.size() == 1
                            || now - last_publish >= std::chrono::milliseconds(100))
                        {
                            if (!stale()) Post(HistoryReady{make_view()});
                            last_publish = now;
                            unpublished_matches = false;
                        }
                    }
                }
                if (!stale())
                {
                    if (unpublished_matches) Post(HistoryReady{make_view()});
                    cached_search_path = request.path;
                    cached_search_text = request.query.search;
                    cached_search_generation = request.query.repository_generation;
                    cached_description_matches = std::move(found_description_matches);
                }
            }
            if (stale()) continue;


        }
        catch (const std::exception& error)
        {
            cached_named_refs.reset();
            cached_references.reset();
            cached_workspaces.reset();
            cached_collapsed_materialized.clear();
            if (!stale()) Post(ErrorEvent{"history", error.what()});
        }
    }
}

void RepositoryEngine::Impl::RunInspector()
{
    RepositoryReadContext context;
    while (true)
    {
        InspectorRequest request;
        {
            std::unique_lock lock(inspector_mutex);
            inspector_cv.wait(lock, [this, &context] {
                return stopping || !inspector_requests.empty()
                    || (context.repository != nullptr && context.session != session.load());
            });
            if (stopping) break;
            if (inspector_requests.empty())
            {
                context.Reset();
                continue;
            }
            request = std::move(inspector_requests.front());
            inspector_requests.pop_front();
        }
        if (request.session != session.load() || request.request != inspector_request.load()) continue;
        try
        {
            context.Ensure(request.path, request.session, request.repository_generation, true);
            if (const auto* diff = std::get_if<LoadDiff>(&request.command))
                LoadPatch(*diff, context.repository.get(), context.gg.get(), request.snapshot_generation,
                    request.request, request.session);
            else if (const auto* file = std::get_if<LoadFileContent>(&request.command))
                LoadFile(*file, context.repository.get(), context.gg.get(), request.request, request.session);
            else
                LoadBlameFile(std::get<LoadBlame>(request.command), context.repository.get(), context.gg.get(),
                    request.snapshot_generation, request.request, request.session);
        }
        catch (const std::exception& error)
        {
            if (request.session == session.load() && request.request == inspector_request.load())
                Post(ErrorEvent{std::holds_alternative<LoadDiff>(request.command) ? "diff"
                    : std::holds_alternative<LoadFileContent>(request.command) ? "file" : "blame", error.what()});
        }
    }
}

void RepositoryEngine::Impl::Run()
{
    while (true)
    {
        std::optional<Command> command;
        {
            std::unique_lock lock(queue_mutex);
            queue_cv.wait(lock, [this] { return stopping || !commands.empty() || watcher.Changed(); });
            if (stopping)
                break;
            if (!commands.empty())
            {
                command = std::move(commands.front());
                commands.pop_front();
            }
        }
        if (command.has_value())
            Execute(*command);
        else if (gg != nullptr && !test_commands_suppressed)
        {
            // Editors and conflict tools commonly update a file through a
            // short delete/rename/write sequence. Give foreground commands
            // priority and avoid snapshotting those transient states.
            {
                std::unique_lock lock(queue_mutex);
                queue_cv.wait_for(lock, std::chrono::milliseconds(25),
                    [this] { return stopping || !commands.empty(); });
                if (stopping) break;
                if (!commands.empty()) continue;
            }
            const RepositoryWatcher::Changes changes = watcher.ConsumeChanges();
            if (changes.worktree || changes.metadata)
                Execute(Refresh{changes.worktree,
                    changes.full_scan ? std::vector<std::string>{} : std::move(changes.paths)});
        }
    }
}

} // namespace Ggui

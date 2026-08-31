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
#include <functional>
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

namespace Ggui
{
using namespace RepositoryInternal;

namespace
{
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

std::vector<std::size_t> HistoryTopologicalOrder(const std::vector<HistoryItem>& items)
{
    std::unordered_map<std::string_view, std::size_t> indexes;
    for (std::size_t index = 0; index < items.size(); ++index) indexes.emplace(items[index].id, index);
    std::vector<std::size_t> children(items.size());
    for (const HistoryItem& item : items)
        for (const std::string& parent : item.parents)
            if (const auto found = indexes.find(parent); found != indexes.end()) ++children[found->second];
    std::vector<std::size_t> ready;
    for (std::size_t index = 0; index < items.size(); ++index)
        if (children[index] == 0) ready.push_back(index);
    std::ranges::sort(ready, std::greater<>());
    std::vector<std::size_t> result;
    while (!ready.empty())
    {
        const std::size_t index = ready.back();
        ready.pop_back();
        result.push_back(index);
        for (const std::string& parent : items[index].parents)
            if (const auto found = indexes.find(parent); found != indexes.end() && --children[found->second] == 0)
            {
                ready.push_back(found->second);
                std::ranges::sort(ready, std::greater<>());
            }
    }
    if (result.size() != items.size()) throw std::runtime_error("history graph is not acyclic");
    return result;
}
} // namespace

void RepositoryEngine::Impl::Execute(const Command& command)
{
    const std::string name = CommandName(command);
    const bool quiet = std::holds_alternative<Refresh>(command) || std::holds_alternative<RebuildHistory>(command)
        || std::holds_alternative<ExpandHistoryRegion>(command)
        || std::holds_alternative<LoadDiff>(command)
        || std::holds_alternative<LoadFileContent>(command);
    if (!quiet)
        Post(OperationStarted{name});
    if (!std::holds_alternative<LoadDiff>(command) && !std::holds_alternative<LoadFileContent>(command)
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
                        || std::holds_alternative<Refresh>(queued)
                        || std::holds_alternative<RebuildHistory>(queued)
                        || std::holds_alternative<ExpandHistoryRegion>(queued);
                });
            }
            {
                std::lock_guard lock(event_mutex);
                std::erase_if(events, [](const Event& queued) {
                    return std::holds_alternative<SnapshotReady>(queued) || std::holds_alternative<DiffReady>(queued)
                        || std::holds_alternative<FileContentReady>(queued);
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
            PublishSnapshot();
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
                    PublishSnapshot();
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
            || std::holds_alternative<LoadFileContent>(command))
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

void RepositoryEngine::Impl::RequestHistory(HistoryQuery query, std::string expand)
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
        if (expand.empty())
        {
            if (query.repository_generation != active_history_query.repository_generation)
                expanded_history_regions.clear();
            active_history_query = query;
        }
        else if (std::ranges::find(expanded_history_regions, expand) == expanded_history_regions.end())
            expanded_history_regions.push_back(expand);
        history_request = HistoryRequest{active_history_query, std::move(expand), std::move(path), session.load(), request};
    }
    history_cv.notify_one();
}

void RepositoryEngine::Impl::RequestClosestBookmark(const std::shared_ptr<const RepoSnapshot>& snapshot)
{
    if (snapshot == nullptr) return;
    const std::string current = snapshot->working_copy.empty() ? snapshot->head : snapshot->working_copy;
    {
        std::lock_guard lock(closest_bookmark_mutex);
        if (closest_bookmark_completed_generation == snapshot->repository_generation
            || closest_bookmark_active_generation == snapshot->repository_generation
            || (closest_bookmark_request.has_value()
                && closest_bookmark_request->repository_generation == snapshot->repository_generation))
            return;
        const std::uint64_t request = ++closest_bookmark_request_version;
        closest_bookmark_request = ClosestBookmarkRequest{snapshot->root, current, snapshot->refs,
            snapshot->repository_generation, session.load(), request};
    }
    closest_bookmark_cv.notify_one();
}

void RepositoryEngine::Impl::RunClosestBookmark()
{
    while (true)
    {
        ClosestBookmarkRequest request;
        {
            std::unique_lock lock(closest_bookmark_mutex);
            closest_bookmark_cv.wait(lock, [this] { return stopping || closest_bookmark_request.has_value(); });
            if (stopping) break;
            request = std::move(*closest_bookmark_request);
            closest_bookmark_request.reset();
            closest_bookmark_active_generation = request.repository_generation;
        }
        const auto stale = [&] {
            return stopping.load() || request.session != session.load()
                || request.request != closest_bookmark_request_version.load();
        };
        std::string label;
        try
        {
            git_repository* raw = nullptr;
            Check(git_repository_open_ext(&raw, request.path.c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr),
                "open closest-bookmark repository");
            GitRepositoryPtr repository(raw);
            git_oid current{};
            const NamedRef* closest = nullptr;
            std::size_t closest_distance = std::numeric_limits<std::size_t>::max();
            if (git_oid_fromstr(&current, request.current.c_str(),
                    git_repository_oid_type(repository.get())) == GIT_OK)
            {
                for (const NamedRef& ref : request.refs)
                {
                    if (stale()) break;
                    if (ref.kind != GG_NAMED_REF_LOCAL_BOOKMARK
                        && ref.kind != GG_NAMED_REF_REMOTE_BOOKMARK) continue;
                    git_oid target{};
                    if (git_oid_fromstr(&target, ref.target.c_str(),
                            git_repository_oid_type(repository.get())) != GIT_OK) continue;
                    std::size_t ahead = 0;
                    std::size_t behind = 0;
                    const int graph = git_oid_equal(&current, &target) != 0 ? GIT_OK
                        : git_graph_ahead_behind(&ahead, &behind, repository.get(), &current, &target);
                    if (graph != GIT_OK || behind != 0) continue;
                    if (ahead < closest_distance || (ahead == closest_distance && closest != nullptr
                            && ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK
                            && closest->kind == GG_NAMED_REF_REMOTE_BOOKMARK))
                    {
                        closest = &ref;
                        closest_distance = ahead;
                    }
                }
            }
            if (!stale() && closest != nullptr)
                label = closest->remote.empty() ? closest->name : closest->remote + "/" + closest->name;
            if (!stale())
            {
                {
                    std::lock_guard lock(closest_bookmark_mutex);
                    closest_bookmark_completed_generation = request.repository_generation;
                }
                Post(ClosestBookmarkReady{request.repository_generation, std::move(label)});
            }
        }
        catch (const std::exception& error)
        {
            if (!stale()) Post(ErrorEvent{"closest bookmark", error.what()});
        }
        {
            std::lock_guard lock(closest_bookmark_mutex);
            if (closest_bookmark_active_generation == request.repository_generation)
                closest_bookmark_active_generation = 0;
        }
    }
}

void RepositoryEngine::Impl::RunHistory()
{
    std::string cached_search_path;
    std::string cached_search_text;
    std::uint64_t cached_search_generation = 0;
    std::vector<git_oid> cached_description_matches;
    while (true)
    {
        HistoryRequest request;
        std::vector<std::string> expansions;
        {
            std::unique_lock lock(history_mutex);
            history_cv.wait(lock, [this] { return stopping || history_request.has_value(); });
            if (stopping) break;
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
            git_repository* raw = nullptr;
            Check(git_repository_open_ext(&raw, request.path.c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr),
                "open history repository");
            GitRepositoryPtr repository(raw);
            gg_repository* raw_gg = nullptr;
            Check(gg_repository_attach(&raw_gg, repository.get()), "attach history repository");
            std::unique_ptr<gg_repository, decltype(&gg_repository_free)> history_gg(raw_gg, gg_repository_free);

            NamedRefs named;
            Check(gg_repository_named_refs(&named.value, history_gg.get()), "load history refs");
            Workspaces workspaces;
            Check(gg_repository_workspaces(&workspaces.value, history_gg.get()), "load history workspaces");
            const auto selected = [](const std::vector<std::string>& values, std::string_view value) {
                return std::ranges::find(values, value) != values.end();
            };
            std::vector<git_oid> heads;
            std::vector<git_oid> selected_bookmarks;
            std::vector<git_oid> unselected_bookmarks;
            std::vector<git_oid> remote_tips;
            const auto add = [](std::vector<git_oid>& values, const git_oid& oid) {
                if (std::ranges::none_of(values, [&](const git_oid& value) { return git_oid_equal(&value, &oid) != 0; }))
                    values.push_back(oid);
            };
            git_oid working{};
            std::string working_copy;
            if (gg_repository_working_copy(&working, history_gg.get()) == GIT_OK)
            {
                add(heads, working);
                working_copy = OidString(working);
            }
            git_reference* raw_head = nullptr;
            if (git_repository_head(&raw_head, repository.get()) == GIT_OK)
            {
                std::unique_ptr<git_reference, decltype(&git_reference_free)> head(raw_head, git_reference_free);
                if (const git_oid* target = git_reference_target(head.get()))
                {
                    add(heads, *target);
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
                    add(heads, ref.target);
            }
            // Other workspaces are visible leaf heads only when they continue a
            // selected bookmark and are not inside a branch claimed by an
            // unselected bookmark.
            for (std::size_t index = 0; index < workspaces.value.count; ++index)
            {
                const git_oid& candidate = workspaces.value.items[index].working_copy;
                bool descended = false;
                for (const git_oid& bookmark : selected_bookmarks)
                {
                    const int value = git_oid_equal(&candidate, &bookmark) != 0 ? 1
                        : git_graph_descendant_of(repository.get(), &candidate, &bookmark);
                    Check(value, "inspect selected bookmark descendants");
                    descended |= value != 0;
                }
                bool claimed = false;
                for (const git_oid& bookmark : unselected_bookmarks)
                {
                    const int value = git_oid_equal(&candidate, &bookmark) != 0 ? 1
                        : git_graph_descendant_of(repository.get(), &candidate, &bookmark);
                    Check(value, "inspect unselected bookmark descendants");
                    claimed |= value != 0;
                }
                if (descended && !claimed) add(heads, candidate);
            }
            git_reference_iterator* raw_visible = nullptr;
            if (git_reference_iterator_glob_new(
                    &raw_visible, repository.get(), "refs/gg/visible-heads/*") == GIT_OK)
            {
                std::unique_ptr<git_reference_iterator, decltype(&git_reference_iterator_free)>
                    visible(raw_visible, git_reference_iterator_free);
                git_reference* raw_ref = nullptr;
                while (git_reference_next(&raw_ref, visible.get()) == GIT_OK)
                {
                    std::unique_ptr<git_reference, decltype(&git_reference_free)> ref(raw_ref, git_reference_free);
                    const git_oid* target = git_reference_target(ref.get());
                    if (target == nullptr) continue;
                    bool descended = false;
                    for (const git_oid& bookmark : selected_bookmarks)
                    {
                        const int value = git_oid_equal(target, &bookmark) != 0 ? 1
                            : git_graph_descendant_of(repository.get(), target, &bookmark);
                        if (value >= 0) descended |= value != 0;
                    }
                    bool claimed = false;
                    for (const git_oid& bookmark : unselected_bookmarks)
                    {
                        const int value = git_oid_equal(target, &bookmark) != 0 ? 1
                            : git_graph_descendant_of(repository.get(), target, &bookmark);
                        if (value >= 0) claimed |= value != 0;
                    }
                    if (descended && !claimed) add(heads, *target);
                }
            }
            if (stale()) continue;

            std::unordered_set<std::string> search_matches;
            std::unordered_set<std::string> description_search_matches;
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

            std::ranges::sort(heads, [](const git_oid& left, const git_oid& right) {
                return git_oid_cmp(&left, &right) < 0;
            });

            // Merge bases are mandatory skeleton junctions. Multiple bases are
            // retained for criss-cross histories; ENOTFOUND leaves independent
            // components rather than inventing an edge.
            std::vector<git_oid> mandatory = heads;
            for (std::size_t left = 0; left < heads.size(); ++left)
                for (std::size_t right = left + 1; right < heads.size(); ++right)
                {
                    git_oidarray bases{};
                    const int result = git_merge_bases(&bases, repository.get(), &heads[left], &heads[right]);
                    if (result == GIT_OK)
                    {
                        for (std::size_t index = 0; index < bases.count; ++index) add(mandatory, bases.ids[index]);
                        git_oidarray_dispose(&bases);
                    }
                    else if (result != GIT_ENOTFOUND) Check(result, "find history merge bases");
                }
            if (reusable_description_search)
                for (const git_oid& oid : cached_description_matches)
                {
                    search_matches.insert(OidString(oid));
                    description_search_matches.insert(OidString(oid));
                    add(mandatory, oid);
                }
            std::ranges::sort(mandatory, [](const git_oid& left, const git_oid& right) {
                return git_oid_cmp(&left, &right) < 0;
            });

            const auto hydrate = [&](const std::vector<git_oid>& oids) {
                Revisions values;
                gg_oid_array input{const_cast<git_oid*>(oids.data()), oids.size()};
                Check(gg_repository_lookup_revisions(&values.value, history_gg.get(), input), "hydrate history revisions");
                std::unordered_map<std::string, Revision> result;
                for (std::size_t index = 0; index < values.value.count; ++index)
                {
                    Revision revision = ConvertRevision(values.value.items[index], working_copy);
                    result.emplace(revision.oid, std::move(revision));
                }
                return result;
            };

            // Reachability queries on a repository without a commit graph can
            // be expensive even though the displayed set is bounded. Query
            // only the mandatory junctions, then propagate a positive result
            // through the honest in-view ancestry. This is exact: every
            // propagated edge is either a direct Git parent or a collapsed
            // relation whose reachability was proven while building it.
            std::unordered_map<std::string, bool> remote_reachability;
            const auto remotely_reachable = [&](const git_oid& oid) {
                const std::string id = OidString(oid);
                if (const auto found = remote_reachability.find(id); found != remote_reachability.end())
                    return found->second;
                bool pushed = false;
                for (const git_oid& tip : remote_tips)
                {
                    const int reachable = git_oid_equal(&tip, &oid) != 0 ? 1
                        : git_graph_descendant_of(repository.get(), &tip, &oid);
                    if (reachable < 0)
                    {
                        // Preserve the existing conservative lock on graph
                        // errors rather than enabling a destructive rewrite.
                        pushed = true;
                        break;
                    }
                    if (reachable != 0) { pushed = true; break; }
                }
                remote_reachability.emplace(id, pushed);
                return pushed;
            };

            // The skeleton pays once to prove how mandatory junctions are
            // related. Detailed views reuse those relations and only locate
            // one matching materialized frontier per relation instead of
            // repeating an ancestry query for every frontier commit.
            std::unordered_map<std::string, std::vector<std::string>> skeleton_connections;

            const auto make_view = [&](const std::vector<git_oid>& materialized, bool skeleton) {
                auto view = std::make_shared<HistoryView>();
                view->repository_generation = request.query.repository_generation;
                view->request = request.request;
                view->skeleton = skeleton;
                view->search = request.query.search;
                auto revisions = hydrate(materialized);
                std::unordered_set<std::string> present;
                for (const git_oid& oid : materialized) present.insert(OidString(oid));
                std::unordered_map<std::string, std::string> frontier_regions;
                std::vector<std::string> junctions;
                for (const git_oid& oid : mandatory) junctions.push_back(OidString(oid));
                for (const git_oid& oid : materialized)
                {
                    const std::string id = OidString(oid);
                    HistoryItem item;
                    item.id = id;
                    item.kind = HistoryItemKind::Commit;
                    item.revision = revisions.at(id);
                    item.search_match = search_matches.contains(id);
                    for (const std::string& parent : item.revision.parents)
                    {
                        if (present.contains(parent))
                        {
                            item.parents.push_back(parent);
                            continue;
                        }
                        if (!skeleton)
                        {
                            const std::string region_id = "region:" + parent + ":";
                            item.parents.push_back(region_id);
                            frontier_regions.emplace(parent, region_id);
                            if (std::ranges::none_of(view->items,
                                    [&](const HistoryItem& existing) { return existing.id == region_id; }))
                            {
                                HistoryItem region;
                                region.id = region_id;
                                region.kind = HistoryItemKind::CollapsedRegion;
                                view->items.push_back(std::move(region));
                            }
                            continue;
                        }
                        git_oid parent_oid{};
                        Check(git_oid_fromstr(&parent_oid, parent.c_str(), git_repository_oid_type(repository.get())),
                            "parse history frontier");
                        std::vector<std::string> boundaries;
                        for (const std::string& candidate : junctions)
                        {
                            if (candidate == id) continue;
                            git_oid candidate_oid{};
                            Check(git_oid_fromstr(&candidate_oid, candidate.c_str(), git_repository_oid_type(repository.get())),
                                "parse history junction");
                            const int related = git_oid_equal(&parent_oid, &candidate_oid) != 0 ? 1
                                : git_graph_descendant_of(repository.get(), &parent_oid, &candidate_oid);
                            // A shallow parent has an honest Git edge but no
                            // local object with which to prove reachability.
                            // Leave its region disconnected rather than
                            // manufacturing a junction.
                            if (related < 0) continue;
                            if (related != 0) boundaries.push_back(candidate);
                        }
                        // Keep every incomparable nearest junction. This is
                        // essential for criss-cross histories with multiple
                        // merge bases.
                        const std::vector<std::string> related_boundaries = boundaries;
                        boundaries.clear();
                        for (const std::string& candidate : related_boundaries)
                        {
                            git_oid candidate_oid{};
                            Check(git_oid_fromstr(&candidate_oid, candidate.c_str(),
                                git_repository_oid_type(repository.get())), "parse candidate junction");
                            const bool shadowed = std::ranges::any_of(related_boundaries, [&](const std::string& other) {
                                if (other == candidate) return false;
                                git_oid other_oid{};
                                Check(git_oid_fromstr(&other_oid, other.c_str(),
                                    git_repository_oid_type(repository.get())), "parse nearer junction");
                                const int nearer = git_graph_descendant_of(
                                    repository.get(), &other_oid, &candidate_oid);
                                Check(nearer, "choose history junction");
                                return nearer != 0;
                            });
                            if (!shadowed) boundaries.push_back(candidate);
                        }
                        if (boundaries.empty()) boundaries.emplace_back();
                        for (const std::string& boundary : boundaries)
                        {
                            const std::string region_id = "region:" + parent + ":" + boundary;
                            item.parents.push_back(region_id);
                            if (std::ranges::none_of(view->items,
                                    [&](const HistoryItem& existing) { return existing.id == region_id; }))
                            {
                                HistoryItem region;
                                region.id = region_id;
                                region.kind = HistoryItemKind::CollapsedRegion;
                                if (!boundary.empty()) region.parents.push_back(boundary);
                                view->items.push_back(std::move(region));
                            }
                            if (!boundary.empty())
                            {
                                auto& connections = skeleton_connections[id];
                                if (std::ranges::find(connections, boundary) == connections.end())
                                    connections.push_back(boundary);
                            }
                        }
                    }
                    view->items.push_back(std::move(item));
                }

                if (!skeleton)
                {
                    const auto find_frontier = [&](const std::string& boundary,
                                                   const std::vector<std::string>& frontiers)
                        -> std::optional<std::string> {
                        git_oid boundary_oid{};
                        Check(git_oid_fromstr(&boundary_oid, boundary.c_str(),
                            git_repository_oid_type(repository.get())), "parse reused history junction");
                        std::function<std::optional<std::string>(std::size_t, std::size_t)> find =
                            [&](std::size_t begin, std::size_t end) -> std::optional<std::string> {
                                if (begin >= end) return std::nullopt;
                                std::vector<git_oid> descendants;
                                descendants.reserve(end - begin);
                                for (std::size_t index = begin; index < end; ++index)
                                {
                                    git_oid oid{};
                                    if (git_oid_fromstr(&oid, frontiers[index].c_str(),
                                            git_repository_oid_type(repository.get())) == GIT_OK)
                                        descendants.push_back(oid);
                                }
                                if (descendants.empty()) return std::nullopt;
                                const int reachable = git_graph_reachable_from_any(repository.get(), &boundary_oid,
                                    descendants.data(), descendants.size());
                                if (reachable <= 0) return std::nullopt;
                                if (end - begin == 1) return frontiers[begin];
                                const std::size_t middle = begin + (end - begin) / 2;
                                if (auto result = find(begin, middle)) return result;
                                return find(middle, end);
                            };
                        return find(0, frontiers.size());
                    };

                    for (const auto& [anchor, boundaries] : skeleton_connections)
                    {
                        if (!present.contains(anchor)) continue;
                        std::vector<std::string> pending{anchor};
                        std::unordered_set<std::string> seen;
                        std::vector<std::string> frontiers;
                        while (!pending.empty())
                        {
                            std::string current = std::move(pending.back());
                            pending.pop_back();
                            if (!seen.insert(current).second) continue;
                            const auto revision = revisions.find(current);
                            if (revision == revisions.end()) continue;
                            for (const std::string& parent : revision->second.parents)
                                if (present.contains(parent)) pending.push_back(parent);
                                else if (std::ranges::find(frontiers, parent) == frontiers.end())
                                    frontiers.push_back(parent);
                        }
                        for (const std::string& boundary : boundaries)
                        {
                            if (seen.contains(boundary)) continue;
                            const auto frontier = find_frontier(boundary, frontiers);
                            if (!frontier.has_value()) continue;
                            const auto region_id = frontier_regions.find(*frontier);
                            if (region_id == frontier_regions.end()) continue;
                            const auto region = std::ranges::find(view->items, region_id->second, &HistoryItem::id);
                            if (region != view->items.end()
                                && std::ranges::find(region->parents, boundary) == region->parents.end())
                                region->parents.push_back(boundary);
                        }
                    }

                }
                const std::vector<std::size_t> order = HistoryTopologicalOrder(view->items);
                std::vector<HistoryItem> ordered;
                ordered.reserve(view->items.size());
                for (const std::size_t index : order) ordered.push_back(std::move(view->items[index]));
                view->items = std::move(ordered);

                std::unordered_set<std::string> pushed_items;
                for (const git_oid& tip : remote_tips)
                    if (present.contains(OidString(tip))) pushed_items.insert(OidString(tip));
                const auto propagate_pushed = [&] {
                    for (HistoryItem& item : view->items)
                        if (pushed_items.contains(item.id))
                        {
                            if (item.kind == HistoryItemKind::Commit) item.revision.pushed = true;
                            pushed_items.insert(item.parents.begin(), item.parents.end());
                        }
                };
                // A visible remote tip proves all of its displayed ancestry
                // without any graph query. Only disconnected mandatory
                // junctions need an exact repository check.
                propagate_pushed();
                for (const git_oid& oid : mandatory)
                    if (present.contains(OidString(oid)) && !pushed_items.contains(OidString(oid)))
                    {
                        const std::string id = OidString(oid);
                        // Free-text matches may originate at unrelated refs.
                        // Keep an unproven match conservatively locked rather
                        // than delaying the search view with up to 50 deep
                        // reachability queries. Connected matches were
                        // already proven by propagation above.
                        if (description_search_matches.contains(id) || remotely_reachable(oid))
                            pushed_items.insert(id);
                    }
                propagate_pushed();
                return view;
            };

            // Expansion keeps the currently rendered detailed view in place;
            // replacing it with the sparse skeleton would cause a visible
            // jump and destroy the scroll anchor while nearby commits load.
            if (!stale() && request.expand.empty())
            {
                auto skeleton = make_view(mandatory, true);
                if (!stale()) Post(HistoryReady{std::move(skeleton)});
            }
            if (stale()) continue;

            std::vector<git_oid> materialized = mandatory;
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
                if (std::ranges::none_of(materialized, [&](const git_oid& value) { return git_oid_equal(&value, &oid) != 0; }))
                {
                    materialized.push_back(oid);
                    ++ordinary;
                }
                for (unsigned int index = 0; index < git_commit_parentcount(commit.get()); ++index)
                    pending.push_back(*git_commit_parent_id(commit.get(), index));
            }
            for (const std::string& region : expansions)
            {
                if (stale()) break;
                const std::size_t begin = std::string_view("region:").size();
                const std::size_t end = region.find(':', begin);
                if (end == std::string::npos) continue;
                git_oid seed{};
                if (git_oid_fromstr(&seed, region.substr(begin, end - begin).c_str(),
                        git_repository_oid_type(repository.get())) != GIT_OK) continue;
                std::deque<git_oid> nearby{seed};
                std::unordered_set<std::string> local;
                for (std::size_t added = 0; !nearby.empty() && added < 128 && !stale();)
                {
                    const git_oid oid = nearby.front(); nearby.pop_front();
                    if (!local.insert(OidString(oid)).second) continue;
                    git_commit* raw_commit = nullptr;
                    const int lookup = git_commit_lookup(&raw_commit, repository.get(), &oid);
                    if (lookup == GIT_ENOTFOUND) continue;
                    Check(lookup, "expand history region");
                    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
                    if (std::ranges::none_of(materialized,
                            [&](const git_oid& value) { return git_oid_equal(&value, &oid) != 0; }))
                    { materialized.push_back(oid); ++added; }
                    for (unsigned int index = 0; index < git_commit_parentcount(commit.get()); ++index)
                        nearby.push_back(*git_commit_parent_id(commit.get(), index));
                }
            }

            // Description search is the only operation allowed to scan beyond
            // the bounded graph. It is latest-wins and stops after 50 matches.
            if (!request.query.search.empty() && !direct_search_match
                && !reusable_description_search && !stale())
            {
                std::vector<git_oid> found_description_matches;
                git_revwalk* raw_walk = nullptr;
                Check(git_revwalk_new(&raw_walk, repository.get()), "create history search");
                std::unique_ptr<git_revwalk, decltype(&git_revwalk_free)> walk(raw_walk, git_revwalk_free);
                git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL);
                Check(git_revwalk_push_glob(walk.get(), "refs/*"), "search repository history");
                git_oid oid{};
                while (search_matches.size() < 50 && !stale() && git_revwalk_next(&oid, walk.get()) == GIT_OK)
                {
                    git_commit* raw_commit = nullptr;
                    if (git_commit_lookup(&raw_commit, repository.get(), &oid) != GIT_OK) continue;
                    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
                    const char* message = git_commit_message(commit.get());
                    if (message != nullptr && ContainsInsensitiveText(message, request.query.search))
                    {
                        const std::string id = OidString(oid);
                        search_matches.insert(id);
                        description_search_matches.insert(id);
                        found_description_matches.push_back(oid);
                        add(materialized, oid);
                        // Treat an older match as a junction so the nearest
                        // collapsed frontier connects to it instead of
                        // rendering related history as a separate component.
                        add(mandatory, oid);
                    }
                }
                if (!stale())
                {
                    cached_search_path = request.path;
                    cached_search_text = request.query.search;
                    cached_search_generation = request.query.repository_generation;
                    cached_description_matches = std::move(found_description_matches);
                }
            }
            if (!stale())
            {
                auto detail = make_view(materialized, false);
                if (!stale()) Post(HistoryReady{std::move(detail)});
            }
        }
        catch (const std::exception& error)
        {
            if (!stale()) Post(ErrorEvent{"history", error.what()});
        }
    }
}

void RepositoryEngine::Impl::RunInspector()
{
    while (true)
    {
        InspectorRequest request;
        {
            std::unique_lock lock(inspector_mutex);
            inspector_cv.wait(lock, [this] { return stopping || !inspector_requests.empty(); });
            if (stopping) break;
            request = std::move(inspector_requests.front());
            inspector_requests.pop_front();
        }
        if (request.session != session.load() || request.request != inspector_request.load()) continue;
        try
        {
            std::string path;
            {
                std::lock_guard lock(history_mutex);
                path = repository_path;
            }
            git_repository* raw = nullptr;
            Check(git_repository_open_ext(&raw, path.c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr),
                "open inspector repository");
            GitRepositoryPtr repository(raw);
            gg_repository* read_gg = nullptr;
            Check(gg_repository_attach(&read_gg, repository.get()), "attach inspector repository");
            std::unique_ptr<gg_repository, decltype(&gg_repository_free)> owned_gg(read_gg, gg_repository_free);
            const std::uint64_t snapshot_generation = generation;
            if (const auto* diff = std::get_if<LoadDiff>(&request.command))
                LoadPatch(*diff, repository.get(), read_gg, snapshot_generation, request.request, request.session);
            else
                LoadFile(std::get<LoadFileContent>(request.command), repository.get(), read_gg,
                    request.request, request.session);
        }
        catch (const std::exception& error)
        {
            if (request.session == session.load() && request.request == inspector_request.load())
                Post(ErrorEvent{std::holds_alternative<LoadDiff>(request.command) ? "diff" : "file", error.what()});
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

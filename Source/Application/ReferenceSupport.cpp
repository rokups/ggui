// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Ggui::ApplicationInternal
{
namespace
{
ImU32 NamedRefBadgeColor(std::string_view name, const std::vector<NamedRef>& refs,
    gg_named_ref_kind local_kind, gg_named_ref_kind remote_kind, ImU32 local_color)
{
    const auto local = std::ranges::find_if(refs, [&](const NamedRef& ref) {
        return ref.kind == local_kind && ref.name == name;
    });
    const bool remote = std::ranges::any_of(refs, [&](const NamedRef& ref) {
        return ref.kind == remote_kind && ref.name == name;
    });
    if (local == refs.end()) return kBadgeRemote;
    if (!remote) return local_color;
    const bool synchronized = std::ranges::all_of(refs, [&](const NamedRef& ref) {
        return ref.kind != remote_kind || ref.name != name || ref.target == local->target;
    });
    return synchronized ? kBadgeBookmarkSynced : kBadgeBookmarkDiverged;
}
} // namespace

ImU32 BookmarkBadgeColor(std::string_view name, const std::vector<NamedRef>& refs)
{
    return NamedRefBadgeColor(name, refs, GG_NAMED_REF_LOCAL_BOOKMARK,
        GG_NAMED_REF_REMOTE_BOOKMARK, kBadgeBookmark);
}

ImU32 RefBadgeColor(const NamedRef& ref, const std::vector<NamedRef>& refs)
{
    const bool bookmark = ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK
        || ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK;
    const gg_named_ref_kind local_kind = bookmark ? GG_NAMED_REF_LOCAL_BOOKMARK : GG_NAMED_REF_LOCAL_TAG;
    const gg_named_ref_kind remote_kind = bookmark ? GG_NAMED_REF_REMOTE_BOOKMARK : GG_NAMED_REF_REMOTE_TAG;
    const bool local_here = std::ranges::any_of(refs, [&](const NamedRef& candidate) {
        return candidate.kind == local_kind && candidate.name == ref.name && candidate.target == ref.target;
    });
    const bool remote_here = std::ranges::any_of(refs, [&](const NamedRef& candidate) {
        return candidate.kind == remote_kind && candidate.name == ref.name && candidate.target == ref.target;
    });
    if (!local_here) return kBadgeRemote;
    const bool remote_elsewhere = std::ranges::any_of(refs, [&](const NamedRef& candidate) {
        return candidate.kind == remote_kind && candidate.name == ref.name && candidate.target != ref.target;
    });
    if (remote_elsewhere) return kBadgeBookmarkDiverged;
    if (remote_here) return kBadgeBookmarkSynced;
    return bookmark ? kBadgeBookmark : kBadgeTag;
}

std::string ReferenceLabel(const NamedRef& ref)
{
    return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && !ref.remote.empty()
        ? ref.remote + "/" + ref.name : ref.name;
}

std::pair<std::string, std::size_t> ReferenceBadgeLabel(const NamedRef& ref, const std::vector<NamedRef>& refs)
{
    const bool bookmark = ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK
        || ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK;
    const gg_named_ref_kind local_kind = bookmark ? GG_NAMED_REF_LOCAL_BOOKMARK : GG_NAMED_REF_LOCAL_TAG;
    const gg_named_ref_kind remote_kind = bookmark ? GG_NAMED_REF_REMOTE_BOOKMARK : GG_NAMED_REF_REMOTE_TAG;
    const auto local = std::ranges::find_if(refs, [&](const NamedRef& candidate) {
        return candidate.kind == local_kind && candidate.name == ref.name && candidate.target == ref.target;
    });
    if (local != refs.end())
        return ref.kind == local_kind ? std::pair{ref.name, std::size_t{0}}
                                      : std::pair<std::string, std::size_t>{};
    const auto remote = std::ranges::find_if(refs, [&](const NamedRef& candidate) {
        return candidate.kind == remote_kind && candidate.name == ref.name && candidate.target == ref.target;
    });
    return remote != refs.end() && ref.kind == remote_kind && ref.remote == remote->remote
            && ref.target == remote->target
        ? std::pair{ref.name, std::size_t{0}} : std::pair<std::string, std::size_t>{};
}

const Remote* DefaultRemote(const RepoSnapshot& snapshot)
{
    const auto origin = std::ranges::find(snapshot.remotes, "origin", &Remote::name);
    return origin != snapshot.remotes.end() ? &*origin
                                            : snapshot.remotes.empty() ? nullptr : &snapshot.remotes.front();
}

const NamedRef* BookmarkAt(const RepoSnapshot& snapshot, const std::string& revision)
{
    const auto tracked = std::ranges::find_if(snapshot.refs, [&](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.target == revision && ref.tracked;
    });
    if (tracked != snapshot.refs.end())
        return &*tracked;
    const auto local = std::ranges::find_if(snapshot.refs, [&](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.target == revision;
    });
    return local == snapshot.refs.end() ? nullptr : &*local;
}

std::string RemoteForBookmark(const RepoSnapshot& snapshot, std::string_view bookmark)
{
    const auto tracked = std::ranges::find_if(snapshot.refs, [&](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.name == bookmark
            && std::ranges::find(snapshot.remotes, ref.remote, &Remote::name) != snapshot.remotes.end();
    });
    if (tracked != snapshot.refs.end())
        return tracked->remote;
    const Remote* remote = DefaultRemote(snapshot);
    return remote == nullptr ? "" : remote->name;
}

std::string RefRemotes(const std::vector<NamedRef>& refs, std::string_view name, gg_named_ref_kind kind)
{
    std::string result;
    for (const NamedRef& ref : refs)
    {
        if (ref.kind != kind || ref.name != name || ref.remote.empty())
            continue;
        if (!result.empty()) result += ", ";
        result += ref.remote;
        if (kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.desync_known)
        {
            if (ref.remote_commits != 0)
                result += " -" + std::to_string(ref.remote_commits);
            if (ref.local_commits != 0)
                result += " +" + std::to_string(ref.local_commits);
        }
    }
    return result;
}

const std::string& CurrentCommit(const RepoSnapshot& snapshot)
{
    return snapshot.working_copy.empty() ? snapshot.head : snapshot.working_copy;
}

const Revision* ResolveSnapshotRevision(const RepoSnapshot& snapshot, std::string_view identifier)
{
    if (identifier.empty())
        return nullptr;
    if (identifier == "@")
        identifier = CurrentCommit(snapshot);
    bool ambiguous = false;
    const Revision* result = nullptr;
    for (const Revision& revision : snapshot.revisions)
    {
        const bool matches = revision.oid.starts_with(identifier)
            || std::ranges::any_of(revision.aliases,
                [&](const std::string& alias) { return alias.starts_with(identifier); });
        if (!matches)
            continue;
        if (result != nullptr && result->oid != revision.oid)
        {
            ambiguous = true;
            result = nullptr;
            break;
        }
        result = &revision;
    }
    if (result != nullptr || ambiguous)
        return result;
    for (const NamedRef& ref : snapshot.refs)
        if ((ref.remote.empty() && ref.name == identifier)
            || (!ref.remote.empty() && ref.remote + "/" + ref.name == identifier))
            if (const auto revision = std::ranges::find(snapshot.revisions, ref.target, &Revision::oid);
                revision != snapshot.revisions.end())
                return &*revision;
    return nullptr;
}

const Revision* RebaseBranchRoot(
    const RepoSnapshot& snapshot, std::string_view source, std::string_view destination)
{
    const Revision* source_revision = ResolveSnapshotRevision(snapshot, source);
    const Revision* destination_revision = ResolveSnapshotRevision(snapshot, destination);
    if (source_revision == nullptr || destination_revision == nullptr)
        return nullptr;
    std::unordered_map<std::string_view, const Revision*> by_oid;
    for (const Revision& revision : snapshot.revisions)
        by_oid.emplace(revision.oid, &revision);
    const auto ancestors = [&](const Revision* start) {
        std::unordered_set<std::string_view> result;
        std::vector<const Revision*> pending{start};
        while (!pending.empty())
        {
            const Revision* revision = pending.back();
            pending.pop_back();
            if (!result.emplace(revision->oid).second)
                continue;
            for (const std::string& parent : revision->parents)
                if (const auto found = by_oid.find(parent); found != by_oid.end())
                    pending.push_back(found->second);
        }
        return result;
    };
    std::unordered_set<std::string_view> source_only = ancestors(source_revision);
    for (std::string_view oid : ancestors(destination_revision))
        source_only.erase(oid);
    const Revision* root = nullptr;
    for (std::string_view oid : source_only)
    {
        const Revision* revision = by_oid.at(oid);
        if (std::ranges::none_of(revision->parents, [&](const std::string& parent) {
                return source_only.contains(parent);
            }))
        {
            if (root != nullptr)
                return nullptr;
            root = revision;
        }
    }
    return root;
}

} // namespace Ggui::ApplicationInternal

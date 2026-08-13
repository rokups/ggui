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

ImU32 BookmarkBadgeColor(std::string_view name, const std::vector<NamedRef>& refs)
{
    const auto local = std::ranges::find_if(refs, [&](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == name;
    });
    const bool remote = std::ranges::any_of(refs, [&](const NamedRef& ref) {
        return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.name == name;
    });
    if (local == refs.end())
        return kBadgeRemote;
    if (!remote)
        return kBadgeBookmark;
    const bool synchronized = std::ranges::all_of(refs, [&](const NamedRef& ref) {
        return ref.kind != GG_NAMED_REF_REMOTE_BOOKMARK || ref.name != name || ref.target == local->target;
    });
    return synchronized ? kBadgeBookmarkSynced : kBadgeBookmarkDiverged;
}

ImU32 RefBadgeColor(const NamedRef& ref, const std::vector<NamedRef>& refs)
{
    if (ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK || ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK)
        return BookmarkBadgeColor(ref.name, refs);
    if (ref.kind == GG_NAMED_REF_LOCAL_TAG)
        return kBadgeTag;
    return kBadgeRemote;
}

std::string ReferenceLabel(const NamedRef& ref)
{
    return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && !ref.remote.empty()
        ? ref.remote + "/" + ref.name : ref.name;
}

std::pair<std::string, std::size_t> ReferenceBadgeLabel(const NamedRef& ref, const std::vector<NamedRef>& refs)
{
    if (ref.kind != GG_NAMED_REF_LOCAL_BOOKMARK && ref.kind != GG_NAMED_REF_REMOTE_BOOKMARK)
        return {ReferenceLabel(ref), 0};
    const auto local = std::ranges::find_if(refs, [&](const NamedRef& candidate) {
        return candidate.kind == GG_NAMED_REF_LOCAL_BOOKMARK && candidate.name == ref.name
            && candidate.target == ref.target;
    });
    const auto remote = std::ranges::find_if(refs, [&](const NamedRef& candidate) {
        return candidate.kind == GG_NAMED_REF_REMOTE_BOOKMARK && candidate.name == ref.name
            && candidate.target == ref.target && !candidate.remote.empty();
    });
    if (local == refs.end() || remote == refs.end())
        return {ReferenceLabel(ref), 0};
    if (ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK)
        return {};
    return {ReferenceLabel(*remote), remote->remote.size() + 1};
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

const NamedRef* ClosestBookmark(const RepoSnapshot& snapshot, const std::string& revision)
{
    std::vector<std::string_view> pending{revision};
    std::unordered_set<std::string_view> visited;
    for (std::size_t index = 0; index < pending.size(); ++index)
    {
        const std::string_view oid = pending[index];
        if (!visited.emplace(oid).second)
            continue;
        if (const NamedRef* local = BookmarkAt(snapshot, std::string(oid)); local != nullptr)
            return local;
        const auto remote = std::ranges::find_if(snapshot.refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.target == oid;
        });
        if (remote != snapshot.refs.end())
            return &*remote;
        const auto node = std::ranges::find(snapshot.revisions, oid, &Revision::oid);
        if (node != snapshot.revisions.end())
            for (const std::string& parent : node->parents)
                pending.push_back(parent);
    }
    return nullptr;
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

// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>
#include <imgui_stdlib.h>

#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

void Application::EnsureRemoteSelection()
{
    if (_snapshot == nullptr)
    {
        _selected_remotes.clear();
        return;
    }
    std::erase_if(_selected_remotes, [this](const std::string& name) {
        return std::ranges::find(_snapshot->remotes, name, &Remote::name) == _snapshot->remotes.end();
    });
    if (_selected_remotes.empty())
        _selected_remotes_user_selected = false;
    if (_selected_remotes_user_selected || _snapshot->remotes.empty())
        return;
    const auto origin = std::ranges::find(_snapshot->remotes, "origin", &Remote::name);
    if (origin != _snapshot->remotes.end())
        _selected_remotes = {origin->name};
    else if (_selected_remotes.empty())
        _selected_remotes = {_snapshot->remotes.front().name};
}

bool Application::IsSelectedRemote(std::string_view remote) const
{
    return std::ranges::find(_selected_remotes, remote) != _selected_remotes.end();
}

bool Application::IsVisibleBookmarkRef(const NamedRef& ref) const
{
    return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK
        || (ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && IsSelectedRemote(ref.remote));
}

void Application::RestoreRepositorySelections(const std::string& root)
{
    _selected_remotes.clear();
    _selected_remotes_user_selected = false;
    if (const auto found = _repository_selected_remotes.find(root);
        found != _repository_selected_remotes.end())
    {
        _selected_remotes = found->second;
        _selected_remotes_user_selected = true;
    }
    EnsureRemoteSelection();

    _visible_bookmarks.clear();
    _visible_bookmarks_user_selected = false;
    if (const auto found = _repository_visible_bookmarks.find(root);
        found != _repository_visible_bookmarks.end())
    {
        _visible_bookmarks = found->second;
        _visible_bookmarks_user_selected = true;
    }
    EnsureVisibleBookmarkSelection();

    _selected_tags.clear();
    if (const auto found = _repository_selected_tags.find(root);
        found != _repository_selected_tags.end())
        _selected_tags = found->second;
    EnsureTagSelection();
}

void Application::RememberRepositorySelections()
{
    if (_snapshot == nullptr || _snapshot->root.empty())
        return;
    _repository_selected_remotes[_snapshot->root] = _selected_remotes;
    _repository_visible_bookmarks[_snapshot->root] = _visible_bookmarks;
    _repository_selected_tags[_snapshot->root] = _selected_tags;
}

void Application::EnsureTagSelection()
{
    if (_snapshot == nullptr)
    {
        _selected_tags.clear();
        return;
    }
    std::erase_if(_selected_tags, [this](const std::string& name) {
        return std::ranges::none_of(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.name == name
                && (ref.kind == GG_NAMED_REF_LOCAL_TAG || ref.kind == GG_NAMED_REF_REMOTE_TAG);
        });
    });
}

void Application::EnsureVisibleBookmarkSelection()
{
    if (_snapshot == nullptr)
    {
        _visible_bookmarks.clear();
        return;
    }
    const auto exists = [this](const std::string& name) {
        return std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.name == name && IsVisibleBookmarkRef(ref);
        });
    };
    std::erase_if(_visible_bookmarks, [&](const std::string& name) { return !exists(name); });
    if (_visible_bookmarks.empty())
        _visible_bookmarks_user_selected = false;
    if (_visible_bookmarks_user_selected)
        return;

    const std::string current = CurrentCommit(*_snapshot);
    std::vector<std::string> pending{current};
    std::unordered_set<std::string> visited;
    const NamedRef* closest = nullptr;
    for (std::size_t index = 0; index < pending.size() && closest == nullptr; ++index)
    {
        if (!visited.emplace(pending[index]).second)
            continue;
        const auto bookmark = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.target == pending[index] && IsVisibleBookmarkRef(ref);
        });
        if (bookmark != _snapshot->refs.end())
            closest = &*bookmark;
        else if (const auto revision = std::ranges::find(_history_revisions, pending[index], &Revision::oid);
            revision != _history_revisions.end())
            pending.insert(pending.end(), revision->parents.begin(), revision->parents.end());
    }
    if (closest != nullptr)
    {
        _visible_bookmarks = {closest->name};
        return;
    }
    if (!_visible_bookmarks.empty())
        return;
    const auto first = std::ranges::find_if(
        _snapshot->refs, [this](const NamedRef& ref) { return IsVisibleBookmarkRef(ref); });
    if (first != _snapshot->refs.end())
        _visible_bookmarks.push_back(first->name);
}

std::string Application::VisibleBookmarksKey() const
{
    std::vector<std::string> names = _visible_bookmarks;
    std::ranges::sort(names);
    std::string result;
    for (const std::string& name : names)
    {
        result += name;
        result.push_back('\n');
    }
    names = _selected_remotes;
    std::ranges::sort(names);
    for (const std::string& name : names)
        result += "remote:" + name + '\n';
    names = _selected_tags;
    std::ranges::sort(names);
    for (const std::string& name : names)
        result += "tag:" + name + '\n';
    if (!_reveal_revision.empty())
        result += "reveal:" + _reveal_revision + '\n';
    return result;
}

void Application::RenderBookmarks()
{
    if (!ImGui::Begin("Bookmarks", &_show_bookmarks))
    {
        ImGui::End();
        return;
    }

    // Create bookmark and filter controls
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    const bool actions_locked = !_active_operation.empty();
    ImGui::BeginDisabled(actions_locked);
    if (ActionButton(ICON_MS_BOOKMARK_ADD, "Create bookmark", ImVec2(-1.0f, 0.0f)))
        OpenDialog(Dialog::Bookmark);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##bookmark filter", "Filter bookmarks", &_bookmark_filter);

    EnsureRemoteSelection();
    EnsureVisibleBookmarkSelection();

    std::vector<NamedRef> bookmark_refs;
    std::ranges::copy_if(_snapshot->refs, std::back_inserter(bookmark_refs),
        [this](const NamedRef& ref) { return IsVisibleBookmarkRef(ref); });

    // Unique bookmark names
    std::vector<std::string> names;
    for (const NamedRef& ref : bookmark_refs)
    {
        if ((ref.kind != GG_NAMED_REF_LOCAL_BOOKMARK && ref.kind != GG_NAMED_REF_REMOTE_BOOKMARK)
            || std::ranges::find(names, ref.name) != names.end())
            continue;
        names.push_back(ref.name);
    }

    // Bookmark rows. Keep the controls above fixed while long bookmark lists
    // scroll independently.
    ImGui::BeginChild("bookmark list", {}, ImGuiChildFlags_Borders);
    for (const std::string& name : names)
    {
        if (!ContainsInsensitive(name, _bookmark_filter))
            continue;
        const auto local = std::ranges::find_if(bookmark_refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == name;
        });
        const auto remote_ref = std::ranges::find_if(bookmark_refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.name == name;
        });
        const NamedRef& ref = local != bookmark_refs.end() ? *local : *remote_ref;
        const std::string remotes = RefRemotes(bookmark_refs, name, GG_NAMED_REF_REMOTE_BOOKMARK);
        ImGui::PushID(name.c_str());
        bool elided = false;
        const auto selected = std::ranges::find(_visible_bookmarks, name);
        if (BadgedSelectable(name, selected != _visible_bookmarks.end(), 36.0f,
                BookmarkBadgeColor(name, bookmark_refs), {}, &elided))
        {
            _visible_bookmarks_user_selected = true;
            if (ImGui::GetIO().KeyCtrl)
                _visible_bookmarks = {name};
            else if (selected == _visible_bookmarks.end())
                _visible_bookmarks.push_back(name);
            else if (_visible_bookmarks.size() > 1)
                _visible_bookmarks.erase(selected);
            RememberRepositorySelections();
        }
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        elided |= DrawTextWithin(ImGui::GetWindowDrawList(), ImVec2(minimum.x + 12.0f, minimum.y + 21.0f),
            maximum.x - 8.0f, remotes, kTextMuted);

        // Bookmark context menu
        if (ImGui::BeginPopupContextItem("bookmark context"))
        {
            if (ActionMenuItem(ICON_MS_VISIBILITY, "Reveal commit")) RevealRevision(ref.target);
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy name")) ImGui::SetClipboardText(name.c_str());
            ImGui::Separator();
            ImGui::BeginDisabled(actions_locked);
            const auto tracked = std::ranges::find_if(bookmark_refs, [&](const NamedRef& candidate) {
                return candidate.kind == GG_NAMED_REF_REMOTE_BOOKMARK && candidate.name == name;
            });
            const std::string remote = tracked != bookmark_refs.end() ? tracked->remote
                : _selected_remotes.empty() ? "" : _selected_remotes.front();
            const bool has_local = local != bookmark_refs.end();
            const std::string& current = CurrentCommit(*_snapshot);
            const BookmarkRelation current_relation = has_local && !current.empty()
                ? ClassifyBookmarkRelation(*_snapshot, local->target, current)
                : BookmarkRelation::Unavailable;
            if (ActionMenuItem(ICON_MS_MERGE, "Merge into @", nullptr,
                    has_local && !current.empty() && local->target != current))
                EnqueueAction(NewChange{{}, {"@", local->target}, {}, {}, false});
            if (ActionMenuItem(ICON_MS_REBASE, "Rebase @ onto bookmark", nullptr,
                    current_relation == BookmarkRelation::RemoteAhead
                        || current_relation == BookmarkRelation::Diverged))
            {
                QueueCommands({Rebase{"@", local->target, true}}, {current},
                    "Rebasing @ will rewrite locked commits.");
            }
            ImGui::Separator();
            if (ActionMenuItem(ICON_MS_CLOUD_UPLOAD, "Push", nullptr, has_local && !remote.empty()))
                EnqueueAction(Push{name, remote});
            if (ActionMenuItem(ICON_MS_PUBLISH, "Push to...", nullptr,
                    has_local && !_snapshot->remotes.empty()))
            {
                OpenDialog(Dialog::PushTo);
                _input_primary = remote;
                _input_secondary = name;
            }
            if (has_local)
            {
                for (const NamedRef& candidate : _snapshot->refs)
                {
                    if (candidate.kind != GG_NAMED_REF_REMOTE_BOOKMARK || !candidate.tracked
                        || candidate.name != name || candidate.remote.empty()
                        || ClassifyBookmarkRelation(*_snapshot, local->target, candidate.target)
                            != BookmarkRelation::Diverged)
                        continue;
                    const std::string label = "Reconcile with " + candidate.remote + "/" + name
                        + "...###reconcile-" + candidate.remote;
                    if (ActionMenuItem(ICON_MS_REBASE, label))
                    {
                        OpenDialog(Dialog::Reconcile);
                        _input_primary = name;
                        _input_secondary = candidate.remote;
                        _input_tertiary = local->target;
                        _input_filesets = candidate.target;
                        _dialog_snapshot_generation = _snapshot->generation;
                    }
                }
            }
            ImGui::Separator();
            if (ActionMenuItem(ICON_MS_EDIT, "Rename...", nullptr, has_local))
            {
                OpenDialog(Dialog::BookmarkRename);
                _input_primary = name;
                _input_secondary = name;
            }
            const std::string delete_label = IconLabel(ICON_MS_DELETE, "Delete");
            if (ImGui::BeginMenu(delete_label.c_str()))
            {
                if (ActionMenuItem(ICON_MS_BOOKMARK, "Local", nullptr, has_local))
                    EnqueueAction(Bookmark{GG_BOOKMARK_DELETE, {name}, {}, {}});
                for (const NamedRef& candidate : _snapshot->refs)
                    if (candidate.kind == GG_NAMED_REF_REMOTE_BOOKMARK && candidate.name == name
                        && !candidate.remote.empty()
                        && ActionMenuItem(ICON_MS_CLOUD, candidate.remote))
                        EnqueueAction(RemoteBookmarkDelete{name, candidate.remote});
                ImGui::EndMenu();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        // Elided bookmark details
        if (hovered && elided)
        {
            ImGui::BeginTooltip();
            ImGui::Text("Bookmark: %s", name.c_str());
            ImGui::Text("Remotes: %s", remotes.empty() ? "(local only)" : remotes.c_str());
            TextLabelledId("Commit: ", ref.target, RevisionPrefix(ref.target),
                CommitIdColor(ref.target == _snapshot->working_copy));
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

void Application::RenderTags()
{
    if (!ImGui::Begin("Tags", &_show_tags))
    {
        ImGui::End();
        return;
    }

    // Create tag and filter controls
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    const bool actions_locked = !_active_operation.empty();
    ImGui::BeginDisabled(actions_locked);
    if (ImGui::Button("Create tag", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::Tag);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##tag filter", "Filter tags", &_tag_filter);

    EnsureTagSelection();

    // Unique tag names
    std::vector<std::string> names;
    for (const NamedRef& ref : _snapshot->refs)
    {
        if ((ref.kind != GG_NAMED_REF_LOCAL_TAG && ref.kind != GG_NAMED_REF_REMOTE_TAG)
            || std::ranges::find(names, ref.name) != names.end())
            continue;
        names.push_back(ref.name);
    }

    // Tag rows
    for (const std::string& name : names)
    {
        if (!ContainsInsensitive(name, _tag_filter))
            continue;
        const auto local = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_TAG && ref.name == name;
        });
        const auto remote_ref = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_REMOTE_TAG && ref.name == name;
        });
        const NamedRef& ref = local != _snapshot->refs.end() ? *local : *remote_ref;
        const std::string remotes = RefRemotes(_snapshot->refs, name, GG_NAMED_REF_REMOTE_TAG);
        ImGui::PushID(name.c_str());
        bool elided = false;
        const auto selected = std::ranges::find(_selected_tags, name);
        if (BadgedSelectable(name, selected != _selected_tags.end(), 36.0f,
                RefBadgeColor(ref, _snapshot->refs), {}, &elided))
        {
            bool reveal = false;
            if (ImGui::GetIO().KeyCtrl)
            {
                _selected_tags = {name};
                reveal = true;
            }
            else if (selected == _selected_tags.end())
            {
                _selected_tags.push_back(name);
                reveal = true;
            }
            else
                _selected_tags.erase(selected);
            RememberRepositorySelections();
            if (reveal) RevealRevision(ref.target);
        }
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        if (!elided)
        {
            const float id_x = minimum.x + 20.0f + ImGui::CalcTextSize(name.c_str()).x;
            elided = DrawHighlightedIdWithin(ImGui::GetWindowDrawList(), ImVec2(id_x, minimum.y + 3.0f),
                maximum.x - 8.0f, ref.target, RevisionPrefix(ref.target),
                CommitIdColor(ref.target == _snapshot->working_copy));
        }
        elided |= DrawTextWithin(ImGui::GetWindowDrawList(), ImVec2(minimum.x + 12.0f, minimum.y + 21.0f),
            maximum.x - 8.0f, remotes, kTextMuted);

        // Tag context menu
        if (ImGui::BeginPopupContextItem("tag context"))
        {
            if (ActionMenuItem(ICON_MS_VISIBILITY, "Reveal commit")) RevealRevision(ref.target);
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy name")) ImGui::SetClipboardText(name.c_str());
            const std::string copy_label = IconLabel(ICON_MS_CONTENT_COPY, "Copy");
            if (ImGui::BeginMenu(copy_label.c_str()))
            {
                IdCopyMenuItems("commit ID", ref.target, RevisionPrefix(ref.target));
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::BeginDisabled(actions_locked);
            if (ActionMenuItem(ICON_MS_DELETE, "Delete", nullptr, local != _snapshot->refs.end()))
                EnqueueAction(Tag{GG_TAG_DELETE, {name}, {}, false});
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        // Elided tag details
        if (hovered && elided)
        {
            ImGui::BeginTooltip();
            ImGui::Text("Tag: %s", name.c_str());
            ImGui::Text("Remotes: %s", remotes.empty() ? "(local only)" : remotes.c_str());
            TextLabelledId("Commit: ", ref.target, RevisionPrefix(ref.target),
                CommitIdColor(ref.target == _snapshot->working_copy));
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::End();
}

void Application::RenderWorkspaces()
{
    if (!ImGui::Begin("Workspaces", &_show_workspaces))
    {
        ImGui::End();
        return;
    }

    // Add workspace action
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    const bool actions_locked = !_active_operation.empty();
    ImGui::BeginDisabled(actions_locked);
    if (ImGui::Button("Add workspace", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::WorkspaceAdd);
    ImGui::EndDisabled();

    // Workspace rows
    for (const Workspace& workspace : _snapshot->workspaces)
    {
        ImGui::PushID(&workspace);
        const ImU32 accent = workspace.stale ? kStatusDeleted : kBadgeWorkingCopy;
        bool elided = false;
        if (BadgedSelectable(workspace.name,
                !workspace.working_copy.empty() && workspace.working_copy == _selected_revision,
                40.0f, accent, {}, &elided)
            && !workspace.working_copy.empty())
            SelectRevision(workspace.working_copy);
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        const std::string_view location = workspace.stale ? std::string_view("Unavailable")
                                                          : std::string_view(workspace.root);
        elided |= DrawTextWithin(ImGui::GetWindowDrawList(), ImVec2(minimum.x + 12.0f, minimum.y + 23.0f),
            maximum.x - 8.0f, location, kTextMuted);

        // Workspace context menu
        if (ImGui::BeginPopupContextItem("workspace context"))
        {
            if (ActionMenuItem(ICON_MS_FOLDER, "Open directory", nullptr, !workspace.stale))
                OpenExternalPath(workspace.root, "Workspace directory"); // GCOV_EXCL_LINE: external application handoff
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy name"))
                ImGui::SetClipboardText(workspace.name.c_str());
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy path"))
                ImGui::SetClipboardText(workspace.root.c_str());
            ImGui::BeginDisabled(actions_locked);
            if (ActionMenuItem(ICON_MS_DELETE, "Forget", nullptr, workspace.managed))
                EnqueueAction(WorkspaceForget{{workspace.name}});
            if (ActionMenuItem(ICON_MS_EDIT, "Rename current...", nullptr, workspace.managed))
                OpenDialog(Dialog::WorkspaceRename);
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        // Elided workspace details
        if (hovered && elided)
        {
            ImGui::BeginTooltip();
            ImGui::Text("Workspace: %s", workspace.name.c_str());
            ImGui::Text("Directory: %.*s", static_cast<int>(location.size()), location.data());
            if (workspace.working_copy.empty())
                ImGui::TextDisabled("No commit checked out");
            else
                TextLabelledId(workspace.managed ? "Working copy: " : "Git HEAD: ",
                    workspace.working_copy, RevisionPrefix(workspace.working_copy),
                    CommitIdColor(workspace.working_copy == _snapshot->working_copy));
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::End();
}

void Application::RenderRemotes()
{
    if (!ImGui::Begin("Remotes", &_show_remotes))
    {
        ImGui::End();
        return;
    }

    // Add remote action
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    const bool actions_locked = !_active_operation.empty();
    ImGui::BeginDisabled(actions_locked);
    if (ActionButton(ICON_MS_ADD, "Add remote", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::RemoteAdd);
    ImGui::EndDisabled();

    EnsureRemoteSelection();

    // Remote rows
    ImGui::BeginChild("remote list", {}, ImGuiChildFlags_Borders);
    for (const Remote& remote : _snapshot->remotes)
    {
        ImGui::PushID(&remote);
        const bool separate_push = !remote.push_url.empty() && remote.push_url != remote.fetch_url;
        bool elided = false;
        const auto selected = std::ranges::find(_selected_remotes, remote.name);
        if (BadgedSelectable(remote.name, selected != _selected_remotes.end(),
                separate_push ? 56.0f : 40.0f, kBadgeRemote, {}, &elided))
        {
            _selected_remotes_user_selected = true;
            if (ImGui::GetIO().KeyCtrl)
                _selected_remotes = {remote.name};
            else if (selected == _selected_remotes.end())
                _selected_remotes.push_back(remote.name);
            else if (_selected_remotes.size() > 1)
                _selected_remotes.erase(selected);
            EnsureVisibleBookmarkSelection();
            RememberRepositorySelections();
        }
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        elided |= DrawTextWithin(draw, ImVec2(minimum.x + 12.0f, minimum.y + 23.0f), maximum.x - 8.0f,
            remote.fetch_url, kTextMuted);
        const std::string push_url = "Push: " + remote.push_url;
        if (separate_push)
            elided |= DrawTextWithin(draw, ImVec2(minimum.x + 12.0f, minimum.y + 39.0f), maximum.x - 8.0f,
                push_url, kTextMuted);

        // Remote context menu
        if (ImGui::BeginPopupContextItem("remote context"))
        {
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy name")) ImGui::SetClipboardText(remote.name.c_str());
            ImGui::BeginDisabled(actions_locked);
            if (ActionMenuItem(ICON_MS_CLOUD_DOWNLOAD, "Pull")) EnqueueAction(Fetch{remote.name, true});
            if (ActionMenuItem(ICON_MS_SYNC, "Fetch")) EnqueueAction(Fetch{remote.name, false});
            ImGui::Separator();
            if (ActionMenuItem(ICON_MS_DELETE, "Delete remote")) EnqueueAction(DeleteRemote{remote.name});
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        // Elided remote details
        if (hovered && elided)
        {
            ImGui::BeginTooltip();
            ImGui::Text("Remote: %s", remote.name.c_str());
            ImGui::Text("Fetch: %s", remote.fetch_url.c_str());
            ImGui::Text("Push: %s", remote.push_url.empty() ? remote.fetch_url.c_str() : remote.push_url.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

} // namespace Ggui

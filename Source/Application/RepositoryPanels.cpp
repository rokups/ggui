// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>
#include <imgui_stdlib.h>

#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

void Application::RenderBookmarks()
{
    if (!ImGui::Begin("Bookmarks", &_show_bookmarks))
    {
        ImGui::End();
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    const bool actions_locked = !_active_operation.empty();
    ImGui::BeginDisabled(actions_locked);
    if (ActionButton(ICON_MS_BOOKMARK_ADD, "Create bookmark", ImVec2(-1.0f, 0.0f)))
        OpenDialog(Dialog::Bookmark);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##bookmark filter", "Filter bookmarks", &_bookmark_filter);
    std::vector<std::string> names;
    for (const NamedRef& ref : _snapshot->refs)
    {
        if ((ref.kind != GG_NAMED_REF_LOCAL_BOOKMARK && ref.kind != GG_NAMED_REF_REMOTE_BOOKMARK)
            || std::ranges::find(names, ref.name) != names.end())
            continue;
        names.push_back(ref.name);
    }
    for (const std::string& name : names)
    {
        if (!ContainsInsensitive(name, _bookmark_filter))
            continue;
        const auto local = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.name == name;
        });
        const auto remote_ref = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& ref) {
            return ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK && ref.name == name;
        });
        const NamedRef& ref = local != _snapshot->refs.end() ? *local : *remote_ref;
        const std::string remotes = RefRemotes(_snapshot->refs, name, GG_NAMED_REF_REMOTE_BOOKMARK);
        ImGui::PushID(name.c_str());
        bool elided = false;
        if (BadgedSelectable(name, ref.target == _selected_revision, 36.0f,
                BookmarkBadgeColor(name, _snapshot->refs), {}, &elided))
            SelectRevision(ref.target);
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        elided |= DrawTextWithin(ImGui::GetWindowDrawList(), ImVec2(minimum.x + 12.0f, minimum.y + 21.0f),
            maximum.x - 8.0f, remotes, kTextMuted);
        if (ImGui::BeginPopupContextItem("bookmark context"))
        {
            if (ActionMenuItem(ICON_MS_VISIBILITY, "Reveal commit")) RevealRevision(ref.target);
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy name")) ImGui::SetClipboardText(name.c_str());
            ImGui::Separator();
            ImGui::BeginDisabled(actions_locked);
            const auto tracked = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& candidate) {
                return candidate.kind == GG_NAMED_REF_REMOTE_BOOKMARK && candidate.name == name;
            });
            const std::string remote = tracked != _snapshot->refs.end() ? tracked->remote
                : std::ranges::any_of(_snapshot->remotes, [](const Remote& candidate) { return candidate.name == "origin"; })
                ? "origin"
                : _snapshot->remotes.empty() ? "" : _snapshot->remotes.front().name;
            const bool has_local = local != _snapshot->refs.end();
            if (ActionMenuItem(ICON_MS_CLOUD_UPLOAD, "Push", nullptr, has_local && !remote.empty()))
                _engine.Enqueue(Push{name, remote});
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
                    _engine.Enqueue(Bookmark{GG_BOOKMARK_DELETE, {name}, {}, {}});
                for (const NamedRef& candidate : _snapshot->refs)
                    if (candidate.kind == GG_NAMED_REF_REMOTE_BOOKMARK && candidate.name == name
                        && !candidate.remote.empty()
                        && ActionMenuItem(ICON_MS_CLOUD, candidate.remote))
                        _engine.Enqueue(RemoteBookmarkDelete{name, candidate.remote});
                ImGui::EndMenu();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        if (hovered && elided)
        {
            ImGui::BeginTooltip();
            ImGui::Text("Bookmark: %s", name.c_str());
            ImGui::Text("Remotes: %s", remotes.empty() ? "(local only)" : remotes.c_str());
            ImGui::Text("Commit: %s", ref.target.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    const bool actions_locked = !_active_operation.empty();
    ImGui::BeginDisabled(actions_locked);
    if (ImGui::Button("Create tag", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::Tag);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##tag filter", "Filter tags", &_tag_filter);
    std::vector<std::string> names;
    for (const NamedRef& ref : _snapshot->refs)
    {
        if ((ref.kind != GG_NAMED_REF_LOCAL_TAG && ref.kind != GG_NAMED_REF_REMOTE_TAG)
            || std::ranges::find(names, ref.name) != names.end())
            continue;
        names.push_back(ref.name);
    }
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
        if (BadgedSelectable(name, ref.target == _selected_revision, 36.0f,
                RefBadgeColor(ref, _snapshot->refs), {}, &elided))
            SelectRevision(ref.target);
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
                _engine.Enqueue(Tag{GG_TAG_DELETE, {name}, {}, false});
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        if (hovered && elided)
        {
            ImGui::BeginTooltip();
            ImGui::Text("Tag: %s", name.c_str());
            ImGui::Text("Remotes: %s", remotes.empty() ? "(local only)" : remotes.c_str());
            ImGui::Text("Commit: %s", ref.target.c_str());
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    const bool actions_locked = !_active_operation.empty();
    ImGui::BeginDisabled(actions_locked);
    if (ImGui::Button("Add workspace", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::WorkspaceAdd);
    ImGui::EndDisabled();
    for (const Workspace& workspace : _snapshot->workspaces)
    {
        ImGui::PushID(&workspace);
        const ImU32 accent = workspace.stale ? kStatusDeleted : kBadgeWorkingCopy;
        bool elided = false;
        if (BadgedSelectable(workspace.name, workspace.working_copy == _selected_revision, 40.0f,
                accent, {}, &elided))
            SelectRevision(workspace.working_copy);
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        const std::string_view location = workspace.stale ? std::string_view("Unavailable")
                                                          : std::string_view(workspace.root);
        elided |= DrawTextWithin(ImGui::GetWindowDrawList(), ImVec2(minimum.x + 12.0f, minimum.y + 23.0f),
            maximum.x - 8.0f, location, kTextMuted);
        if (ImGui::BeginPopupContextItem("workspace context"))
        {
            if (ActionMenuItem(ICON_MS_FOLDER, "Open directory", nullptr, !workspace.stale))
                OpenExternalPath(workspace.root, "Workspace directory"); // GCOV_EXCL_LINE: external application handoff
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy name"))
                ImGui::SetClipboardText(workspace.name.c_str());
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy path"))
                ImGui::SetClipboardText(workspace.root.c_str());
            ImGui::BeginDisabled(actions_locked);
            if (ActionMenuItem(ICON_MS_DELETE, "Forget")) _engine.Enqueue(WorkspaceForget{{workspace.name}});
            if (ActionMenuItem(ICON_MS_EDIT, "Rename current...")) OpenDialog(Dialog::WorkspaceRename);
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        if (hovered && elided)
        {
            ImGui::BeginTooltip();
            ImGui::Text("Workspace: %s", workspace.name.c_str());
            ImGui::Text("Directory: %.*s", static_cast<int>(location.size()), location.data());
            ImGui::Text("Working copy: %s", workspace.working_copy.c_str());
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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
    const bool actions_locked = !_active_operation.empty();
    ImGui::BeginDisabled(actions_locked);
    if (ActionButton(ICON_MS_ADD, "Add remote", ImVec2(-1.0f, 0.0f))) OpenDialog(Dialog::RemoteAdd);
    ImGui::EndDisabled();
    for (const Remote& remote : _snapshot->remotes)
    {
        ImGui::PushID(&remote);
        const bool separate_push = !remote.push_url.empty() && remote.push_url != remote.fetch_url;
        bool elided = false;
        BadgedSelectable(remote.name, false, separate_push ? 56.0f : 40.0f, kBadgeRemote, {}, &elided);
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
        if (ImGui::BeginPopupContextItem("remote context"))
        {
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy name")) ImGui::SetClipboardText(remote.name.c_str());
            ImGui::BeginDisabled(actions_locked);
            if (ActionMenuItem(ICON_MS_CLOUD_DOWNLOAD, "Pull")) _engine.Enqueue(Fetch{remote.name, true});
            if (ActionMenuItem(ICON_MS_SYNC, "Fetch")) _engine.Enqueue(Fetch{remote.name, false});
            ImGui::Separator();
            if (ActionMenuItem(ICON_MS_DELETE, "Delete remote")) _engine.Enqueue(DeleteRemote{remote.name});
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
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
    ImGui::PopStyleVar();
    ImGui::End();
}

} // namespace Ggui

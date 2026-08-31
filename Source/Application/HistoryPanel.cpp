// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

void Application::UpdateGraphBuild()
{
    if (_snapshot == nullptr) return;
    EnsureRemoteSelection();
    EnsureVisibleBookmarkSelection();
    EnsureTagSelection();
    const std::string key = VisibleBookmarksKey();
    const auto now = std::chrono::steady_clock::now();
    if (_history_observed_filter != _graph_filter)
    {
        _history_observed_filter = _graph_filter;
        _history_filter_changed = now;
    }
    const bool filter_ready = _graph_filter.empty()
        || now - _history_filter_changed >= std::chrono::milliseconds(200);
    if (!filter_ready) return;
    if (_history_requested_generation == _snapshot->repository_generation
        && _history_requested_key == key && _history_requested_filter == _graph_filter)
        return;
    HistoryQuery query;
    query.bookmarks = _visible_bookmarks;
    query.tags = _selected_tags;
    query.remotes = _selected_remotes;
    query.search = _graph_filter;
    query.repository_generation = _snapshot->repository_generation;
    _engine.Enqueue(RebuildHistory{std::move(query)});
    _history_requested_generation = _snapshot->repository_generation;
    _history_requested_key = key;
    _history_requested_filter = _graph_filter;
}

void Application::RebuildIdPrefixes()
{
    const auto build = [](const std::vector<std::string>& values, auto& destination) {
        destination.clear();
        const std::vector<std::size_t> lengths = UniquePrefixLengths(values);
        for (std::size_t index = 0; index < values.size(); ++index)
            destination.emplace(values[index], lengths[index]);
    };
    std::vector<std::string> revision_ids;
    for (const Revision& revision : _history_revisions)
    {
        revision_ids.push_back(revision.oid);
        revision_ids.insert(revision_ids.end(), revision.aliases.begin(), revision.aliases.end());
    }
    std::vector<std::string> operation_ids;
    for (const Operation& operation : _snapshot->operations) operation_ids.push_back(operation.oid);
    build(revision_ids, _revision_prefixes);
    build(operation_ids, _operation_prefixes);
}

bool Application::RevealRevisionLoaded() const
{
    return !_reveal_revision.empty() && std::ranges::any_of(_history_revisions, [this](const Revision& revision) {
        return revision.oid == _reveal_revision
            || std::ranges::find(revision.aliases, _reveal_revision) != revision.aliases.end();
    });
}

bool Application::HistoryTargetConnected(std::string_view target) const
{
    return _history_view != nullptr && std::ranges::any_of(_history_view->items, [&](const HistoryItem& item) {
        return item.kind == HistoryItemKind::Commit && (item.revision.oid == target
            || std::ranges::find(item.revision.aliases, target) != item.revision.aliases.end());
    });
}

void Application::ExpandGraphRow(std::size_t visible_row, bool)
{
    if (_history_view == nullptr || visible_row >= _history_view->items.size()) return;
    const HistoryItem& item = _history_view->items[visible_row];
    if (item.kind == HistoryItemKind::CollapsedRegion)
        _engine.Enqueue(ExpandHistoryRegion{item.id});
}

std::size_t Application::RevisionPrefix(const std::string& oid) const
{
    const auto found = _revision_prefixes.find(oid);
    return found == _revision_prefixes.end() ? std::min<std::size_t>(1, oid.size()) : found->second;
}

std::size_t Application::OperationPrefix(const std::string& oid) const
{
    const auto found = _operation_prefixes.find(oid);
    return found == _operation_prefixes.end() ? std::min<std::size_t>(1, oid.size()) : found->second;
}

void Application::RenderHistory()
{
    if (!_reveal_revision.empty()) ImGui::SetNextWindowFocus();
    if (!ImGui::Begin("History", &_show_history)) { ImGui::End(); return; }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##graph filter", "Search changes, IDs, bookmarks, tags", &_graph_filter);
    UpdateGraphBuild();
    ImGui::BeginChild("graph scroll", {}, ImGuiChildFlags_Borders);

    const bool usable = _history_view != nullptr
        && _history_view->items.size() == _graph_rows.size()
        && _history_view->items.size() == _visible_revisions.size();
    if (!_reveal_revision.empty() && usable)
    {
        const auto found = std::ranges::find_if(_history_view->items, [this](const HistoryItem& item) {
            return item.kind == HistoryItemKind::Commit && (item.revision.oid == _reveal_revision
                || std::ranges::find(item.revision.aliases, _reveal_revision) != item.revision.aliases.end());
        });
        if (found != _history_view->items.end())
        {
            const float row = static_cast<float>(found - _history_view->items.begin()) * kRowHeight;
            ImGui::SetScrollY(std::max(0.0f, row - ImGui::GetContentRegionAvail().y * 0.5f));
            SelectRevision(found->revision.oid);
            _reveal_revision.clear();
        }
    }

    const ImGuiIO& io = ImGui::GetIO();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (usable && focused && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift && !io.KeySuper
        && (ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat)
            || ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat)))
    {
        const int direction = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : -1;
        int current = -1;
        for (int index = 0; index < static_cast<int>(_history_view->items.size()); ++index)
            if (_history_view->items[static_cast<std::size_t>(index)].kind == HistoryItemKind::Commit
                && _history_view->items[static_cast<std::size_t>(index)].revision.oid == _selected_revision)
                current = index;
        for (int index = current + direction; index >= 0
            && index < static_cast<int>(_history_view->items.size()); index += direction)
            if (_history_view->items[static_cast<std::size_t>(index)].kind == HistoryItemKind::Commit)
            {
                SelectRevision(_history_view->items[static_cast<std::size_t>(index)].revision.oid);
                ImGui::SetScrollY(std::max(0.0f, index * kRowHeight - ImGui::GetContentRegionAvail().y * 0.5f));
                break;
            }
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    int graph_columns = 1;
    for (const GraphRow& row : _graph_rows) graph_columns = std::max(graph_columns, GraphColumnCount(row));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    // The stock ImGui target frame obscures both the graph and our action-specific
    // feedback. History renders a precise insertion line or commit-content border instead.
    ImGui::PushStyleColor(ImGuiCol_DragDropTarget, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_DragDropTargetBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    std::optional<std::pair<ImVec2, ImVec2>> reorder_marker;
    ImGuiListClipper clipper;
    clipper.Begin(usable ? static_cast<int>(_history_view->items.size()) : 0, kRowHeight);
    _rendered_history_rows = 0;
    while (clipper.Step())
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
        {
            ++_rendered_history_rows;
            const HistoryItem& item = _history_view->items[static_cast<std::size_t>(index)];
            const GraphRow& row = _graph_rows[static_cast<std::size_t>(index)];
            ImGui::PushID(item.id.c_str());
            const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            const bool region = item.kind == HistoryItemKind::CollapsedRegion;
            if (region)
                ImGui::Dummy(ImVec2(width, kRowHeight));
            else
                ImGui::InvisibleButton("row", ImVec2(width, kRowHeight),
                    ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const float center = (minimum.y + maximum.y) * 0.5f;
            const float lane_width = HistoryLaneWidth(width, graph_columns);
            const bool hovered = region ? ImGui::IsMouseHoveringRect(minimum, maximum)
                                        : ImGui::IsItemHovered();
            const bool clicked = !region && ImGui::IsItemClicked();
            const bool region_clicked = region && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            const bool selected = !region
                && std::ranges::find(_selected_revisions, item.revision.oid) != _selected_revisions.end();
            const ImU32 background = selected ? kRowSelected : hovered ? kRowHover : kRowBackground;
            draw->AddRectFilled(minimum, maximum, background, 5.0f);
            if (item.search_match) draw->AddRect(minimum, maximum, IM_COL32(220, 170, 70, 255), 5.0f, 0, 2.0f);

            const float graph_left = minimum.x + kGraphPadding;
            const auto lane_x = [&](int column) { return graph_left + column * lane_width + lane_width * 0.5f; };
            const auto color = [](int track) { return kLaneColors[static_cast<std::size_t>(track) % kLaneColors.size()]; };
            const float dot_x = lane_x(row.column);
            // Route pass-by lanes by their stable track identity instead of
            // joining whatever happens to occupy the same column above and
            // below this row. Curves make column compaction readable without
            // implying a relationship to the commit bullet.
            for (std::size_t before = 0; before < row.tracks_before.size(); ++before)
            {
                const int track = row.tracks_before[before];
                if (track == row.track)
                {
                    draw->AddBezierQuadratic({lane_x(static_cast<int>(before)), minimum.y},
                        {lane_x(static_cast<int>(before)), center}, {dot_x, center}, color(track), 2.0f);
                    continue;
                }
                auto after = std::ranges::find(row.tracks_after, track);
                if (after != row.tracks_after.end())
                {
                    const int after_column = static_cast<int>(after - row.tracks_after.begin());
                    draw->AddBezierCubic({lane_x(static_cast<int>(before)), minimum.y},
                        {lane_x(static_cast<int>(before)), center}, {lane_x(after_column), center},
                        {lane_x(after_column), maximum.y}, color(track), 2.0f);
                }
            }
            for (std::size_t parent_index = 0; parent_index < row.parent_columns.size(); ++parent_index)
            {
                const int parent = row.parent_columns[parent_index];
                draw->AddBezierQuadratic({dot_x, center}, {lane_x(parent), center},
                    {lane_x(parent), maximum.y}, color(row.parent_tracks[parent_index]), 2.0f);
            }

            const float content_x = minimum.x
                + HistoryContentOffset(lane_width, GraphColumnCount(row));
            if (region)
            {
                draw->AddCircleFilled({dot_x, center}, std::min(3.0f, lane_width * 0.3f), kTextMuted);
                const std::string label = _history_expansion_pending == item.id ? "Loading..." : "...";
                const ImVec2 after_row = ImGui::GetCursorScreenPos();
                const ImVec2 label_size = ImGui::CalcTextSize(label.c_str());
                const ImVec2 label_position{content_x, center - label_size.y * 0.5f};
                ImGui::SetCursorScreenPos(label_position);
                const bool expand = ImGui::InvisibleButton(label.c_str(), label_size);
                const bool show_more_hovered = ImGui::IsItemHovered();
#ifdef IMGUI_ENABLE_TEST_ENGINE
                const ImGuiID show_more_id = ImGui::GetItemID();
#endif
                if (show_more_hovered)
                {
                    ImGui::BeginTooltip(); ImGui::TextUnformatted("Show more"); ImGui::EndTooltip();
#ifdef IMGUI_ENABLE_TEST_ENGINE
                    ImGuiContext& g = *GImGui;
                    ImGuiTestEngineHook_ItemInfo(
                        &g, show_more_id, "Show more", g.LastItemData.StatusFlags);
#endif
                }
                draw->AddText(label_position,
                    show_more_hovered ? IM_COL32(90, 150, 255, 255) : kTextMuted, label.c_str());
                ImGui::SetCursorScreenPos(after_row);
                if ((expand || region_clicked) && _history_expansion_pending.empty())
                {
                    int anchor = index - 1;
                    while (anchor >= 0
                        && _history_view->items[static_cast<std::size_t>(anchor)].kind != HistoryItemKind::Commit)
                        --anchor;
                    if (anchor < 0)
                    {
                        anchor = index + 1;
                        while (anchor < static_cast<int>(_history_view->items.size())
                            && _history_view->items[static_cast<std::size_t>(anchor)].kind
                                != HistoryItemKind::Commit)
                            ++anchor;
                    }
                    if (anchor >= 0 && anchor < static_cast<int>(_history_view->items.size()))
                    {
                        _history_anchor = _history_view->items[static_cast<std::size_t>(anchor)].revision.oid;
                        _history_anchor_offset = anchor * kRowHeight - ImGui::GetScrollY();
                    }
                    _history_expansion_pending = item.id;
                    _engine.Enqueue(ExpandHistoryRegion{item.id});
                }
                ImGui::PopID();
                continue;
            }

            const Revision& revision = item.revision;
            if (clicked) SelectRevision(revision.oid, io.KeyCtrl);
            draw->AddCircleFilled({dot_x, center}, std::min(kDotRadius, lane_width * 0.35f),
                revision.conflicted ? kStatusConflict : revision.working_copy ? kWorkingCommitId
                    : revision.pushed ? kStatusPushed : kStatusUnpushed);
            const std::string title = FirstLine(revision.description);
            ImVec2 cursor{content_x, center - ImGui::GetTextLineHeight() * 0.5f};
            std::vector<std::pair<std::string, ImU32>> badges;
            if (const auto refs = _history_refs_by_revision.find(revision.oid);
                refs != _history_refs_by_revision.end())
                for (const std::size_t ref_index : refs->second)
                {
                    const NamedRef& ref = _snapshot->refs[ref_index];
                    const bool remote = ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK
                        || ref.kind == GG_NAMED_REF_REMOTE_TAG;
                    const ImU32 badge_color = remote ? kBadgeRemote
                        : ref.kind == GG_NAMED_REF_LOCAL_TAG ? kBadgeTag : kBadgeBookmark;
                    badges.emplace_back(ReferenceLabel(ref), badge_color);
                }
            const std::string_view shown = title.empty() ? "(no description)" : std::string_view(title);
            const float content_right = maximum.x - 12.0f;
            float badge_width = 0.0f;
            for (const auto& [label, color] : badges)
            {
                (void)color;
                badge_width += ImGui::CalcTextSize(label.c_str()).x + FontPx(20.0f);
            }
            const float message_right = badges.empty() ? content_right
                : std::max(content_x, content_right - badge_width - FontPx(6.0f));
            DrawElidedText(draw, cursor, message_right, shown, ImGui::GetColorU32(ImGuiCol_Text));
            cursor.x += std::min(ImGui::CalcTextSize(shown.data(), shown.data() + shown.size()).x,
                std::max(0.0f, message_right - cursor.x));
#ifdef IMGUI_ENABLE_TEST_ENGINE
            {
                ImGuiContext& g = *GImGui;
                const ImGuiID message_id = ImGui::GetID("commit message");
                const ImRect message_rect({content_x, minimum.y}, {cursor.x, maximum.y});
                IMGUI_TEST_ENGINE_ITEM_ADD(message_id, message_rect, nullptr);
                ImGuiTestEngineHook_ItemInfo(
                    &g, message_id, std::string(shown).c_str(), ImGuiItemStatusFlags_None);
            }
#endif
            if (!badges.empty()) cursor.x += FontPx(6.0f);
            for (const auto& [ref_label, badge_color] : badges)
            {
#ifdef IMGUI_ENABLE_TEST_ENGINE
                const ImVec2 badge_start = cursor;
#endif
                DrawBadge(draw, cursor, center, ref_label, badge_color);
#ifdef IMGUI_ENABLE_TEST_ENGINE
                {
                    ImGuiContext& g = *GImGui;
                    const std::string item_label = "ref:" + ref_label;
                    const ImGuiID badge_id = ImGui::GetID(item_label.c_str());
                    const ImRect badge_rect({badge_start.x, minimum.y}, {cursor.x, maximum.y});
                    IMGUI_TEST_ENGINE_ITEM_ADD(badge_id, badge_rect, nullptr);
                    ImGuiTestEngineHook_ItemInfo(
                        &g, badge_id, ref_label.c_str(), ImGuiItemStatusFlags_None);
                }
#endif
            }

            const bool actions_locked = !_active_operation.empty();
            if (!actions_locked && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
            {
                // Ctrl is the copy modifier during a drag, not a request to
                // toggle the dragged commit out of the current selection.
                if (io.KeyCtrl
                    && std::ranges::find(_selected_revisions, revision.oid) == _selected_revisions.end())
                    SelectRevision(revision.oid, true);
                ImGui::SetDragDropPayload("GGUI_CHANGE", revision.oid.c_str(), revision.oid.size() + 1);
                ImGui::EndDragDropSource();
            }
            std::optional<DropAction> hovered_drop;
            bool hovered_entire_branch = false;
            bool hovered_copy = false;
            if (!actions_locked && ImGui::BeginDragDropTarget())
            {
                const float ratio = (ImGui::GetMousePos().y - minimum.y) / kRowHeight;
                const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
                if (dragging != nullptr && dragging->IsDataType("GGUI_CHANGE"))
                {
                    hovered_drop = ratio < 0.2f ? DropAction::ReorderBefore
                        : ratio >= 0.8f                 ? DropAction::ReorderAfter
                        : io.KeyAlt                     ? DropAction::Rebase
                                                        : DropAction::Squash;
                    hovered_entire_branch = ratio >= 0.2f && ratio < 0.8f && io.KeyShift;
                    const bool reorder = *hovered_drop == DropAction::ReorderBefore
                        || *hovered_drop == DropAction::ReorderAfter;
                    hovered_copy = reorder && io.KeyCtrl;
                    if (reorder)
                    {
                        RenderRevisionTooltip(
                            DropTooltip(*hovered_drop, false, hovered_copy), revision.oid);
                    }
                    else
                    {
                        const std::string_view hint =
                            "No modifier: squash change | Shift: squash entire branch\n"
                            "Alt: rebase change | Alt+Shift: rebase entire branch";
                        RenderRevisionTooltip(
                            DropTooltip(*hovered_drop, hovered_entire_branch), revision.oid, hint);
                    }
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE"))
                {
                    const std::string source = static_cast<const char*>(payload->Data);
                    if (source != revision.oid && hovered_drop.has_value())
                    {
                        _pending_drop = {
                            source, revision.oid, *hovered_drop, hovered_entire_branch, hovered_copy};
                        OpenDialog(Dialog::ConfirmDrop);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (hovered_drop == DropAction::ReorderBefore || hovered_drop == DropAction::ReorderAfter)
            {
                const float marker_left = dot_x - std::min(kDotRadius, lane_width * 0.35f) - 5.0f;
                constexpr float rounding = 4.0f;
                const float marker_y = hovered_drop == DropAction::ReorderBefore ? minimum.y : maximum.y;
                draw->AddRect({marker_left, minimum.y + 1.0f},
                    {maximum.x - 1.0f, maximum.y - 1.0f}, IM_COL32(70, 120, 210, 190), rounding,
                    ImDrawFlags_None, 2.0f);
                // Draw the insertion line after all rows. A following row's
                // background otherwise covers half of a bottom-edge marker.
                reorder_marker = std::pair{ImVec2(marker_left + rounding, marker_y),
                    ImVec2(maximum.x - 1.0f - rounding, marker_y)};
            }
            else if (hovered_drop.has_value())
            {
                const float marker_left = dot_x - std::min(kDotRadius, lane_width * 0.35f) - 5.0f;
                const ImU32 marker_color = *hovered_drop == DropAction::Squash
                    ? IM_COL32(220, 170, 70, 230) : IM_COL32(90, 150, 255, 230);
                draw->AddRect({marker_left, minimum.y + 1.0f}, {maximum.x - 1.0f, maximum.y - 1.0f},
                    marker_color, 4.0f, ImDrawFlags_None, 2.0f);
            }
            if (ImGui::BeginPopupContextItem("change context"))
            {
                ImGui::BeginDisabled(actions_locked);
                if (ActionMenuItem(ICON_MS_ADD, "New", "N")) { SelectRevision(revision.oid); CreateChange(revision.oid); }
                RenderSelectedChangeActions(revision.oid, true);
                ImGui::Separator();
                if (ActionMenuItem(ICON_MS_BOOKMARK_ADD, "Create bookmark..."))
                { SelectRevision(revision.oid); OpenDialog(Dialog::Bookmark); }
                ImGui::EndDisabled();
                ImGui::Separator();
                IdCopyMenuItems("commit ID", revision.oid, RevisionPrefix(revision.oid));
                ImGui::EndPopup();
            }
            if (hovered && !hovered_drop.has_value() && ImGui::GetDragDropPayload() == nullptr)
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(shown.data(), shown.data() + shown.size());
                ImGui::Text("Author: %s", revision.author.empty() ? "(unknown)" : revision.author.c_str());
                TextLabelledId("Commit: ", revision.oid, RevisionPrefix(revision.oid), CommitIdColor(revision.working_copy));
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
    if (reorder_marker.has_value())
        draw->AddLine(reorder_marker->first, reorder_marker->second,
            IM_COL32(100, 175, 255, 255), 4.0f);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    if (_history_scroll_frames > 0 && usable)
    {
        ImGui::SetScrollY(std::max(0.0f, _history_scroll_target - _history_anchor_offset));
        if (--_history_scroll_frames == 0) { _history_scroll_target = -1.0f; _history_anchor_offset = 0.0f; }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace Ggui

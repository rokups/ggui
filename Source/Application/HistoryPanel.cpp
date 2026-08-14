// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace Ggui
{
using namespace ApplicationInternal;

void Application::RebuildGraph()
{
    _visible_revisions.clear();
    std::vector<GraphNode> nodes;
    for (int index = 0; index < static_cast<int>(_snapshot->revisions.size()); ++index)
    {
        const Revision& revision = _snapshot->revisions[index];
        bool matches = _graph_filter.empty() || ContainsInsensitive(revision.description, _graph_filter)
            || ContainsInsensitive(revision.oid, _graph_filter)
            || std::ranges::any_of(revision.aliases,
                [&](const std::string& alias) { return ContainsInsensitive(alias, _graph_filter); });
        if (!matches)
        {
            matches = std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
                return ref.target == revision.oid && ContainsInsensitive(ReferenceLabel(ref), _graph_filter);
            });
        }
        if (matches)
        {
            _visible_revisions.push_back(index);
            nodes.push_back({revision.oid, revision.parents});
        }
    }
    _graph_rows = BuildGraphLayout(nodes);
    _graph_generation = _snapshot->generation;
    _built_filter = _graph_filter;
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
    revision_ids.reserve(_snapshot->revisions.size());
    for (const Revision& revision : _snapshot->revisions)
    {
        revision_ids.push_back(revision.oid);
        revision_ids.insert(revision_ids.end(), revision.aliases.begin(), revision.aliases.end());
    }
    std::vector<std::string> operation_ids;
    operation_ids.reserve(_snapshot->operations.size());
    for (const Operation& operation : _snapshot->operations)
        operation_ids.push_back(operation.oid);
    build(revision_ids, _revision_prefixes);
    build(operation_ids, _operation_prefixes);
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
    // History window and filter
    if (!_reveal_revision.empty()) ImGui::SetNextWindowFocus();
    if (!ImGui::Begin("History", &_show_history))
    {
        ImGui::End();
        return;
    }
    const bool actions_locked = !_active_operation.empty();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##graph filter", "Filter changes, IDs, bookmarks, tags", &_graph_filter);
    if (_graph_generation != _snapshot->generation || _built_filter != _graph_filter)
        RebuildGraph();

    // Scrollable revision graph
    ImGui::BeginChild("graph scroll", {}, ImGuiChildFlags_Borders);

    // Reveal requested revision
    if (!_reveal_revision.empty())
    {
        const auto target = std::ranges::find_if(_visible_revisions, [&](int index) {
            return _snapshot->revisions[index].oid == _reveal_revision;
        });
        if (target != _visible_revisions.end())
        {
            const float row = static_cast<float>(target - _visible_revisions.begin()) * kRowHeight;
            const float viewport = ImGui::GetContentRegionAvail().y;
            ImGui::SetScrollY(std::max(0.0f, row - (viewport - kRowHeight) * 0.5f));
        }
        _reveal_revision.clear();
    }

    // Keyboard revision navigation
    const ImGuiIO& io = ImGui::GetIO();
    const bool keyboard_navigation = !io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper
        && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const ImGuiID navigation_owner = ImGui::GetID("history arrow navigation");
    if (keyboard_navigation)
    {
        ImGui::SetKeyOwner(ImGuiKey_UpArrow, navigation_owner, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_DownArrow, navigation_owner, ImGuiInputFlags_LockThisFrame);
    }
    const bool navigate_up = keyboard_navigation
        && ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, navigation_owner);
    const bool navigate_down = keyboard_navigation
        && ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, navigation_owner);
    if ((navigate_up || navigate_down) && !_visible_revisions.empty())
    {
        const int direction = navigate_down ? 1 : -1;
        const auto selected = std::ranges::find_if(_visible_revisions, [&](int index) {
            return _snapshot->revisions[index].oid == _selected_revision;
        });
        const int current = selected == _visible_revisions.end()
            ? (direction > 0 ? -1 : static_cast<int>(_visible_revisions.size()))
            : static_cast<int>(selected - _visible_revisions.begin());
        const int target = std::clamp(current + direction, 0, static_cast<int>(_visible_revisions.size()) - 1);
        if (target != current)
        {
            SelectRevision(_snapshot->revisions[_visible_revisions[static_cast<std::size_t>(target)]].oid);
            const float target_y = target * kRowHeight;
            const float viewport = ImGui::GetContentRegionAvail().y;
            if (target_y < ImGui::GetScrollY())
                ImGui::SetScrollY(target_y);
            else if (target_y + kRowHeight > ImGui::GetScrollY() + viewport)
                ImGui::SetScrollY(target_y + kRowHeight - viewport);
        }
    }

    // Visible revision rows
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(_visible_revisions.size()), kRowHeight);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    while (clipper.Step())
    {
        for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible)
        {
            const Revision& revision = _snapshot->revisions[_visible_revisions[visible]];
            const GraphRow& row = _graph_rows[visible];
            ImGui::PushID(revision.oid.c_str());
            const int row_column_count = GraphColumnCount(row);
            const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            ImGui::InvisibleButton("row", ImVec2(width, kRowHeight),
                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const float center = (minimum.y + maximum.y) * 0.5f;
            const bool selected = std::ranges::find(_selected_revisions, revision.oid) != _selected_revisions.end();
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) SelectRevision(revision.oid, ImGui::GetIO().KeyCtrl);

            // Revision drag source
            if (!actions_locked && ImGui::BeginDragDropSource(
                    ImGuiDragDropFlags_SourceAllowNullID | ImGuiDragDropFlags_SourceNoPreviewTooltip))
            {
                const bool choose_action = ImGui::GetCurrentContext()->ActiveIdMouseButton == ImGuiMouseButton_Right;
                ImGui::SetDragDropPayload(
                    choose_action ? "GGUI_CHANGE_ACTION" : "GGUI_CHANGE", revision.oid.c_str(), revision.oid.size() + 1);
                ImGui::EndDragDropSource();
            }

            // Revision and file drop target
            std::optional<DropAction> hovered_drop;
            ImVec2 drop_zone_minimum{};
            ImVec2 drop_zone_maximum{};
            ImU32 drop_outline_color = 0;
            bool hovered_action_drop = false;
            if (!actions_locked && ImGui::BeginDragDropTarget())
            {
                const float ratio = (ImGui::GetMousePos().y - minimum.y) / kRowHeight;
                const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
                if (dragging != nullptr && dragging->IsDataType("GGUI_CHANGE"))
                {
                    hovered_drop = ratio < 0.2f ? DropAction::ReorderBefore
                        : ratio < 0.8f                  ? DropAction::Squash
                                                      : DropAction::Rebase;
                    const std::string tooltip = DropTooltip(*hovered_drop, revision.oid);
                    ImGui::SetTooltip("%s", tooltip.c_str());
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE"))
                {
                    _pending_drop = {static_cast<const char*>(payload->Data), revision.oid, *hovered_drop};
                    if (_pending_drop.source != _pending_drop.target) OpenDialog(Dialog::ConfirmDrop);
                }
                hovered_action_drop = dragging != nullptr && dragging->IsDataType("GGUI_CHANGE_ACTION");
                if (hovered_action_drop)
                    ImGui::SetTooltip("Choose an action for %s", revision.oid.c_str());
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE_ACTION"))
                {
                    _pending_drop = {static_cast<const char*>(payload->Data), revision.oid, DropAction::ReorderBefore};
                    _open_drop_actions = _pending_drop.source != _pending_drop.target;
                }
                if (_compare_to.empty())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_FILE"))
                    {
                        const char* source = static_cast<const char*>(payload->Data);
                        const char* path = source + std::char_traits<char>::length(source) + 1;
                        if (source != revision.oid && path < source + payload->DataSize && *path != '\0')
                            QueueCommands({MoveFiles{source, revision.oid, {path}}}, {source, revision.oid},
                                "Moving this file will rewrite a locked source or destination commit.");
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Revision context menu
            if (!hovered_action_drop && ImGui::BeginPopupContextItem("change context"))
            {
                const std::string copy_label = IconLabel(ICON_MS_CONTENT_COPY, "Copy");
                if (ImGui::BeginMenu(copy_label.c_str()))
                {
                    IdCopyMenuItems("commit ID", revision.oid, RevisionPrefix(revision.oid));
                    for (std::size_t index = 0; index < revision.aliases.size(); ++index)
                        IdCopyMenuItems("alias " + std::to_string(index + 1), revision.aliases[index],
                            RevisionPrefix(revision.aliases[index]));
                    if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Full description", nullptr,
                            !revision.description.empty()))
                        ImGui::SetClipboardText(revision.description.c_str());
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                ImGui::BeginDisabled(actions_locked);
                if (ActionMenuItem(ICON_MS_ADD, "New", "N"))
                {
                    SelectRevision(revision.oid);
                    CreateChange(true);
                }
                ImGui::Separator();
                const NamedRef* bookmark = BookmarkAt(*_snapshot, revision.oid);
                const std::string remote = bookmark == nullptr ? "" : RemoteForBookmark(*_snapshot, bookmark->name);
                if (ActionMenuItem(ICON_MS_CLOUD_UPLOAD, "Push", nullptr,
                        bookmark != nullptr && !remote.empty()))
                    _engine.Enqueue(Push{bookmark->name, remote});
                if (ActionMenuItem(ICON_MS_PUBLISH, "Push to...", nullptr,
                        bookmark != nullptr && !_snapshot->remotes.empty()))
                {
                    OpenDialog(Dialog::PushTo);
                    _input_primary = remote;
                    _input_secondary = bookmark->name;
                }
                ImGui::Separator();
                if (ActionMenuItem(ICON_MS_BOOKMARK_ADD, "Create bookmark..."))
                {
                    SelectRevision(revision.oid);
                    OpenDialog(Dialog::Bookmark);
                }
                const bool can_move_bookmark = std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
                    return ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.target != revision.oid;
                });
                const std::string move_bookmark = IconLabel(ICON_MS_MOVE_ITEM, "Move bookmark here");
                if (ImGui::BeginMenu(move_bookmark.c_str(), can_move_bookmark))
                {
                    for (const NamedRef& ref : _snapshot->refs)
                        if (ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.target != revision.oid
                            && ActionMenuItem(ICON_MS_BOOKMARK, ref.name))
                        {
                            const BookmarkRelation relation =
                                ClassifyBookmarkRelation(*_snapshot, ref.target, revision.oid);
                            if (relation == BookmarkRelation::LocalAhead || relation == BookmarkRelation::Diverged)
                            {
                                OpenDialog(Dialog::ConfirmBookmarkMove);
                                _input_primary = ref.name;
                                _input_secondary = ref.target;
                                _input_tertiary = revision.oid;
                                _dialog_snapshot_generation = _snapshot->generation;
                            }
                            else
                                _engine.Enqueue(Bookmark{GG_BOOKMARK_MOVE, {ref.name}, revision.oid, {}});
                        }
                    ImGui::EndMenu();
                }
                const std::string delete_bookmark = IconLabel(ICON_MS_DELETE, "Delete bookmark");
                if (ImGui::BeginMenu(delete_bookmark.c_str(), bookmark != nullptr))
                {
                    for (const NamedRef& ref : _snapshot->refs)
                        if (ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK && ref.target == revision.oid
                            && ActionMenuItem(ICON_MS_DELETE, ref.name))
                            _engine.Enqueue(Bookmark{GG_BOOKMARK_DELETE, {ref.name}, {}, {}});
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                RenderSelectedChangeActions(revision.oid, true);
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }

            // Row background and drop feedback
            const ImU32 row_fill = _dark_theme
                ? selected ? kRowSelected : hovered ? kRowHover : kRowBackground
                : ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive
                                             : hovered ? ImGuiCol_HeaderHovered : ImGuiCol_WindowBg);
            draw->AddRectFilled(minimum, maximum, row_fill, 6.0f);
            draw->AddRect(minimum, maximum, _dark_theme ? kRowBorder : ImGui::GetColorU32(ImGuiCol_Border), 6.0f);
            if (selected)
                draw->AddRectFilled(minimum, ImVec2(minimum.x + 4.0f, maximum.y), kBadgeBookmark, 6.0f,
                    ImDrawFlags_RoundCornersLeft);
            if (hovered_drop.has_value())
            {
                const ImU32 zone_color = *hovered_drop == DropAction::ReorderBefore ? IM_COL32(90, 150, 255, 80)
                    : *hovered_drop == DropAction::Squash                            ? IM_COL32(220, 170, 70, 80)
                                                                                     : IM_COL32(110, 210, 145, 80);
                drop_outline_color = *hovered_drop == DropAction::ReorderBefore ? IM_COL32(90, 150, 255, 230)
                    : *hovered_drop == DropAction::Squash                        ? IM_COL32(220, 170, 70, 230)
                                                                                 : IM_COL32(110, 210, 145, 230);
                const float zone_top = *hovered_drop == DropAction::ReorderBefore ? minimum.y
                    : *hovered_drop == DropAction::Squash                          ? minimum.y + kRowHeight * 0.2f
                                                                                  : minimum.y + kRowHeight * 0.8f;
                const float zone_bottom = *hovered_drop == DropAction::ReorderBefore ? minimum.y + kRowHeight * 0.2f
                    : *hovered_drop == DropAction::Squash                             ? minimum.y + kRowHeight * 0.8f
                                                                                     : maximum.y;
                drop_zone_minimum = ImVec2(minimum.x, zone_top);
                drop_zone_maximum = ImVec2(maximum.x, zone_bottom);
                draw->AddRectFilled(drop_zone_minimum, drop_zone_maximum, zone_color);
            }
            const float graph_width = row_column_count * kLaneWidth + kGraphPadding * 2.0f;
            draw->AddRectFilled(minimum, ImVec2(minimum.x + graph_width, maximum.y),
                _dark_theme ? kGraphBackground : IM_COL32(229, 233, 239, 255), 6.0f, ImDrawFlags_RoundCornersLeft);
            if (hovered_action_drop)
                draw->AddRectFilled(minimum, maximum, IM_COL32(90, 150, 255, 80), 6.0f);

            // Graph lanes
            const float graph_left = minimum.x + kGraphPadding;
            auto lane_x = [&](int column) { return graph_left + column * kLaneWidth + kLaneWidth * 0.5f; };
            auto color = [](int track) { return kLaneColors[static_cast<std::size_t>(track) % kLaneColors.size()]; };

            std::vector<int> shifted_before(row.tracks_before.size(), -1);
            std::vector<int> shifted_after(row.tracks_after.size(), -1);
            for (std::size_t before = 0; before < row.tracks_before.size(); ++before)
            {
                const auto after = std::find(row.tracks_after.begin(), row.tracks_after.end(), row.tracks_before[before]);
                if (after != row.tracks_after.end() && static_cast<std::size_t>(after - row.tracks_after.begin()) != before)
                {
                    shifted_before[before] = static_cast<int>(after - row.tracks_after.begin());
                    shifted_after[shifted_before[before]] = static_cast<int>(before);
                }
            }
            for (std::size_t column = 0; column < row.tracks_before.size(); ++column)
                draw->AddLine(ImVec2(lane_x(static_cast<int>(column)), minimum.y),
                    ImVec2(lane_x(static_cast<int>(column)), shifted_before[column] < 0 ? center : center - 7.0f),
                    color(row.tracks_before[column]), 2.0f);
            for (std::size_t column = 0; column < row.tracks_after.size(); ++column)
            {
                const bool new_non_current_lane = static_cast<int>(column) != row.column && shifted_after[column] < 0
                    && std::ranges::find(row.tracks_before, row.tracks_after[column]) == row.tracks_before.end();
                if (new_non_current_lane)
                    continue;
                draw->AddLine(ImVec2(lane_x(static_cast<int>(column)), shifted_after[column] < 0 ? center : center + 7.0f),
                    ImVec2(lane_x(static_cast<int>(column)), maximum.y), color(row.tracks_after[column]), 2.0f);
            }
            for (std::size_t before = 0; before < shifted_before.size(); ++before)
            {
                if (shifted_before[before] < 0) continue;
                draw->AddBezierCubic(ImVec2(lane_x(static_cast<int>(before)), center - 7.0f),
                    ImVec2(lane_x(static_cast<int>(before)), center), ImVec2(lane_x(shifted_before[before]), center),
                    ImVec2(lane_x(shifted_before[before]), center + 7.0f), color(row.tracks_before[before]), 2.0f);
            }
            const float dot_x = lane_x(row.column);
            for (int parent_column : row.parent_columns)
            {
                if (parent_column == row.column || parent_column >= static_cast<int>(row.tracks_after.size())) continue;
                const float parent_x = lane_x(parent_column);
                draw->AddBezierCubic(ImVec2(dot_x, center + kDotRadius), ImVec2(dot_x, center + 12.0f),
                    ImVec2(parent_x, maximum.y - 10.0f), ImVec2(parent_x, maximum.y),
                    color(row.tracks_after[parent_column]), 2.0f);
            }
            if (row.continues_beyond_layout)
                draw->AddLine(ImVec2(dot_x, center), ImVec2(dot_x, maximum.y), color(row.track), 2.0f);
            if (selected)
                draw->AddCircle(ImVec2(dot_x, center), kDotRadius + 3.0f, IM_COL32(47, 129, 247, 150), 0, 2.0f);

            // Revision node
            draw->AddCircleFilled(ImVec2(dot_x, center), kDotRadius,
                revision.conflicted ? kStatusConflict
                    : revision.working_copy ? kStatusAdded
                    : revision.pushed       ? kStatusPushed
                                            : kStatusUnpushed);
            draw->AddCircle(ImVec2(dot_x, center), kDotRadius, IM_COL32(17, 24, 39, 255), 0, 1.25f);

            // Revision summary
            const float content_x = std::min(minimum.x + graph_width + 12.0f, maximum.x - 8.0f);
            const float content_right = maximum.x - 8.0f;
            ImGui::PushClipRect(ImVec2(content_x, minimum.y), ImVec2(content_right, maximum.y), true);
            const std::string description = FirstLine(revision.description);
            const std::string_view title = description.empty() ? "(no description)" : std::string_view(description);
            ImVec2 content_cursor(content_x, center - ImGui::GetTextLineHeight() * 0.5f);
            bool elided = false;
            const auto draw_text = [&](std::string_view text, ImU32 color, float gap) {
                content_cursor.x += gap;
                const float text_width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
                if (content_cursor.x + text_width > content_right)
                {
                    DrawElidedText(draw, content_cursor, content_right, text, color);
                    elided = true;
                    return false;
                }
                draw->AddText(content_cursor, color, text.data(), text.data() + text.size());
                content_cursor.x += text_width;
                return true;
            };
            draw_text(title, ImGui::GetColorU32(ImGuiCol_Text), 0.0f);
            const auto draw_id = [&](const std::string& id, std::size_t prefix, ImU32 color) {
                if (elided)
                    return;
                content_cursor.x += 16.0f;
                const std::size_t shown = std::min(id.size(), std::max<std::size_t>(8, prefix));
                const std::string_view visible(id.data(), shown);
                if (content_cursor.x + ImGui::CalcTextSize(visible.data(), visible.data() + visible.size()).x
                    > content_right)
                {
                    DrawElidedText(draw, content_cursor, content_right, visible, color);
                    elided = true;
                    return;
                }
                content_cursor.x = DrawHighlightedId(draw, content_cursor, id, prefix, color);
            };
            draw_id(revision.oid, RevisionPrefix(revision.oid), CommitIdColor(revision.working_copy));
            if (!elided)
                draw_text(revision.author, kTextMuted, 16.0f);
            ImVec2 badge_cursor(content_cursor.x + 12.0f, center);
            std::vector<std::string> drawn_refs;
            for (const NamedRef& ref : _snapshot->refs)
            {
                if (elided)
                    break;
                if (ref.target != revision.oid) continue;
                const bool bookmark = ref.kind == GG_NAMED_REF_LOCAL_BOOKMARK
                    || ref.kind == GG_NAMED_REF_REMOTE_BOOKMARK;
                const auto [label, dimmed_prefix] = ReferenceBadgeLabel(ref, _snapshot->refs);
                if (label.empty())
                    continue;
                const std::string key = (bookmark ? "bookmark:" : "tag:") + label;
                if (std::ranges::find(drawn_refs, key) != drawn_refs.end())
                    continue;
                drawn_refs.push_back(key);
                const float badge_width = ImGui::CalcTextSize(label.c_str()).x + FontPx(14.0f);
                if (badge_cursor.x + badge_width > content_right)
                {
                    DrawElidedBadge(draw, badge_cursor, center, content_right, label,
                        RefBadgeColor(ref, _snapshot->refs), dimmed_prefix);
                    elided = true;
                    break;
                }
                DrawBadge(draw, badge_cursor, center, label, RefBadgeColor(ref, _snapshot->refs), dimmed_prefix);
            }
            ImGui::PopClipRect();

            if (hovered && elided)
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 520.0f);
                const std::string message = LimitedLines(revision.description, 16);
                ImGui::TextUnformatted(message.empty() ? "(no description)" : message.c_str());
                ImGui::Separator();
                ImGui::Text("Author: %s", revision.author.empty() ? "(unknown)" : revision.author.c_str());
                ImGui::Text("Commit: %s", revision.oid.c_str());
                if (!revision.aliases.empty())
                {
                    std::string aliases;
                    for (const std::string& alias : revision.aliases)
                    {
                        if (!aliases.empty()) aliases += ", ";
                        aliases += alias;
                    }
                    ImGui::TextWrapped("Aliases: %s", aliases.c_str());
                }
                if (!revision.parents.empty())
                {
                    std::string parents;
                    for (const std::string& parent : revision.parents)
                    {
                        if (!parents.empty()) parents += ", ";
                        parents += parent;
                    }
                    ImGui::TextWrapped("Parents: %s", parents.c_str());
                }
                std::string refs;
                for (const NamedRef& ref : _snapshot->refs)
                {
                    if (ref.target != revision.oid) continue;
                    if (!refs.empty()) refs += ", ";
                    refs += ref.remote.empty() ? ref.name : ref.remote + "/" + ref.name;
                }
                if (!refs.empty()) ImGui::TextWrapped("Refs: %s", refs.c_str());
                ImGui::TextUnformatted(revision.pushed ? "Locked (pushed)" : "Not pushed");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            if (hovered_drop.has_value())
                draw->AddRect(drop_zone_minimum + ImVec2(1.0f, 1.0f), drop_zone_maximum - ImVec2(1.0f, 1.0f),
                    drop_outline_color, 3.0f, ImDrawFlags_None, 2.0f);
            else if (hovered_action_drop)
                draw->AddRect(ImVec2(minimum.x + 1.0f, minimum.y + 1.0f),
                    ImVec2(maximum.x - 1.0f, maximum.y - 1.0f), IM_COL32(90, 150, 255, 230), 5.0f,
                    ImDrawFlags_None, 2.0f);
            ImGui::SetCursorScreenPos(ImVec2(minimum.x, maximum.y));
            ImGui::PopID();
        }
    }

    // Drop zone after the final revision
    ImGui::InvisibleButton("move to end", ImVec2(-1.0f, 22.0f));
    const ImVec2 end_minimum = ImGui::GetItemRectMin();
    const ImVec2 end_maximum = ImGui::GetItemRectMax();
    bool hovered_end_drop = false;
    if (!actions_locked && !_visible_revisions.empty() && ImGui::BeginDragDropTarget())
    {
        ImGui::TextUnformatted("Move after final change");
        const Revision& final = _snapshot->revisions[_visible_revisions.back()];
        const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
        hovered_end_drop = dragging != nullptr
            && (dragging->IsDataType("GGUI_CHANGE") || dragging->IsDataType("GGUI_CHANGE_ACTION"));
        if (dragging != nullptr && dragging->IsDataType("GGUI_CHANGE"))
        {
            const std::string tooltip = DropTooltip(DropAction::ReorderAfter, final.oid);
            ImGui::SetTooltip("%s", tooltip.c_str());
        }
        else if (dragging != nullptr && dragging->IsDataType("GGUI_CHANGE_ACTION"))
            ImGui::SetTooltip("Choose an action after %s", final.oid.c_str());
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE"))
        {
            _pending_drop = {static_cast<const char*>(payload->Data), final.oid, DropAction::ReorderAfter};
            if (_pending_drop.source != _pending_drop.target) OpenDialog(Dialog::ConfirmDrop);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE_ACTION"))
        {
            _pending_drop = {static_cast<const char*>(payload->Data), final.oid, DropAction::ReorderAfter};
            _open_drop_actions = _pending_drop.source != _pending_drop.target;
        }
        ImGui::EndDragDropTarget();
    }
    if (hovered_end_drop)
        draw->AddRect(ImVec2(end_minimum.x + 1.0f, end_minimum.y + 1.0f),
            ImVec2(end_maximum.x - 1.0f, end_maximum.y - 1.0f), IM_COL32(90, 150, 255, 230), 5.0f,
            ImDrawFlags_None, 2.0f);
    ImGui::EndChild();

    // Explicit drag-and-drop action picker
    if (_open_drop_actions)
    {
        ImGui::OpenPopup("Drop action");
        _open_drop_actions = false;
    }
    if (ImGui::BeginPopup("Drop action"))
    {
        ImGui::BeginDisabled(actions_locked);
        std::optional<DropAction> action;
        if (ActionMenuItem(ICON_MS_ARROW_DOWNWARD, "Move before")) action = DropAction::ReorderBefore;
        if (ActionMenuItem(ICON_MS_ARROW_UPWARD, "Move after")) action = DropAction::ReorderAfter;
        if (ActionMenuItem(ICON_MS_MERGE, "Squash")) action = DropAction::Squash;
        if (ActionMenuItem(ICON_MS_REBASE, "Rebase")) action = DropAction::Rebase;
        if (action.has_value())
        {
            _pending_drop.action = *action;
            OpenDialog(Dialog::ConfirmDrop);
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace Ggui

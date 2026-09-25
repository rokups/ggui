// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

namespace
{

struct FileDropPayload
{
    std::string source;
    std::vector<std::string> paths;
};

std::optional<std::string> ParseChangeDropPayload(const ImGuiPayload& payload)
{
    if (payload.Data == nullptr || payload.DataSize < 2)
        return std::nullopt;
    const char* begin = static_cast<const char*>(payload.Data);
    const char* end = begin + payload.DataSize;
    const char* terminator = std::find(begin, end, '\0');
    if (terminator == end || terminator == begin)
        return std::nullopt;
    return std::string(begin, terminator);
}

// ChangesPanel emits NUL-terminated strings: the source change, then one or
// more paths. Validate every boundary before constructing strings so
// malformed external payloads cannot turn into an arbitrary fileset or an
// out-of-bounds read.
std::optional<FileDropPayload> ParseFileDropPayload(const ImGuiPayload& payload)
{
    if (payload.Data == nullptr || payload.DataSize < 2)
        return std::nullopt;
    const char* begin = static_cast<const char*>(payload.Data);
    const char* end = begin + payload.DataSize;
    const char* source_end = std::find(begin, end, '\0');
    if (source_end == end || source_end == begin)
        return std::nullopt;
    FileDropPayload result{std::string(begin, source_end), {}};
    for (const char* path_begin = source_end + 1; path_begin != end;)
    {
        const char* path_end = std::find(path_begin, end, '\0');
        if (path_end == end || path_end == path_begin)
            return std::nullopt;
        result.paths.emplace_back(path_begin, path_end);
        path_begin = path_end + 1;
    }
    if (result.paths.empty())
        return std::nullopt;
    return result;
}

} // namespace

void Application::UpdateGraphBuild()
{
    if (_snapshot == nullptr) return;
    EnsureRemoteSelection();
    EnsureVisibleBranchSelection();
    EnsureTagSelection();
    const std::string key = VisibleBranchesKey();
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
    query.branches = _visible_branches;
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

void Application::ExpandGraphRow(std::size_t visible_row, bool merge_history)
{
    if (_history_view == nullptr || visible_row >= _history_view->items.size()
        || !_history_expansion_pending.empty()) return;
    const HistoryItem& item = _history_view->items[visible_row];
    if (item.kind == HistoryItemKind::CollapsedRegion)
    {
        _history_expansion_pending = item.id;
        _engine.Enqueue(ExpandHistoryRegion{item.id});
    }
    else if (merge_history && item.revision.parents.size() > 1)
    {
        _history_expansion_pending = item.id;
        _engine.Enqueue(ExpandHistoryRegion{item.id, true});
    }
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
    const bool actions_locked = !_active_operation.empty();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##graph filter", "Search changes, IDs, branches, tags", &_graph_filter);
    UpdateGraphBuild();
    ImGui::BeginChild("graph scroll", {}, ImGuiChildFlags_Borders);
    const bool history_window_hovered = ImGui::IsWindowHovered();

    const bool usable = _history_view != nullptr
        && _history_view->items.size() == _graph_rows.size()
        && _history_view->items.size() == _visible_revisions.size();
    const bool working_tree_selected = usable && std::ranges::any_of(_history_view->items,
        [this](const HistoryItem& item) {
            return item.kind == HistoryItemKind::WorkingTree && item.id == _selected_revision;
        });
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
    const bool action_hotkeys = usable && focused && _dialog == Dialog::None
        && _active_operation.empty() && !_selected_revision.empty() && !working_tree_selected
        && !io.WantTextInput
        && !io.KeyCtrl && !io.KeySuper;
    if (action_hotkeys)
    {
        if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N))
            CreateChange(_selected_revision, io.KeyAlt);
        else if (!io.KeyAlt && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_E))
            EnqueueAction(Edit{CheckoutTarget(_selected_revision)});
        else if (!io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_D))
            EnqueueAction(Duplicate{_selected_revision, io.KeyShift});
        else if (!io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_S))
            RequestSquash(_selected_revision, io.KeyShift);
        else if (io.KeyAlt && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S))
            OpenDialog(Dialog::Split);
        else if (!io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_A))
            RequestAbandon(_selected_revision, io.KeyShift);
    }
    if (usable && focused && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift && !io.KeySuper
        && (ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat)
            || ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat)))
    {
        const int direction = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : -1;
        int current = -1;
        for (int index = 0; index < static_cast<int>(_history_view->items.size()); ++index)
            if (_history_view->items[static_cast<std::size_t>(index)].kind != HistoryItemKind::CollapsedRegion
                && _history_view->items[static_cast<std::size_t>(index)].id == _selected_revision)
                current = index;
        for (int index = current + direction; index >= 0
            && index < static_cast<int>(_history_view->items.size()); index += direction)
            if (_history_view->items[static_cast<std::size_t>(index)].kind != HistoryItemKind::CollapsedRegion)
            {
                SelectRevision(_history_view->items[static_cast<std::size_t>(index)].id);
                ImGui::SetScrollY(std::max(0.0f, index * kRowHeight - ImGui::GetContentRegionAvail().y * 0.5f));
                break;
            }
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    int graph_columns = 1;
    for (const GraphRow& row : _graph_rows) graph_columns = std::max(graph_columns, GraphColumnCount(row));
    const std::optional<int> highlighted_track = _history_hovered_track < 0
        ? std::nullopt : std::optional{_history_hovered_track};
    const std::optional<int> highlighted_commit_row = _history_hovered_commit_row < 0
        ? std::nullopt : std::optional{_history_hovered_commit_row};
    int next_hovered_track = -1;
    int next_hovered_commit_row = -1;
    const ImVec2 item_spacing = ImGui::GetStyle().ItemSpacing;
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
            const bool working_tree = item.kind == HistoryItemKind::WorkingTree;
            if (region)
                ImGui::Dummy(ImVec2(width, kRowHeight));
            else
                ImGui::InvisibleButton(working_tree ? "working tree" : "row", ImVec2(width, kRowHeight),
                    ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const float center = (minimum.y + maximum.y) * 0.5f;
            const float lane_width = HistoryLaneWidth(width, graph_columns);
            const float row_content_x = minimum.x
                + HistoryContentOffset(lane_width, GraphColumnCount(row));
            const bool hovered = region ? history_window_hovered && ImGui::IsMouseHoveringRect(minimum, maximum)
                : ImGui::IsItemHovered() && ImGui::GetMousePos().x >= row_content_x;
            const bool clicked = !region && ImGui::IsItemClicked();
            const bool region_clicked = region && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            const bool selected = !region
                && std::ranges::find(_selected_revisions, item.id) != _selected_revisions.end();
            const ImU32 background = selected ? kRowSelected : hovered ? kRowHover : kRowBackground;
            draw->AddRectFilled(minimum, maximum, background, 5.0f);
            if (item.search_match) draw->AddRect(minimum, maximum, IM_COL32(220, 170, 70, 255), 5.0f, 0, 2.0f);

            const float graph_left = minimum.x + kGraphPadding;
            const auto lane_x = [&](int column) { return graph_left + column * lane_width + lane_width * 0.5f; };
            const auto color = [](int track) { return kLaneColors[static_cast<std::size_t>(track) % kLaneColors.size()]; };

            // Hit-test the curves in the actual rendered row. Keeping the
            // resulting track for the following frame lets the highlight be
            // drawn behind every visible segment of that lane.
            if (history_window_hovered && !region && ImGui::GetDragDropPayload() == nullptr
                && ImGui::IsMouseHoveringRect(minimum, maximum))
            {
                const ImVec2 mouse = ImGui::GetMousePos();
                if (mouse.x >= row_content_x)
                {
                    next_hovered_track = row.track;
                    next_hovered_commit_row = index;
                }
                else
                {
                    const float vertical = std::clamp(
                        (mouse.y - minimum.y) / (maximum.y - minimum.y), 0.0f, 1.0f);
                    float closest_distance = std::numeric_limits<float>::max();
                    const auto consider = [&](int track, float x) {
                        const float distance = std::fabs(mouse.x - x);
                        if (distance < closest_distance)
                        {
                            closest_distance = distance;
                            next_hovered_track = track;
                        }
                    };
                    if (vertical < 0.5f)
                    {
                        const float normalized = vertical * 2.0f;
                        const float parameter = 1.0f - std::sqrt(1.0f - normalized);
                        for (std::size_t incoming = 0; incoming < row.incoming_columns.size(); ++incoming)
                        {
                            const float start = lane_x(row.incoming_columns[incoming]);
                            consider(row.incoming_tracks[incoming],
                                start + (lane_x(row.column) - start) * parameter * parameter);
                        }
                    }
                    else
                    {
                        const float parameter = std::sqrt((vertical - 0.5f) * 2.0f);
                        for (std::size_t parent = 0; parent < row.parent_columns.size(); ++parent)
                        {
                            const float start = lane_x(row.column);
                            const float end = lane_x(row.parent_columns[parent]);
                            consider(row.parent_tracks[parent],
                                start * (1.0f - parameter) * (1.0f - parameter)
                                    + end * (1.0f - (1.0f - parameter) * (1.0f - parameter)));
                        }
                    }
                    for (std::size_t before = 0; before < row.tracks_before.size(); ++before)
                    {
                        const int track = row.tracks_before[before];
                        if (std::ranges::find(row.incoming_tracks, track) != row.incoming_tracks.end()) continue;
                        const auto after = std::ranges::find(row.tracks_after, track);
                        if (after == row.tracks_after.end()) continue;
                        float parameter_low = 0.0f;
                        float parameter_high = 1.0f;
                        for (int iteration = 0; iteration < 8; ++iteration)
                        {
                            const float parameter = (parameter_low + parameter_high) * 0.5f;
                            const float y = parameter * parameter * parameter
                                - 1.5f * parameter * parameter + 1.5f * parameter;
                            if (y < vertical) parameter_low = parameter;
                            else parameter_high = parameter;
                        }
                        const float parameter = (parameter_low + parameter_high) * 0.5f;
                        const float smooth = parameter * parameter * (3.0f - 2.0f * parameter);
                        const float start = lane_x(static_cast<int>(before));
                        const float end = lane_x(static_cast<int>(after - row.tracks_after.begin()));
                        consider(track, start + (end - start) * smooth);
                    }
                    if (closest_distance > std::max(5.0f, lane_width * 0.45f))
                        next_hovered_track = -1;
                }
            }
            const auto quadratic = [&](ImVec2 first, ImVec2 control, ImVec2 last, int track) {
                if (highlighted_track == track)
                    draw->AddBezierQuadratic(first, control, last, IM_COL32(255, 255, 255, 115), 5.0f);
                draw->AddBezierQuadratic(first, control, last, color(track), 2.0f);
            };
            const auto cubic = [&](ImVec2 first, ImVec2 first_control, ImVec2 second_control,
                                   ImVec2 last, int track) {
                if (highlighted_track == track)
                    draw->AddBezierCubic(
                        first, first_control, second_control, last, IM_COL32(255, 255, 255, 115), 5.0f);
                draw->AddBezierCubic(first, first_control, second_control, last, color(track), 2.0f);
            };
            const float dot_x = lane_x(row.column);
            // Route pass-by lanes by their stable track identity instead of
            // joining whatever happens to occupy the same column above and
            // below this row. Curves keep eager column compaction readable.
            for (std::size_t incoming = 0; incoming < row.incoming_columns.size(); ++incoming)
                quadratic({lane_x(row.incoming_columns[incoming]), minimum.y},
                    {lane_x(row.incoming_columns[incoming]), center}, {dot_x, center},
                    row.incoming_tracks[incoming]);
            for (std::size_t before = 0; before < row.tracks_before.size(); ++before)
            {
                const int track = row.tracks_before[before];
                if (std::ranges::find(row.incoming_tracks, track) != row.incoming_tracks.end()) continue;
                auto after = std::ranges::find(row.tracks_after, track);
                if (after != row.tracks_after.end())
                {
                    const int after_column = static_cast<int>(after - row.tracks_after.begin());
                    cubic({lane_x(static_cast<int>(before)), minimum.y},
                        {lane_x(static_cast<int>(before)), center}, {lane_x(after_column), center},
                        {lane_x(after_column), maximum.y}, track);
                }
            }
            for (std::size_t parent_index = 0; parent_index < row.parent_columns.size(); ++parent_index)
            {
                const int parent = row.parent_columns[parent_index];
                quadratic({dot_x, center}, {lane_x(parent), center},
                    {lane_x(parent), maximum.y}, row.parent_tracks[parent_index]);
            }

            const float content_x = row_content_x;
            if (working_tree)
            {
                if (clicked) SelectRevision(item.id);
                const float dot_radius = std::min(kDotRadius, lane_width * 0.35f);
                draw->AddCircle({dot_x, center}, dot_radius,
                    ImGui::GetColorU32(ImGuiCol_Text), 0, 2.0f);
                const ImVec2 label_position{content_x, center - ImGui::GetTextLineHeight() * 0.5f};
                // RenderText draws like AddText and also reaches ImGui's text log.
                ImGui::RenderText(label_position, "Working tree");
                if (hovered)
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(item.parents.empty()
                        ? "Working tree changes before the first commit"
                        : "Working tree changes relative to @");
                    ImGui::EndTooltip();
                }
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, item_spacing);
                if (ImGui::BeginPopupContextItem("working tree context"))
                {
                    const std::string& active_commit = CurrentCommit(*_snapshot);
                    const bool has_changes = !_snapshot->status.empty();
                    const bool conflicted = std::ranges::any_of(
                        _snapshot->status, [](const StatusEntry& file) { return file.conflicted; });
                    ImGui::BeginDisabled(actions_locked);
                    if (ActionMenuItem(ICON_MS_COMMIT, "Commit...", nullptr, has_changes))
                    {
                        SelectRevision(item.id);
                        OpenDialog(Dialog::Commit);
                    }
                    // Amending @ with the whole working tree squashes every
                    // change into it; an empty description keeps its message.
                    if (ActionMenuItem(ICON_MS_MERGE, "Squash into @...", nullptr,
                            has_changes && !active_commit.empty()))
                    {
                        SelectRevision(item.id);
                        OpenDialog(Dialog::Commit);
                        if (_dialog == Dialog::Commit)
                        {
                            _input_mode = 1;
                            _dialog_revision = active_commit;
                        }
                    }
                    ImGui::Separator();
                    if (ActionMenuItem(ICON_MS_DELETE, "Abandon changes...", nullptr,
                            has_changes && !conflicted && !active_commit.empty()))
                    {
                        SelectRevision(item.id);
                        RequestWorkingFiles(item.id, _snapshot->status, false);
                    }
                    ImGui::EndDisabled();
                    ImGui::EndPopup();
                }
                ImGui::PopStyleVar();
                ImGui::PopID();
                continue;
            }
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
            const float dot_radius = std::min(kDotRadius, lane_width * 0.35f);
            const bool working_copy_conflicted = revision.working_copy && _snapshot != nullptr
                && std::ranges::any_of(_snapshot->status, [](const StatusEntry& entry) {
                    return entry.conflicted;
                });
            const bool conflicted = revision.conflicted || working_copy_conflicted;
            const ImRect dot_rect({dot_x - dot_radius - 3.0f, center - dot_radius - 3.0f},
                {dot_x + dot_radius + 3.0f, center + dot_radius + 3.0f});
            const bool merge = revision.parents.size() > 1;
            const bool merge_expanded = item.parents.size() > 1;
            const bool merge_hovered = merge && history_window_hovered
                && ImGui::GetDragDropPayload() == nullptr
                && ImGui::IsMouseHoveringRect(dot_rect.Min, dot_rect.Max);
            if (merge_hovered)
            {
                next_hovered_track = row.track;
                next_hovered_commit_row = index;
            }
            const bool merge_clicked = merge_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            if (clicked && !merge_clicked) SelectRevision(revision.oid, io.KeyCtrl);
            if (highlighted_commit_row == index)
                draw->AddCircle({dot_x, center}, dot_radius + 2.5f,
                    IM_COL32(255, 255, 255, 180), 0, 2.5f);
            draw->AddCircleFilled({dot_x, center}, dot_radius,
                conflicted ? kHistoryConflict : revision.working_copy ? kWorkingCommitId
                    : revision.pushed ? kStatusPushed : kStatusUnpushed);
            if (merge)
            {
                const ImU32 sign_color = IM_COL32(25, 25, 25, 255);
                const float half = std::max(2.0f, dot_radius - 2.0f);
                draw->AddLine({dot_x - half, center}, {dot_x + half, center}, sign_color, 1.5f);
                if (!merge_expanded)
                    draw->AddLine({dot_x, center - half}, {dot_x, center + half}, sign_color, 1.5f);
                const char* action = merge_expanded ? "Collapse merge" : "Expand merge";
#ifdef IMGUI_ENABLE_TEST_ENGINE
                ImGuiContext& g = *GImGui;
                const ImGuiID merge_id = ImGui::GetID(action);
                IMGUI_TEST_ENGINE_ITEM_ADD(merge_id, dot_rect, nullptr);
                ImGuiTestEngineHook_ItemInfo(&g, merge_id, action, ImGuiItemStatusFlags_None);
#endif
                if (merge_hovered)
                {
                    ImGui::BeginTooltip(); ImGui::TextUnformatted(action); ImGui::EndTooltip();
                }
                if (merge_clicked)
                {
                    _history_anchor = revision.oid;
                    _history_anchor_offset = index * kRowHeight - ImGui::GetScrollY();
                    ExpandGraphRow(static_cast<std::size_t>(index), true);
                }
            }
            const std::string title = FirstLine(revision.description);
            ImVec2 cursor{content_x, center - ImGui::GetTextLineHeight() * 0.5f};
            struct HistoryBadge
            {
                std::string label;
                ImU32 color = 0;
                bool branch = false;
                bool local = false;
                bool current = false;
                bool elsewhere = false;
                std::string remotes;
            };
            std::vector<HistoryBadge> badges;
            if (const auto refs = _history_refs_by_revision.find(revision.oid);
                refs != _history_refs_by_revision.end())
                for (const std::size_t ref_index : refs->second)
                {
                    const NamedRef& ref = _snapshot->refs[ref_index];
                    auto [label, dimmed_prefix] = ReferenceBadgeLabel(ref, _snapshot->refs);
                    (void)dimmed_prefix;
                    if (label.empty()) continue;
                    const bool branch = ref.kind == GG_NAMED_REF_LOCAL_BRANCH
                        || ref.kind == GG_NAMED_REF_REMOTE_BRANCH;
                    const gg_named_ref_kind local_kind = branch
                        ? GG_NAMED_REF_LOCAL_BRANCH : GG_NAMED_REF_LOCAL_TAG;
                    const gg_named_ref_kind remote_kind = branch
                        ? GG_NAMED_REF_REMOTE_BRANCH : GG_NAMED_REF_REMOTE_TAG;
                    HistoryBadge badge{std::move(label), RefBadgeColor(ref, _snapshot->refs), branch,
                        std::ranges::any_of(_snapshot->refs, [&](const NamedRef& candidate) {
                            return candidate.kind == local_kind && candidate.name == ref.name
                                && candidate.target == ref.target;
                        }), false, false, {}};
                    for (const NamedRef& candidate : _snapshot->refs)
                        if (candidate.kind == GG_NAMED_REF_LOCAL_BRANCH && candidate.name == ref.name
                            && candidate.target == ref.target)
                        {
                            badge.current = candidate.current;
                            badge.elsewhere = !candidate.workspace.empty();
                        }
                    // Branches another workspace has checked out cannot be
                    // checked out or moved here; show them muted.
                    if (badge.elsewhere)
                        badge.color = (badge.color & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 110);
                    std::vector<std::string> remotes;
                    for (const NamedRef& candidate : _snapshot->refs)
                        if (candidate.kind == remote_kind && candidate.name == ref.name
                            && candidate.target == ref.target
                            && !candidate.remote.empty())
                            remotes.push_back(candidate.remote);
                    std::ranges::sort(remotes);
                    const auto unique = std::ranges::unique(remotes);
                    remotes.erase(unique.begin(), unique.end());
                    for (const std::string& remote : remotes)
                    {
                        if (!badge.remotes.empty()) badge.remotes += ", ";
                        badge.remotes += remote;
                    }
                    badges.push_back(std::move(badge));
                }
            const std::string_view shown = title.empty() ? "(no description)" : std::string_view(title);
            const float content_right = maximum.x - 12.0f;
            float badge_width = 0.0f;
            for (const HistoryBadge& badge : badges)
            {
                badge_width += BadgeWidth(badge.label) + FontPx(6.0f);
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
            const HistoryBadge* hovered_badge = nullptr;
            for (const HistoryBadge& badge : badges)
            {
                const ImVec2 badge_start = cursor;
                DrawBadge(draw, cursor, center, badge.label, badge.color, 0,
                    badge.current ? kBadgeCurrentOutline : 0);
                if (ImGui::IsMouseHoveringRect(
                        {badge_start.x, minimum.y}, {cursor.x, maximum.y}))
                    hovered_badge = &badge;
#ifdef IMGUI_ENABLE_TEST_ENGINE
                {
                    ImGuiContext& g = *GImGui;
                    const std::string item_label = "ref:" + badge.label;
                    const ImGuiID badge_id = ImGui::GetID(item_label.c_str());
                    const ImRect badge_rect({badge_start.x, minimum.y}, {cursor.x, maximum.y});
                    IMGUI_TEST_ENGINE_ITEM_ADD(badge_id, badge_rect, nullptr);
                    ImGuiTestEngineHook_ItemInfo(
                        &g, badge_id, badge.label.c_str(), ImGuiItemStatusFlags_None);
                }
#endif
            }

            // A left-button drag that starts on a local branch pill moves
            // that branch instead of the change.
            if (ImGui::IsItemActivated())
            {
                if (hovered_badge != nullptr && hovered_badge->branch && hovered_badge->local
                    && ImGui::GetCurrentContext()->ActiveIdMouseButton == ImGuiMouseButton_Left)
                    _history_branch_drag = hovered_badge->label;
                else
                    _history_branch_drag.clear();
            }
            if (!actions_locked && !_history_branch_drag.empty()
                && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
            {
                ImGui::SetDragDropPayload("GGUI_BRANCH",
                    _history_branch_drag.c_str(), _history_branch_drag.size() + 1);
                ImGui::EndDragDropSource();
            }
            else if (!actions_locked && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
            {
                // Ctrl is the copy modifier during a drag, not a request to
                // toggle the dragged commit out of the current selection.
                if (io.KeyCtrl
                    && std::ranges::find(_selected_revisions, revision.oid) == _selected_revisions.end())
                    SelectRevision(revision.oid, true);
                // A right-button drag is an explicit request to choose the
                // operation after the drop. Keep the ordinary left-button
                // drag's modifier-driven behaviour intact.
                const bool choose_action = ImGui::GetCurrentContext()->ActiveIdMouseButton
                    == ImGuiMouseButton_Right;
                ImGui::SetDragDropPayload(choose_action ? "GGUI_CHANGE_ACTION" : "GGUI_CHANGE",
                    revision.oid.c_str(), revision.oid.size() + 1);
                ImGui::EndDragDropSource();
            }
            std::optional<DropAction> hovered_drop;
            bool hovered_entire_branch = false;
            bool hovered_copy = false;
            bool hovered_file_drop = false;
            bool hovered_action_drop = false;
            bool hovered_branch_drop = false;
            if (!actions_locked && ImGui::BeginDragDropTarget())
            {
                const float ratio = (ImGui::GetMousePos().y - minimum.y) / kRowHeight;
                const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
                if (dragging != nullptr && dragging->IsDataType("GGUI_FILE"))
                {
                    const std::optional<FileDropPayload> file = ParseFileDropPayload(*dragging);
                    if (_compare_to.empty() && file.has_value() && file->source != revision.oid)
                    {
                        hovered_file_drop = true;
                        RenderRevisionTooltip(file->paths.size() == 1 ? "Move file to change"
                                : "Move files to change", revision.oid);
                    }
                }
                else if (dragging != nullptr && dragging->IsDataType("GGUI_CHANGE"))
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
                else if (dragging != nullptr && dragging->IsDataType("GGUI_BRANCH"))
                {
                    const std::string_view name(static_cast<const char*>(dragging->Data));
                    hovered_branch_drop = std::ranges::none_of(_snapshot->refs, [&](const NamedRef& ref) {
                        return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == name
                            && ref.target == revision.oid;
                    });
                    if (hovered_branch_drop)
                        RenderRevisionTooltip("Move branch to", revision.oid, name);
                }
                hovered_action_drop = dragging != nullptr && dragging->IsDataType("GGUI_CHANGE_ACTION");
                if (hovered_action_drop)
                    RenderRevisionTooltip("Choose an action for", revision.oid);
                if (_compare_to.empty())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_FILE"))
                    {
                        const std::optional<FileDropPayload> file = ParseFileDropPayload(*payload);
                        if (file.has_value() && file->source != revision.oid)
                            QueueCommands({MoveFiles{file->source, revision.oid, file->paths}},
                                {file->source, revision.oid},
                                "Moving these files will rewrite a locked source or destination commit.");
                    }
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE"))
                {
                    const std::optional<std::string> source = ParseChangeDropPayload(*payload);
                    if (source.has_value() && *source != revision.oid && hovered_drop.has_value())
                    {
                        _pending_drop = {
                            *source, revision.oid, *hovered_drop, hovered_entire_branch, hovered_copy};
                        OpenDialog(Dialog::ConfirmDrop);
                    }
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE_ACTION"))
                {
                    const std::optional<std::string> source = ParseChangeDropPayload(*payload);
                    if (source.has_value() && *source != revision.oid)
                    {
                        _pending_drop = {*source, revision.oid, DropAction::ReorderBefore, false, false};
                        _open_drop_actions = true;
                    }
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_BRANCH"))
                {
                    const std::string_view name(static_cast<const char*>(payload->Data));
                    const auto branch = std::ranges::find_if(_snapshot->refs, [&](const NamedRef& ref) {
                        return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.name == name;
                    });
                    if (branch != _snapshot->refs.end())
                        MoveBranch(*branch, revision.oid);
                }
                ImGui::EndDragDropTarget();
            }
            if (hovered_file_drop || hovered_branch_drop)
            {
                const float marker_left = dot_x - std::min(kDotRadius, lane_width * 0.35f) - 5.0f;
                draw->AddRect({marker_left, minimum.y + 1.0f}, {maximum.x - 1.0f, maximum.y - 1.0f},
                    IM_COL32(90, 150, 255, 230), 4.0f, ImDrawFlags_None, 2.0f);
            }
            else if (hovered_drop == DropAction::ReorderBefore || hovered_drop == DropAction::ReorderAfter)
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
            else if (hovered_action_drop)
            {
                const float marker_left = dot_x - std::min(kDotRadius, lane_width * 0.35f) - 5.0f;
                draw->AddRect({marker_left, minimum.y + 1.0f}, {maximum.x - 1.0f, maximum.y - 1.0f},
                    IM_COL32(90, 150, 255, 230), 4.0f, ImDrawFlags_None, 2.0f);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, item_spacing);
            if (!hovered_action_drop && ImGui::BeginPopupContextItem("change context"))
            {
                ImGui::BeginDisabled(actions_locked);
                if (ActionMenuItem(ICON_MS_ADD, "New", "N")) { SelectRevision(revision.oid); CreateChange(revision.oid); }
                if (ActionMenuItem(ICON_MS_CALL_SPLIT, "New detached", "Alt+N"))
                { SelectRevision(revision.oid); CreateChange(revision.oid, true); }
                RenderSelectedChangeActions(revision.oid, true);
                ImGui::Separator();
                const NamedRef* branch = BranchAt(*_snapshot, revision.oid);
                const auto checkout_here = [&](const NamedRef& ref) {
                    return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.target == revision.oid
                        && ref.workspace.empty() && !ref.current;
                };
                if (std::ranges::count_if(_snapshot->refs, checkout_here) > 1
                    && ImGui::BeginMenu(TempIconLabel(ICON_MS_LOGIN, "Check out")))
                {
                    for (const NamedRef& ref : _snapshot->refs)
                        if (checkout_here(ref) && ActionMenuItem(ICON_MS_BOOKMARK, ref.name))
                        { SelectRevision(revision.oid); EnqueueAction(Edit{ref.name}); }
                    ImGui::EndMenu();
                }
                if (ActionMenuItem(ICON_MS_BOOKMARK_ADD, "Create branch..."))
                { SelectRevision(revision.oid); OpenDialog(Dialog::Branch); }
                const bool can_move_branch = std::ranges::any_of(_snapshot->refs, [&](const NamedRef& ref) {
                    return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.target != revision.oid;
                });
                const std::string move_branch = IconLabel(ICON_MS_MOVE_ITEM, "Move branch here");
                if (ImGui::BeginMenu(move_branch.c_str(), can_move_branch))
                {
                    for (const NamedRef& ref : _snapshot->refs)
                        if (ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.target != revision.oid
                            && ActionMenuItem(ICON_MS_BOOKMARK, ref.name))
                            MoveBranch(ref, revision.oid);
                    ImGui::EndMenu();
                }
                const auto local_here = [&](const NamedRef& ref) {
                    return ref.kind == GG_NAMED_REF_LOCAL_BRANCH && ref.target == revision.oid;
                };
                if (std::ranges::count_if(_snapshot->refs, local_here) == 1)
                    RenderBranchDeleteMenu(branch->name);
                else if (ImGui::BeginMenu(TempIconLabel(ICON_MS_DELETE, "Delete branch"), branch != nullptr))
                {
                    for (const NamedRef& ref : _snapshot->refs)
                        if (local_here(ref))
                            RenderBranchDeleteMenu(ref.name, true);
                    ImGui::EndMenu();
                }

                ImGui::Separator();
                const std::string remote = branch == nullptr ? "" : RemoteForBranch(*_snapshot, branch->name);
                if (ActionMenuItem(ICON_MS_CLOUD_UPLOAD, "Push", nullptr,
                        branch != nullptr && !remote.empty()))
                    _engine.Enqueue(Push{branch->name, remote});
                if (ActionMenuItem(ICON_MS_PUBLISH, "Push to...", nullptr,
                        branch != nullptr && !_snapshot->remotes.empty()))
                {
                    OpenDialog(Dialog::PushTo);
                    _input_primary = remote;
                    _input_secondary = branch->name;
                }
                ImGui::EndDisabled();
                ImGui::Separator();
                // Keep the common commit-ID actions directly reachable (and
                // compatible with the other reference panels). Additional
                // aliases and the potentially long full description live in
                // the compact Copy submenu below.
                IdCopyMenuItems("commit ID", revision.oid, RevisionPrefix(revision.oid));
                const std::string copy_label = IconLabel(ICON_MS_CONTENT_COPY, "Copy");
                if (ImGui::BeginMenu(copy_label.c_str()))
                {
                    for (std::size_t alias_index = 0; alias_index < revision.aliases.size(); ++alias_index)
                        IdCopyMenuItems("alias " + std::to_string(alias_index + 1), revision.aliases[alias_index],
                            RevisionPrefix(revision.aliases[alias_index]));
                    if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Full description", nullptr,
                            !revision.description.empty()))
                        ImGui::SetClipboardText(revision.description.c_str());
                    ImGui::EndMenu();
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();
            if (hovered && !hovered_drop.has_value() && ImGui::GetDragDropPayload() == nullptr)
            {
                ImGui::BeginTooltip();
                if (hovered_badge != nullptr)
                {
                    ImGui::Text("%s: %s", hovered_badge->branch ? "Branch" : "Tag",
                        hovered_badge->label.c_str());
                    ImGui::Text("Local: %s", hovered_badge->local ? "yes" : "no");
                    ImGui::Text("Remotes: %s", hovered_badge->remotes.empty()
                        ? "(none)" : hovered_badge->remotes.c_str());
                }
                else
                {
                    ImGui::TextUnformatted(shown.data(), shown.data() + shown.size());
                    ImGui::Text("Author: %s", revision.author.empty() ? "(unknown)" : revision.author.c_str());
                    TextLabelledId("Commit: ", revision.oid, RevisionPrefix(revision.oid),
                        CommitIdColor(revision.working_copy));
                }
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
    _history_hovered_track = next_hovered_track;
    _history_hovered_commit_row = next_hovered_commit_row;
    if (reorder_marker.has_value())
        draw->AddLine(reorder_marker->first, reorder_marker->second,
            IM_COL32(100, 175, 255, 255), 4.0f);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    // Keep a small, explicit insertion target after the visible graph. This
    // remains useful when the last item is a collapsed region: resolve the
    // final actual commit rather than treating the region marker as a target.
    ImGui::InvisibleButton("move to end", ImVec2(-1.0f, FontPx(22.0f)));
    const ImVec2 end_minimum = ImGui::GetItemRectMin();
    const ImVec2 end_maximum = ImGui::GetItemRectMax();
    std::string final_revision;
    if (usable)
        for (auto item = _history_view->items.rbegin(); item != _history_view->items.rend(); ++item)
            if (item->kind == HistoryItemKind::Commit)
            {
                final_revision = item->revision.oid;
                break;
            }
    bool hovered_end_drop = false;
    bool hovered_end_action = false;
    if (!actions_locked && !final_revision.empty() && ImGui::BeginDragDropTarget())
    {
        const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
        hovered_end_drop = dragging != nullptr && dragging->IsDataType("GGUI_CHANGE");
        hovered_end_action = dragging != nullptr && dragging->IsDataType("GGUI_CHANGE_ACTION");
        if (hovered_end_drop)
            RenderRevisionTooltip(DropTooltip(DropAction::ReorderAfter), final_revision);
        else if (hovered_end_action)
            RenderRevisionTooltip("Choose an action after", final_revision);
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE"))
        {
            const std::optional<std::string> source = ParseChangeDropPayload(*payload);
            if (source.has_value() && *source != final_revision)
            {
                _pending_drop = {*source, final_revision, DropAction::ReorderAfter, false, false};
                OpenDialog(Dialog::ConfirmDrop);
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("GGUI_CHANGE_ACTION"))
        {
            const std::optional<std::string> source = ParseChangeDropPayload(*payload);
            if (source.has_value() && *source != final_revision)
            {
                _pending_drop = { *source, final_revision, DropAction::ReorderAfter, false, false};
                _open_drop_actions = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (hovered_end_drop || hovered_end_action)
        draw->AddRect({end_minimum.x + 1.0f, end_minimum.y + 1.0f},
            {end_maximum.x - 1.0f, end_maximum.y - 1.0f}, IM_COL32(90, 150, 255, 230),
            5.0f, ImDrawFlags_None, 2.0f);

    if (_history_scroll_frames > 0 && usable)
    {
        ImGui::SetScrollY(std::max(0.0f, _history_scroll_target - _history_anchor_offset));
        if (--_history_scroll_frames == 0) { _history_scroll_target = -1.0f; _history_anchor_offset = 0.0f; }
    }
    ImGui::EndChild();

    // Right-button drags deliberately defer the operation choice until the
    // pointer is released. Opening this popup outside the row loop prevents
    // clipped rows from stealing its position and keeps context menus closed
    // while the action payload is hovering.
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
            _pending_drop.entire_branch = false;
            if (_pending_drop.source != _pending_drop.target)
                OpenDialog(Dialog::ConfirmDrop);
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace Ggui

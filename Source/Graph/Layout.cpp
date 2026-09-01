// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Layout.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace Ggui
{

std::vector<GraphRow> BuildGraphLayout(
    const std::vector<GraphNode>& nodes, std::string_view preferred_tip)
{
    std::unordered_map<std::string_view, int> indices;
    indices.reserve(nodes.size());
    for (int index = 0; index < static_cast<int>(nodes.size()); ++index)
        if (!indices.emplace(nodes[static_cast<std::size_t>(index)].id, index).second)
            throw std::invalid_argument("duplicate graph item ID");
    for (int child = 0; child < static_cast<int>(nodes.size()); ++child)
        for (const std::string& parent : nodes[static_cast<std::size_t>(child)].parents)
        {
            const auto found = indices.find(parent);
            if (found == indices.end()) throw std::invalid_argument("graph parent is missing from the view");
            if (found->second <= child) throw std::invalid_argument("graph is not in children-before-parents order");
        }

    std::vector<bool> preferred(nodes.size());
    auto preferred_node = indices.find(preferred_tip);
    while (preferred_node != indices.end())
    {
        const int index = preferred_node->second;
        preferred[static_cast<std::size_t>(index)] = true;
        if (nodes[static_cast<std::size_t>(index)].parents.empty()) break;
        preferred_node = indices.find(nodes[static_cast<std::size_t>(index)].parents.front());
    }
    const bool has_preferred_branch = std::find(preferred.begin(), preferred.end(), true) != preferred.end();

    std::vector<int> active_nodes;
    std::vector<int> active_tracks;
    // Reserve the primary routing identity for the preferred tip. When a side
    // branch reaches a shared ancestor first, its edge stays active alongside
    // the preferred edge until both meet at the ancestor's dot.
    int next_track = has_preferred_branch ? 1 : 0;
    std::vector<GraphRow> result;
    result.reserve(nodes.size());
    for (int index = 0; index < static_cast<int>(nodes.size()); ++index)
    {
        GraphRow row;
        row.tracks_before = active_tracks;
        std::vector<int> incoming;
        for (int column = 0; column < static_cast<int>(active_nodes.size()); ++column)
            if (active_nodes[static_cast<std::size_t>(column)] == index)
                incoming.push_back(column);
        if (incoming.empty())
        {
            row.column = static_cast<int>(active_nodes.size());
            row.track = preferred[static_cast<std::size_t>(index)] ? 0 : next_track++;
        }
        else
        {
            int primary = incoming.front();
            if (preferred[static_cast<std::size_t>(index)])
            {
                const auto preferred_incoming = std::find_if(incoming.begin(), incoming.end(), [&](int column) {
                    return active_tracks[static_cast<std::size_t>(column)] == 0;
                });
                if (preferred_incoming != incoming.end()) primary = *preferred_incoming;
            }
            row.column = primary - static_cast<int>(
                std::find(incoming.begin(), incoming.end(), primary) - incoming.begin());
            row.track = preferred[static_cast<std::size_t>(index)] ? 0
                : active_tracks[static_cast<std::size_t>(primary)];
            for (const int column : incoming)
            {
                row.incoming_columns.push_back(column);
                row.incoming_tracks.push_back(active_tracks[static_cast<std::size_t>(column)]);
            }
            for (auto current = incoming.rbegin(); current != incoming.rend(); ++current)
            {
                active_nodes.erase(active_nodes.begin() + *current);
                active_tracks.erase(active_tracks.begin() + *current);
            }
        }
        int insert_at = row.column;
        for (std::size_t parent_index = 0; parent_index < nodes[static_cast<std::size_t>(index)].parents.size(); ++parent_index)
        {
            const int parent_index_value = indices.at(nodes[static_cast<std::size_t>(index)].parents[parent_index]);
            auto parent = std::find(active_nodes.begin(), active_nodes.end(), parent_index_value);
            const bool preferred_edge = preferred[static_cast<std::size_t>(index)] && parent_index == 0;
            if (preferred_edge)
            {
                for (auto candidate = parent; candidate != active_nodes.end();
                    candidate = std::find(candidate + 1, active_nodes.end(), parent_index_value))
                    if (active_tracks[static_cast<std::size_t>(candidate - active_nodes.begin())] == row.track)
                    {
                        parent = candidate;
                        break;
                    }
            }
            else if (preferred[static_cast<std::size_t>(parent_index_value)])
            {
                for (auto candidate = parent; candidate != active_nodes.end();
                    candidate = std::find(candidate + 1, active_nodes.end(), parent_index_value))
                    if (active_tracks[static_cast<std::size_t>(candidate - active_nodes.begin())] != 0)
                    {
                        parent = candidate;
                        break;
                    }
            }
            const bool needs_preferred_lane = preferred_edge && (parent == active_nodes.end()
                || active_tracks[static_cast<std::size_t>(parent - active_nodes.begin())] != row.track);
            const bool needs_side_lane = !preferred_edge && preferred[static_cast<std::size_t>(parent_index_value)]
                && parent != active_nodes.end()
                && active_tracks[static_cast<std::size_t>(parent - active_nodes.begin())] == 0;
            if (parent == active_nodes.end() || needs_preferred_lane || needs_side_lane)
            {
                const int column = std::min(insert_at, static_cast<int>(active_nodes.size()));
                parent = active_nodes.insert(active_nodes.begin() + column, parent_index_value);
                active_tracks.insert(active_tracks.begin() + column,
                    parent_index == 0 ? row.track : next_track++);
            }
            const int parent_column = static_cast<int>(parent - active_nodes.begin());
            row.parent_columns.push_back(parent_column);
            row.parent_tracks.push_back(active_tracks[static_cast<std::size_t>(parent_column)]);
            insert_at = row.parent_columns.back() + 1;
        }
        row.tracks_after = active_tracks;
        result.push_back(std::move(row));
    }
    if (!active_nodes.empty()) throw std::invalid_argument("graph contains a cycle or unconsumed item");
    return result;
}

int GraphColumnCount(const GraphRow& row)
{
    int count = std::max(1, row.column + 1);
    count = std::max(count, static_cast<int>(row.tracks_before.size()));
    count = std::max(count, static_cast<int>(row.tracks_after.size()));
    for (const int parent : row.parent_columns) count = std::max(count, parent + 1);
    return count;
}

int GraphColumnCount(const std::vector<GraphRow>& rows)
{
    int result = 1;
    for (const GraphRow& row : rows) result = std::max(result, GraphColumnCount(row));
    return result;
}

} // namespace Ggui

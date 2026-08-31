// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "Layout.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace Ggui
{

std::vector<GraphRow> BuildGraphLayout(const std::vector<GraphNode>& nodes)
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

    std::vector<int> active_nodes;
    std::vector<int> active_tracks;
    int next_track = 0;
    std::vector<GraphRow> result;
    result.reserve(nodes.size());
    for (int index = 0; index < static_cast<int>(nodes.size()); ++index)
    {
        GraphRow row;
        row.tracks_before = active_tracks;
        auto current = std::find(active_nodes.begin(), active_nodes.end(), index);
        if (current == active_nodes.end())
        {
            row.column = static_cast<int>(active_nodes.size());
            row.track = next_track++;
        }
        else
        {
            row.column = static_cast<int>(current - active_nodes.begin());
            row.track = active_tracks[static_cast<std::size_t>(row.column)];
            active_nodes.erase(current);
            active_tracks.erase(active_tracks.begin() + row.column);
        }
        int insert_at = row.column;
        for (std::size_t parent_index = 0; parent_index < nodes[static_cast<std::size_t>(index)].parents.size(); ++parent_index)
        {
            const int parent_index_value = indices.at(nodes[static_cast<std::size_t>(index)].parents[parent_index]);
            auto parent = std::find(active_nodes.begin(), active_nodes.end(), parent_index_value);
            if (parent == active_nodes.end())
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

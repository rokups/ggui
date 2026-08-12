// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
// Lane assignment adapted from ImGit's GraphBuilder with arbitrary-parent support.
#include "Layout.hpp"

#include <algorithm>
#include <unordered_map>

namespace Ggui
{

std::vector<GraphRow> BuildGraphLayout(const std::vector<GraphNode>& nodes)
{
    std::vector<GraphRow> result(nodes.size());
    std::unordered_map<std::string, int> indices;
    indices.reserve(nodes.size());
    for (int index = 0; index < static_cast<int>(nodes.size()); ++index)
        indices.emplace(nodes[index].id, index);

    std::vector<int> active_nodes;
    std::vector<int> active_tracks;
    int next_track = 0;
    for (int index = 0; index < static_cast<int>(nodes.size()); ++index)
    {
        GraphRow& row = result[index];
        row.tracks_before = active_tracks;

        auto current = std::find(active_nodes.begin(), active_nodes.end(), index);
        int current_track = next_track++;
        if (current == active_nodes.end())
        {
            row.column = static_cast<int>(active_nodes.size());
        }
        else
        {
            row.column = static_cast<int>(std::distance(active_nodes.begin(), current));
            current_track = active_tracks[row.column];
            active_nodes.erase(current);
            active_tracks.erase(active_tracks.begin() + row.column);
        }
        row.track = current_track;

        int insert_at = row.column;
        for (std::size_t parent_index = 0; parent_index < nodes[index].parents.size(); ++parent_index)
        {
            const auto found = indices.find(nodes[index].parents[parent_index]);
            if (found == indices.end())
            {
                row.continues_beyond_layout = true;
                continue;
            }
            if (found->second <= index)
                continue;
            auto parent = std::find(active_nodes.begin(), active_nodes.end(), found->second);
            if (parent == active_nodes.end())
            {
                const int column = std::min(insert_at, static_cast<int>(active_nodes.size()));
                parent = active_nodes.insert(active_nodes.begin() + column, found->second);
                active_tracks.insert(
                    active_tracks.begin() + column, parent_index == 0 ? current_track : next_track++);
            }
            row.parent_columns.push_back(static_cast<int>(std::distance(active_nodes.begin(), parent)));
            insert_at = row.parent_columns.back() + 1;
        }
        row.tracks_after = active_tracks;
    }
    return result;
}

int GraphColumnCount(const GraphRow& row)
{
    int count = 1;
    count = std::max(count, row.column + 1);
    count = std::max(count, static_cast<int>(row.tracks_before.size()));
    count = std::max(count, static_cast<int>(row.tracks_after.size()));
    for (int column : row.parent_columns)
        count = std::max(count, column + 1);
    return count;
}

int GraphColumnCount(const std::vector<GraphRow>& rows)
{
    int count = 1;
    for (const GraphRow& row : rows)
        count = std::max(count, GraphColumnCount(row));
    return count;
}

} // namespace Ggui

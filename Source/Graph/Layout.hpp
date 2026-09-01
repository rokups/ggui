// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ggui
{

struct GraphNode
{
    std::string id;
    std::vector<std::string> parents;
};

struct GraphRow
{
    int column = 0;
    int track = 0;
    std::vector<int> tracks_before;
    std::vector<int> tracks_after;
    std::vector<int> incoming_columns;
    std::vector<int> incoming_tracks;
    std::vector<int> parent_columns;
    std::vector<int> parent_tracks;
};

// Input must be a complete explicit DAG in children-before-parents order.
// Missing endpoints, duplicate IDs, backwards edges, and cycles are rejected.
// When preferred_tip is present, its first-parent ancestry gets the primary
// color even if another child activates a shared ancestor first.
std::vector<GraphRow> BuildGraphLayout(
    const std::vector<GraphNode>& nodes, std::string_view preferred_tip = {});
int GraphColumnCount(const GraphRow& row);
int GraphColumnCount(const std::vector<GraphRow>& rows);

} // namespace Ggui

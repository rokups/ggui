// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>
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
    std::vector<int> parent_columns;
    // Stable color identity for each parent edge. An edge may adopt an
    // already-active parent's track, but never changes color between dots.
    std::vector<int> parent_tracks;
};

// Input must be a complete explicit DAG in children-before-parents order.
// Missing endpoints, duplicate IDs, backwards edges, and cycles are rejected.
std::vector<GraphRow> BuildGraphLayout(const std::vector<GraphNode>& nodes);
int GraphColumnCount(const GraphRow& row);
int GraphColumnCount(const std::vector<GraphRow>& rows);

} // namespace Ggui

// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
// Lane assignment and rendering geometry adapted from ImGit's graph renderer.
#pragma once

#include <string>
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
    bool continues_beyond_layout = false;
    std::vector<int> tracks_before;
    std::vector<int> tracks_after;
    std::vector<int> parent_columns;
};

std::vector<GraphRow> BuildGraphLayout(const std::vector<GraphNode>& nodes);
int GraphColumnCount(const GraphRow& row);
int GraphColumnCount(const std::vector<GraphRow>& rows);

} // namespace Ggui

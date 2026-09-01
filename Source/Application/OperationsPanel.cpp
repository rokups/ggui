// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

namespace Ggui
{
using namespace ApplicationInternal;

void Application::RenderOperations()
{
    if (!ImGui::Begin("Operations", &_show_operations))
    {
        ImGui::End();
        return;
    }

    // Current operation status
    if (!_error_message.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.35f, 1.0f), "%s", _error_message.c_str());
        ImGui::Separator();
    }
    if (!_progress_phase.empty())
    {
        const float progress = _progress_total == 0 ? 0.0f
                                                    : static_cast<float>(_progress_completed) / _progress_total;
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), _progress_phase.c_str());
    }
    if (!_status_message.empty())
        ImGui::TextDisabled("%s", _status_message.c_str());

    // Operation history
    if (ImGui::BeginTable("operation table", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Operation", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (const Operation& operation : _snapshot->operations)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(operation.description.c_str());
            ImGui::TableNextColumn();
            TextHighlightedId(operation.oid, OperationPrefix(operation.oid), CommitIdColor(false));
            ImGui::TableNextColumn();
            ImGui::PushID(&operation);
            ImGui::BeginDisabled(!_active_operation.empty());
            if (ImGui::SmallButton("Restore")) EnqueueAction(RestoreOperation{operation.oid});
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace Ggui

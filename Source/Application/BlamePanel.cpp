// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace Ggui
{
using namespace ApplicationInternal;

namespace
{

std::string BlameLineText(const BlameLine& line)
{
    std::string result = line.contents;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

bool Matches(const BlameLine& line, std::string_view filter)
{
    return filter.empty() || ContainsInsensitive(line.author, filter)
        || ContainsInsensitive(line.author_email, filter)
        || ContainsInsensitive(line.revision, filter)
        || ContainsInsensitive(line.summary, filter)
        || ContainsInsensitive(line.contents, filter);
}

} // namespace

void Application::RenderBlame()
{
    if (!_show_blame)
        return;
    if (!ImGui::Begin("Blame", &_show_blame))
    {
        ImGui::End();
        return;
    }

    if (_blame_path.empty())
    {
        ImGui::TextDisabled("Select a file to inspect its line history.");
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted(_blame_path.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("@ %s", ShortId(_blame_revision).c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Blame revision %s", _blame_revision.c_str());
    ImGui::SameLine();
    if (ActionButton(ICON_MS_REFRESH, "Refresh", ImVec2(0.0f, 0.0f)))
        RequestBlame(_blame_revision, _blame_path);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##blame filter", "Filter author, revision, or line", &_blame_filter);

    if (_blame_loading)
    {
        ImGui::TextDisabled("Loading blame...");
        ImGui::End();
        return;
    }
    if (_blame.lines.empty())
    {
        ImGui::TextDisabled("No blame information is available for this file.");
        ImGui::End();
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg
        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("blame lines", 4, flags))
    {
        ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, FontPx(54.0f));
        ImGui::TableSetupColumn("Commit", ImGuiTableColumnFlags_WidthFixed, FontPx(86.0f));
        ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthFixed, FontPx(150.0f));
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        std::size_t visible = 0;
        for (const BlameLine& line : _blame.lines)
        {
            if (!Matches(line, _blame_filter))
                continue;
            ++visible;
            ImGui::PushID(static_cast<int>(line.line));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%zu", line.line);
            ImGui::TableSetColumnIndex(1);
            const std::string id = ShortId(line.revision);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.58f, 0.95f, 1.0f));
            if (ImGui::SmallButton(id.c_str()))
                RevealRevision(line.revision);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reveal %s in history", line.revision.c_str());
            if (ImGui::BeginPopupContextItem("blame commit context"))
            {
                if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy commit ID"))
                    ImGui::SetClipboardText(line.revision.c_str());
                if (ActionMenuItem(ICON_MS_OPEN_IN_NEW, "Reveal in history"))
                    RevealRevision(line.revision);
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(line.author.empty() ? "(unknown)" : line.author.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                if (!line.author_email.empty()) ImGui::TextUnformatted(line.author_email.c_str());
                if (line.timestamp != 0) ImGui::TextUnformatted(FormatTimestamp(line.timestamp).c_str());
                if (line.boundary) ImGui::TextDisabled("Boundary commit");
                if (!line.summary.empty()) ImGui::TextWrapped("%s", line.summary.c_str());
                ImGui::EndTooltip();
            }
            ImGui::TableSetColumnIndex(3);
            const std::string contents = BlameLineText(line);
            ImGui::TextUnformatted(contents.c_str());
            ImGui::PopID();
        }
        if (visible == 0)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("No matching lines.");
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace Ggui

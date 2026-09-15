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
#include <utility>
#include <vector>

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
        || ContainsInsensitive(line.contents, filter)
        || ContainsInsensitive(line.previous_revision, filter)
        || ContainsInsensitive(line.previous_author, filter)
        || ContainsInsensitive(line.previous_author_email, filter)
        || ContainsInsensitive(line.previous_summary, filter);
}

std::string AuthorLabel(std::string_view author)
{
    return author.empty() ? "(unknown)" : std::string(author);
}

bool HasPreviousCommit(const BlameLine& line)
{
    return !line.previous_revision.empty() && line.previous_revision != line.revision;
}

std::string SourceLineLabel(const BlameLine& line)
{
    if (line.previous_line == 0 || line.previous_line == line.line)
        return "Source line " + std::to_string(line.original_line);
    return "Source line " + std::to_string(line.previous_line);
}

std::size_t BlameBlockCount(const std::vector<BlameLine>& lines)
{
    std::size_t count = 0;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        if (index == 0 || lines[index - 1].revision != lines[index].revision)
            ++count;
    }
    return count;
}

ImU32 BlameRevisionColor(const BlameLine& line)
{
    // Keep the blame column quiet like GitHub/GitLab. The colored rail is an
    // ownership cue, not a second branch graph; boundary lines get the same
    // modified-state accent used elsewhere in the application.
    if (line.revision.empty())
        return kTextMuted;
    return line.boundary ? kStatusModified : kCommitId;
}

std::string BlameBlockRange(const std::vector<BlameLine>& lines, std::size_t first,
    std::size_t last)
{
    if (first >= lines.size() || last >= lines.size())
        return {};
    if (first == last)
        return "line " + std::to_string(lines[first].line);
    return "lines " + std::to_string(lines[first].line) + "–" + std::to_string(lines[last].line);
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

    const auto requested_history = std::ranges::find(_history_revisions, _blame_revision, &Revision::oid);
    const Revision* viewed = requested_history != _history_revisions.end() ? &*requested_history
        : (!_blame.viewed_revision.oid.empty()
                && (_blame.viewed_revision.oid == _blame_revision || _blame.revision == _blame_revision)
                ? &_blame.viewed_revision : nullptr);
    const std::string viewed_oid = viewed != nullptr && !viewed->oid.empty()
        ? viewed->oid : _blame_revision;
    const ImU32 viewed_color = CommitIdColor(_snapshot != nullptr && viewed_oid == _snapshot->working_copy);

    const auto render_revision_tooltip = [this](std::string_view title, std::string_view revision,
                                                  const Revision* info, std::string_view path)
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(title.data(), title.data() + title.size());
        if (!revision.empty())
        {
            const std::string id(revision);
            TextLabelledId("Commit ", id, RevisionPrefix(id),
                CommitIdColor(_snapshot != nullptr && id == _snapshot->working_copy));
        }
        if (info != nullptr)
        {
            ImGui::TextUnformatted(AuthorLabel(info->author).c_str());
            if (!info->author_email.empty())
                ImGui::TextDisabled("%s", info->author_email.c_str());
            if (info->timestamp != 0)
                ImGui::TextDisabled("%s", FormatTimestamp(info->timestamp).c_str());
            if (!info->description.empty())
                ImGui::TextWrapped("%s", info->description.c_str());
        }
        if (!path.empty())
            ImGui::TextDisabled("Path: %.*s", static_cast<int>(path.size()), path.data());
        ImGui::EndTooltip();
    };

    ImGui::TextUnformatted(_blame_path.c_str());
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextDisabled("@");
    ImGui::SameLine(0.0f, 5.0f);
    if (!viewed_oid.empty())
    {
        if (HighlightedIdButton(viewed_oid, RevisionPrefix(viewed_oid), viewed_color))
            RevealRevision(viewed_oid);
        if (ImGui::IsItemHovered())
            render_revision_tooltip("File snapshot", viewed_oid, viewed, _blame_path);
        if (ImGui::BeginPopupContextItem("blame snapshot context"))
        {
            IdCopyMenuItems("commit ID", viewed_oid, RevisionPrefix(viewed_oid));
            if (ActionMenuItem(ICON_MS_OPEN_IN_NEW, "Reveal commit"))
                RevealRevision(viewed_oid);
            ImGui::EndPopup();
        }
    }
    else
        ImGui::TextDisabled("(unknown)");
    if (viewed != nullptr)
    {
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextDisabled("%s", AuthorLabel(viewed->author).c_str());
        if (ImGui::IsItemHovered() && !viewed->author_email.empty())
            ImGui::SetTooltip("%s", viewed->author_email.c_str());
        if (viewed->timestamp != 0)
        {
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextDisabled("· %s", FormatTimestamp(viewed->timestamp).c_str());
        }
        if (!viewed->description.empty())
        {
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextDisabled("· %s", FirstLine(viewed->description).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", viewed->description.c_str());
        }
    }
    ImGui::SameLine(0.0f, 12.0f);
    if (ActionButton(ICON_MS_REFRESH, "Refresh", ImVec2(0.0f, 0.0f)))
        RequestBlame(_blame_revision, _blame_path);

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##blame filter", "Filter commits, authors, messages, or source lines",
        &_blame_filter);

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

    std::vector<std::size_t> visible_lines;
    visible_lines.reserve(_blame.lines.size());
    for (std::size_t index = 0; index < _blame.lines.size(); ++index)
    {
        if (Matches(_blame.lines[index], _blame_filter))
            visible_lines.push_back(index);
    }

    ImGui::TextDisabled("%zu lines · %zu change blocks", _blame.lines.size(), BlameBlockCount(_blame.lines));
    if (!_blame_filter.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("· %zu matching", visible_lines.size());
    }

    if (visible_lines.empty())
    {
        ImGui::TextDisabled("No matching lines.");
        ImGui::End();
        return;
    }

    std::optional<std::pair<std::string, std::string>> pending_blame;
    const auto queue_blame = [&pending_blame](const BlameLine& line, bool before) {
        const std::string& revision = before ? line.blame_before_revision : line.previous_revision;
        const std::string& path = before ? line.blame_before_path : line.previous_path;
        if (!revision.empty() && !path.empty())
            pending_blame = std::make_pair(revision, path);
    };
    const auto render_context = [this, &queue_blame](const BlameLine& line)
    {
        if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy commit ID", nullptr, !line.revision.empty()))
            ImGui::SetClipboardText(line.revision.c_str());
        if (ActionMenuItem(ICON_MS_OPEN_IN_NEW, "Reveal commit", nullptr, !line.revision.empty()))
            RevealRevision(line.revision);
        if (!line.blame_before_revision.empty() && !line.blame_before_path.empty())
        {
            ImGui::Separator();
            if (ActionMenuItem(ICON_MS_HISTORY, "Blame before this change"))
                queue_blame(line, true);
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy prior commit ID"))
                ImGui::SetClipboardText(line.blame_before_revision.c_str());
        }
        if (HasPreviousCommit(line))
        {
            ImGui::Separator();
            if (ActionMenuItem(ICON_MS_OPEN_IN_NEW, "Reveal originating commit"))
                RevealRevision(line.previous_revision);
            if (!line.previous_path.empty() && ActionMenuItem(ICON_MS_PERSON, "Blame originating source"))
                queue_blame(line, false);
        }
        ImGui::Separator();
        if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy source line"))
            ImGui::SetClipboardText(BlameLineText(line).c_str());
    };

    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersInnerH
        | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
        | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("blame lines", 3, flags))
    {
        // Blame is commonly docked beside the changes list, so keep the
        // metadata column readable without starving the source column at
        // narrow widths. Users can still resize it interactively.
        const float metadata_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.38f,
            FontPx(160.0f), FontPx(320.0f));
        ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, FontPx(54.0f));
        ImGui::TableSetupColumn("Last changed", ImGuiTableColumnFlags_WidthFixed, metadata_width);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (std::size_t visible_index = 0; visible_index < visible_lines.size(); ++visible_index)
        {
            const std::size_t index = visible_lines[visible_index];
            const BlameLine& line = _blame.lines[index];
            const bool block_start = visible_index == 0 || visible_lines[visible_index - 1] + 1 != index
                || _blame.lines[visible_lines[visible_index - 1]].revision != line.revision;
            const ImU32 accent = BlameRevisionColor(line);

            ImGui::PushID(static_cast<int>(line.line));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%zu", line.line);
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("blame line context");

            ImGui::TableSetColumnIndex(1);
            // The thin rail and lightly tinted metadata cell make a hunk read
            // as one unit without turning the blame view into a branch graph.
            const ImRect cell = ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), 1);
            ImGui::GetWindowDrawList()->AddRectFilled(cell.Min,
                ImVec2(cell.Min.x + FontPx(3.0f), cell.Max.y), accent);
            if (block_start)
            {
                if (!line.revision.empty())
                {
                    if (HighlightedIdButton(line.revision, RevisionPrefix(line.revision), accent))
                        RevealRevision(line.revision);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Reveal %s", line.revision.c_str());
                }
                else
                    ImGui::TextDisabled("(unknown)");
                if (!line.blame_before_revision.empty() && !line.blame_before_path.empty())
                {
                    ImGui::SameLine(0.0f, 6.0f);
                    if (ActionButton(ICON_MS_HISTORY, "Before", ImVec2(0.0f, 0.0f)))
                        queue_blame(line, true);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Blame this file before the change");
                }
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::TextDisabled("%s", AuthorLabel(line.author).c_str());
                if (line.timestamp != 0)
                {
                    ImGui::SameLine(0.0f, 6.0f);
                    ImGui::TextDisabled("· %s", FormatTimestamp(line.timestamp).c_str());
                }
                if (!line.summary.empty())
                {
                    ImGui::SameLine(0.0f, 6.0f);
                    ImGui::TextDisabled("· %s", FirstLine(line.summary).c_str());
                }
            }
            else
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("blame line context");

            ImGui::TableSetColumnIndex(2);
            ImGui::PushFont(DiffFont(), 0.0f);
            ImGui::TextUnformatted(BlameLineText(line).c_str());
            ImGui::PopFont();
            const bool source_hovered = ImGui::IsItemHovered();
            if (source_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("blame line context");

            if (source_hovered)
            {
                ImGui::BeginTooltip();
                ImGui::Text("Line %zu · %s", line.line, SourceLineLabel(line).c_str());
                if (block_start)
                {
                    std::size_t block_last = visible_index;
                    while (block_last + 1 < visible_lines.size()
                        && visible_lines[block_last + 1] == visible_lines[block_last] + 1
                        && _blame.lines[visible_lines[block_last + 1]].revision == line.revision)
                        ++block_last;
                    ImGui::TextDisabled("Change block · %s", BlameBlockRange(_blame.lines, index,
                        visible_lines[block_last]).c_str());
                }
                ImGui::Separator();
                ImGui::TextUnformatted("Last changed by");
                ImGui::TextDisabled("%s", line.revision.empty() ? "(none)" : line.revision.c_str());
                ImGui::TextUnformatted(AuthorLabel(line.author).c_str());
                if (!line.author_email.empty())
                    ImGui::TextDisabled("%s", line.author_email.c_str());
                if (line.timestamp != 0)
                    ImGui::TextDisabled("%s", FormatTimestamp(line.timestamp).c_str());
                if (!line.summary.empty())
                    ImGui::TextWrapped("%s", line.summary.c_str());
                if (!line.blame_before_revision.empty() && !line.blame_before_path.empty())
                    ImGui::TextDisabled("Use Before to inspect %s", ShortId(line.blame_before_revision).c_str());
                if (HasPreviousCommit(line))
                {
                    ImGui::Separator();
                    ImGui::TextUnformatted("Originating source");
                    ImGui::TextDisabled("%s", line.previous_revision.c_str());
                    if (!line.previous_path.empty() && line.previous_path != _blame_path)
                        ImGui::TextDisabled("%s", line.previous_path.c_str());
                    ImGui::TextUnformatted(AuthorLabel(line.previous_author).c_str());
                    if (!line.previous_author_email.empty())
                        ImGui::TextDisabled("%s", line.previous_author_email.c_str());
                    if (line.previous_timestamp != 0)
                        ImGui::TextDisabled("%s", FormatTimestamp(line.previous_timestamp).c_str());
                    if (!line.previous_summary.empty() && line.previous_summary != line.summary)
                        ImGui::TextWrapped("%s", line.previous_summary.c_str());
                }
                if (line.boundary)
                    ImGui::TextDisabled("Boundary commit");
                ImGui::EndTooltip();
            }

            if (ImGui::BeginPopup("blame line context"))
            {
                render_context(line);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
    if (pending_blame.has_value())
        RequestBlame(pending_blame->first, pending_blame->second);
}

} // namespace Ggui

// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

inline constexpr std::size_t kNoBlameBlock = static_cast<std::size_t>(-1);

// One editor row: the blame line it shows and the change block it belongs to.
struct BlameRow
{
    std::size_t line_index = 0;
    std::size_t block = 0;
};

// Consecutive visible rows last changed by the same commit. Labels are
// formatted once per load so the gutter does not allocate while drawing.
struct BlameBlock
{
    std::size_t first_row = 0;
    std::size_t last_row = 0;
    std::string author;
    std::string summary;
    std::string date;
};

struct BlameView
{
    std::string text;
    std::vector<BlameRow> rows;
    std::vector<BlameBlock> blocks;
};

BlameView BuildBlameView(const std::vector<BlameLine>& lines, std::string_view filter)
{
    BlameView result;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const BlameLine& line = lines[index];
        if (!Matches(line, filter))
            continue;
        const bool block_start = result.rows.empty() || result.rows.back().line_index + 1 != index
            || lines[result.rows.back().line_index].revision != line.revision;
        if (block_start)
        {
            const std::string timestamp = line.timestamp != 0 ? FormatTimestamp(line.timestamp) : std::string();
            result.blocks.push_back({result.rows.size(), result.rows.size(), AuthorLabel(line.author),
                FirstLine(line.summary), timestamp.substr(0, std::min<std::size_t>(timestamp.size(), 10))});
        }
        result.blocks.back().last_row = result.rows.size();
        if (!result.rows.empty())
            result.text += '\n';
        result.text += BlameLineText(line);
        result.rows.push_back({index, result.blocks.size() - 1});
    }
    return result;
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

    // Persistent blame viewer. The source is shown in the same read-only
    // editor as the diff so it can be selected, searched, and copied; the
    // blame metadata lives in the editor's line decorator gutter.
    static TextEditor editor;
    static BlameView view;
    static std::uint64_t loaded_generation = 0;
    static std::string loaded_revision;
    static std::string loaded_path;
    static std::string loaded_filter;
    static const BlameLine* loaded_lines = nullptr;
    static std::size_t loaded_line_count = 0;
    static bool dark_palette = !_dark_theme;
    static float decorator_width = 0.0f;
    static ImFont* gutter_font = nullptr;
    static std::size_t hovered_block = kNoBlameBlock;
    static std::size_t next_hovered_block = kNoBlameBlock;
    static std::size_t hovered_row = kNoBlameBlock;
    static std::size_t clicked_row = kNoBlameBlock;
    static bool clicked_revision = false;

    if (loaded_generation != _blame.generation || loaded_revision != _blame.revision
        || loaded_path != _blame.path || loaded_lines != _blame.lines.data()
        || loaded_line_count != _blame.lines.size() || loaded_filter != _blame_filter)
    {
        loaded_generation = _blame.generation;
        loaded_revision = _blame.revision;
        loaded_path = _blame.path;
        loaded_lines = _blame.lines.data();
        loaded_line_count = _blame.lines.size();
        loaded_filter = _blame_filter;
        view = BuildBlameView(_blame.lines, _blame_filter);
        editor.SetLanguage(DiffLanguage(_blame_path));
        editor.SetText(view.text);
        editor.SetReadOnlyEnabled(true);
        editor.SetShowLineNumbersEnabled(false);
        hovered_block = kNoBlameBlock;
    }

    if (dark_palette != _dark_theme)
    {
        dark_palette = _dark_theme;
        TextEditor::Palette palette = _dark_theme ? TextEditor::GetDarkPalette() : TextEditor::GetLightPalette();
        palette[static_cast<std::size_t>(TextEditor::Color::selection)] =
            ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
        editor.SetPalette(palette);
    }

    ImGui::TextDisabled("%zu lines · %zu change blocks", _blame.lines.size(), BlameBlockCount(_blame.lines));
    if (!_blame_filter.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("· %zu matching", view.rows.size());
    }

    if (view.rows.empty())
    {
        ImGui::TextDisabled("No matching lines.");
        ImGui::End();
        return;
    }

    // Blame is commonly docked beside the changes list, so keep the gutter
    // readable without starving the source at narrow widths.
    gutter_font = ImGui::GetFont();
    const float gutter_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.38f,
        FontPx(180.0f), FontPx(340.0f));
    if (decorator_width != gutter_width)
    {
        decorator_width = gutter_width;
        editor.SetLineDecorator(decorator_width, [this](TextEditor::Decorator& decorator) {
            if (decorator.line < 0 || static_cast<std::size_t>(decorator.line) >= view.rows.size())
                return;
            const std::size_t row_index = static_cast<std::size_t>(decorator.line);
            const BlameRow& row = view.rows[row_index];
            const BlameBlock& block = view.blocks[row.block];
            const BlameLine& line = _blame.lines[row.line_index];
            const std::size_t first = block.first_row;

            ImGui::PushFont(gutter_font, 0.0f);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 minimum = ImGui::GetCursorScreenPos();
            const ImVec2 maximum(minimum.x + decorator.width, minimum.y + decorator.height);
            // The editor draws code at the top of each row; match its baseline.
            const float text_y = minimum.y;
            const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(minimum, maximum);

            if (row.block == hovered_block)
                draw->AddRectFilled(minimum, maximum, ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.45f));
            // The thin rail makes a change block read as one unit without
            // turning the blame view into a branch graph.
            draw->AddRectFilled(minimum, ImVec2(minimum.x + FontPx(3.0f), maximum.y), BlameRevisionColor(line));
            if (row_index == first && row_index != 0)
            {
                const ImGuiWindow* window = ImGui::GetCurrentWindow();
                draw->AddLine(ImVec2(window->InnerClipRect.Min.x, minimum.y),
                    ImVec2(window->InnerClipRect.Max.x, minimum.y), ImGui::GetColorU32(ImGuiCol_Separator, 0.6f));
            }

            // Line numbers follow the file, not the filtered editor rows.
            const char* number = nullptr;
            const char* number_end = nullptr;
            ImFormatStringToTempBuffer(&number, &number_end, "%zu", line.line);
            const float number_width = ImGui::CalcTextSize(number, number_end).x;
            const float number_x = maximum.x - number_width - FontPx(4.0f);
            draw->AddText(ImVec2(number_x, text_y), ImGui::GetColorU32(ImGuiCol_TextDisabled), number, number_end);

            const float left = minimum.x + FontPx(9.0f);
            const float right = number_x - FontPx(10.0f);
            bool revision_hovered = false;
            if (row_index == first)
            {
                float x = left;
                if (!line.revision.empty())
                {
                    const std::size_t shown = std::min(line.revision.size(),
                        std::max<std::size_t>(8, RevisionPrefix(line.revision)));
                    const float id_width = ImGui::CalcTextSize(line.revision.data(),
                        line.revision.data() + shown).x;
                    revision_hovered = hovered && ImGui::IsMouseHoveringRect(ImVec2(x, minimum.y),
                        ImVec2(x + id_width, maximum.y));
                    DrawHighlightedIdWithin(draw, ImVec2(x, text_y), right, line.revision,
                        RevisionPrefix(line.revision), BlameRevisionColor(line));
                    if (revision_hovered)
                        draw->AddLine(ImVec2(x, text_y + ImGui::GetTextLineHeight()),
                            ImVec2(std::min(x + id_width, right), text_y + ImGui::GetTextLineHeight()),
                            BlameRevisionColor(line));
                    x += id_width + FontPx(8.0f);
                }
                else
                {
                    DrawTextWithin(draw, ImVec2(x, text_y), right, "(unknown)", kTextMuted);
                    x += ImGui::CalcTextSize("(unknown)").x + FontPx(8.0f);
                }
                const float date_width = ImGui::CalcTextSize(block.date.c_str()).x;
                const bool date_fits = !block.date.empty()
                    && right - x >= date_width + FontPx(60.0f);
                const float author_right = date_fits ? right - date_width - FontPx(8.0f) : right;
                DrawTextWithin(draw, ImVec2(x, text_y), author_right, block.author,
                    ImGui::GetColorU32(ImGuiCol_Text));
                if (date_fits)
                    draw->AddText(ImVec2(right - date_width, text_y),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), block.date.c_str());
            }
            else if (row_index == first + 1 && !line.summary.empty())
                DrawTextWithin(draw, ImVec2(left, text_y), right, block.summary,
                    ImGui::GetColorU32(ImGuiCol_TextDisabled));
            ImGui::PopFont();

            if (hovered)
            {
                next_hovered_block = row.block;
                hovered_row = row_index;
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    clicked_row = row_index;
                    clicked_revision = revision_hovered;
                }
            }
        });
    }

    next_hovered_block = kNoBlameBlock;
    hovered_row = kNoBlameBlock;
    clicked_row = kNoBlameBlock;
    clicked_revision = false;
    ImGui::PushFont(DiffFont(), 0.0f);
    editor.Render("##blame view", ImGui::GetContentRegionAvail(), true);
    ImGuiWindow* view_window = ImGui::GetCurrentWindow()->DC.ChildWindows.back();
    IM_ASSERT(view_window->ChildId == ImGui::GetItemID());
    const bool view_hovered = ImGui::IsItemHovered();
    const float line_height = std::max(editor.GetLineHeight(), 1.0f);
    ImGui::PopFont();
    hovered_block = next_hovered_block;

    std::optional<std::pair<std::string, std::string>> pending_blame;
    const auto queue_blame = [&pending_blame](const BlameLine& line, bool before) {
        const std::string& revision = before ? line.blame_before_revision : line.previous_revision;
        const std::string& path = before ? line.blame_before_path : line.previous_path;
        if (!revision.empty() && !path.empty())
            pending_blame = std::make_pair(revision, path);
    };

    // Gutter clicks reveal the commit or select its whole change block.
    if (clicked_row != kNoBlameBlock)
    {
        const BlameRow& row = view.rows[clicked_row];
        const BlameBlock& block = view.blocks[row.block];
        const BlameLine& line = _blame.lines[row.line_index];
        if (clicked_revision && !line.revision.empty())
            RevealRevision(line.revision);
        else
            editor.SelectLines(static_cast<int>(block.first_row), static_cast<int>(block.last_row));
    }

    if (hovered_row != kNoBlameBlock && !ImGui::IsPopupOpen("blame line context"))
    {
        const BlameRow& row = view.rows[hovered_row];
        const BlameBlock& block = view.blocks[row.block];
        const BlameLine& line = _blame.lines[row.line_index];
        ImGui::BeginTooltip();
        ImGui::Text("Line %zu · %s", line.line, SourceLineLabel(line).c_str());
        ImGui::TextDisabled("Change block · %s", BlameBlockRange(_blame.lines,
            view.rows[block.first_row].line_index, view.rows[block.last_row].line_index).c_str());
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
        ImGui::Separator();
        ImGui::TextDisabled("%s", line.revision.empty() ? "Click to select this change block"
                : "Click the commit to reveal it, elsewhere to select the block");
        if (!line.blame_before_revision.empty() && !line.blame_before_path.empty())
            ImGui::TextDisabled("Right-click to blame before this change");
        ImGui::EndTooltip();
    }

    // Line and change-block context menu
    static std::size_t context_row = kNoBlameBlock;
    if (view_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        context_row = static_cast<std::size_t>(std::clamp(static_cast<int>(std::floor(
            (ImGui::GetMousePos().y - view_window->DC.CursorStartPos.y) / line_height)),
            0, static_cast<int>(view.rows.size()) - 1));
        ImGui::OpenPopup("blame line context");
    }
    if (ImGui::BeginPopup("blame line context"))
    {
        if (context_row >= view.rows.size())
            ImGui::CloseCurrentPopup();
        else
        {
            const BlameRow& row = view.rows[context_row];
            const BlameBlock& block = view.blocks[row.block];
            const BlameLine& line = _blame.lines[row.line_index];
            ImGui::BeginDisabled(!editor.AnyCursorHasSelection());
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy", "Ctrl+C"))
                editor.Copy();
            ImGui::EndDisabled();
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy source line"))
                ImGui::SetClipboardText(BlameLineText(line).c_str());
            if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy change block"))
            {
                std::string text;
                for (std::size_t index = block.first_row; index <= block.last_row; ++index)
                    text += BlameLineText(_blame.lines[view.rows[index].line_index]) + '\n';
                ImGui::SetClipboardText(text.c_str());
            }
            if (ActionMenuItem(ICON_MS_SELECT_ALL, "Select change block"))
                editor.SelectLines(static_cast<int>(block.first_row), static_cast<int>(block.last_row));
            ImGui::Separator();
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
        }
        ImGui::EndPopup();
    }
    ImGui::End();
    if (pending_blame.has_value())
        RequestBlame(pending_blame->first, pending_blame->second);
}

} // namespace Ggui

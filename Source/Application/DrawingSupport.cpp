// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <imgui_stdlib.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace Ggui::ApplicationInternal
{

void DrawBadge(ImDrawList* draw, ImVec2& cursor, float center_y, std::string_view label, ImU32 color,
    std::size_t dimmed_prefix)
{
    const ImVec2 text_size = ImGui::CalcTextSize(label.data(), label.data() + label.size());
    const float pad_x = FontPx(7.0f);
    const float pad_top = FontPx(3.0f);
    const float pad_bottom = pad_top;
    const ImVec2 minimum(cursor.x, center_y - text_size.y * 0.5f - pad_top);
    const ImVec2 maximum(cursor.x + text_size.x + pad_x * 2.0f, center_y + text_size.y * 0.5f + pad_bottom);
    draw->AddRectFilled(minimum, maximum, color, FontPx(6.0f));
    ImVec2 text(minimum.x + pad_x, minimum.y + pad_top);
    if (dimmed_prefix != 0)
    {
        draw->AddText(text, kBadgeTextMuted, label.data(), label.data() + dimmed_prefix);
        text.x += ImGui::CalcTextSize(label.data(), label.data() + dimmed_prefix).x;
    }
    draw->AddText(text, IM_COL32_WHITE, label.data() + dimmed_prefix, label.data() + label.size());
    cursor.x = maximum.x + FontPx(6.0f);
}

void DrawElidedText(
    ImDrawList* draw, ImVec2 position, float maximum_x, std::string_view text, ImU32 color)
{
    if (maximum_x <= position.x)
        return;
    const ImVec2 size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::RenderTextEllipsis(draw, position,
        ImVec2(maximum_x, position.y + ImGui::GetTextLineHeight()), maximum_x,
        text.data(), text.data() + text.size(), &size);
    ImGui::PopStyleColor();
}

bool DrawTextWithin(ImDrawList* draw, ImVec2 position, float maximum_x, std::string_view text, ImU32 color)
{
    const float width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
    if (position.x + width <= maximum_x)
    {
        draw->AddText(position, color, text.data(), text.data() + text.size());
        return false;
    }
    DrawElidedText(draw, position, maximum_x, text, color);
    return true;
}

void DrawElidedBadge(
    ImDrawList* draw, ImVec2 cursor, float center_y, float maximum_x, std::string_view label, ImU32 color,
    std::size_t dimmed_prefix)
{
    if (maximum_x <= cursor.x)
        return;
    const float pad_x = FontPx(7.0f);
    const float pad_top = FontPx(3.0f);
    const float pad_bottom = pad_top;
    const float text_y = center_y - ImGui::GetTextLineHeight() * 0.5f;
    const ImVec2 minimum(cursor.x, text_y - pad_top);
    const ImVec2 maximum(maximum_x, text_y + ImGui::GetTextLineHeight() + pad_bottom);
    draw->AddRectFilled(minimum, maximum, color, FontPx(6.0f));
    ImVec2 text(minimum.x + pad_x, text_y);
    if (dimmed_prefix != 0)
    {
        if (DrawTextWithin(draw, text, maximum.x - pad_x, label.substr(0, dimmed_prefix), kBadgeTextMuted))
            return;
        text.x += ImGui::CalcTextSize(label.data(), label.data() + dimmed_prefix).x;
    }
    DrawElidedText(draw, text, maximum.x - pad_x, label.substr(dimmed_prefix), IM_COL32_WHITE);
}

bool BadgedSelectable(std::string_view label, bool selected, float height, ImU32 color,
    std::string_view suffix, bool* out_elided)
{
    const std::string id = "###" + std::string(label);
    const bool clicked = ImGui::Selectable(id.c_str(), selected, 0, ImVec2(0.0f, height));
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(
        minimum, ImVec2(minimum.x + 4.0f, maximum.y), color, 4.0f, ImDrawFlags_RoundCornersLeft);
    ImVec2 text(minimum.x + 12.0f, minimum.y + 3.0f);
    bool elided = DrawTextWithin(draw, text, maximum.x - 8.0f, label, ImGui::GetColorU32(ImGuiCol_Text));
    if (!elided && !suffix.empty())
    {
        text.x += ImGui::CalcTextSize(label.data(), label.data() + label.size()).x + 8.0f;
        elided = DrawTextWithin(draw, text, maximum.x - 8.0f, suffix, kTextMuted);
    }
    if (out_elided != nullptr)
        *out_elided = elided;
    return clicked;
}

float DrawHighlightedId(
    ImDrawList* draw, ImVec2 position, std::string_view id, std::size_t unique_length, ImU32 prefix_color)
{
    const std::size_t shown = std::min(id.size(), std::max<std::size_t>(8, unique_length));
    const std::size_t unique = std::min(shown, unique_length);
    draw->AddText(position, prefix_color, id.data(), id.data() + unique);
    position.x += ImGui::CalcTextSize(id.data(), id.data() + unique).x;
    draw->AddText(position, kTextMuted, id.data() + unique, id.data() + shown);
    return position.x + ImGui::CalcTextSize(id.data() + unique, id.data() + shown).x;
}

bool DrawHighlightedIdWithin(ImDrawList* draw, ImVec2 position, float maximum_x, std::string_view id,
    std::size_t unique_length, ImU32 prefix_color)
{
    const std::size_t shown = std::min(id.size(), std::max<std::size_t>(8, unique_length));
    if (position.x + ImGui::CalcTextSize(id.data(), id.data() + shown).x <= maximum_x)
    {
        DrawHighlightedId(draw, position, id, unique_length, prefix_color);
        return false;
    }
    DrawElidedText(draw, position, maximum_x, std::string_view(id.data(), shown), prefix_color);
    return true;
}

void TextHighlightedId(std::string_view id, std::size_t unique_length, ImU32 prefix_color)
{
    const std::size_t shown = std::min(id.size(), std::max<std::size_t>(8, unique_length));
    const std::size_t unique = std::min(shown, unique_length);
    ImGui::PushStyleColor(ImGuiCol_Text, prefix_color);
    ImGui::TextUnformatted(id.data(), id.data() + unique);
    ImGui::PopStyleColor();
    if (unique != shown)
    {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled("%.*s", static_cast<int>(shown - unique), id.data() + unique);
    }
}

void TextLabelledId(std::string_view label, std::string_view id, std::size_t unique_length, ImU32 prefix_color)
{
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    ImGui::SameLine(0.0f, 0.0f);
    TextHighlightedId(id, unique_length, prefix_color);
}

void DialogInput(const char* label, const char* hint, std::string* value, bool focus,
    ImGuiInputTextFlags flags)
{
    ImGui::TextUnformatted(label);
    if (focus)
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    const std::string id = std::string("###") + label;
    ImGui::InputTextWithHint(id.c_str(), hint, value, flags);
}

void DialogMultiline(const char* label, std::string* value, float height, bool focus)
{
    ImGui::TextUnformatted(label);
    if (focus)
        ImGui::SetKeyboardFocusHere();
    const std::string id = std::string("###") + label;
    ImGui::InputTextMultiline(id.c_str(), value, ImVec2(-1.0f, height));
}

ImU32 CommitIdColor(bool working)
{
    return working ? kWorkingCommitId : kCommitId;
}

} // namespace Ggui::ApplicationInternal

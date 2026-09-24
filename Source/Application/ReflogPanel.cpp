// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include "ApplicationInternal.hpp"

#include <IconsMaterialSymbols.h>
#include <imgui_stdlib.h>

#include <string>
#include <string_view>

namespace Ggui
{
using namespace ApplicationInternal;

namespace
{

std::string ReflogHash(const std::string& value)
{
    return value.empty() ? "(none)" : ShortId(value);
}

bool Matches(const ReflogEntry& entry, std::string_view filter)
{
    return filter.empty() || ContainsInsensitive(entry.message, filter)
        || ContainsInsensitive(entry.author, filter)
        || ContainsInsensitive(entry.author_email, filter)
        || ContainsInsensitive(entry.old_hash, filter)
        || ContainsInsensitive(entry.new_hash, filter);
}

} // namespace

void Application::RenderReflog()
{
    if (!_show_reflog || _snapshot == nullptr)
        return;
    if (!ImGui::Begin("Reflog", &_show_reflog))
    {
        ImGui::End();
        return;
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##reflog filter", "Filter HEAD reflog", &_reflog_filter);
    if (_snapshot->reflog.empty())
    {
        ImGui::TextDisabled("HEAD has no reflog entries.");
        ImGui::End();
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg
        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("reflog entries", 5, flags))
    {
        ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthFixed, FontPx(68.0f));
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, FontPx(138.0f));
        ImGui::TableSetupColumn("Old", ImGuiTableColumnFlags_WidthFixed, FontPx(86.0f));
        ImGui::TableSetupColumn("New", ImGuiTableColumnFlags_WidthFixed, FontPx(86.0f));
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        std::size_t visible = 0;
        for (const ReflogEntry& entry : _snapshot->reflog)
        {
            if (!Matches(entry, _reflog_filter))
                continue;
            ++visible;
            ImGui::PushID(static_cast<int>(entry.index));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("HEAD@{%zu}", entry.index);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(entry.timestamp == 0 ? "(unknown)" : FormatTimestamp(entry.timestamp).c_str());
            if (ImGui::IsItemHovered() && !entry.author.empty())
                ImGui::SetTooltip("%s%s%s", entry.author.c_str(), entry.author_email.empty() ? "" : " <",
                    entry.author_email.empty() ? "" : (entry.author_email + ">").c_str());

            ImGui::TableSetColumnIndex(2);
            const std::string old_hash = ReflogHash(entry.old_hash);
            if (entry.old_commit_available && !entry.old_hash.empty())
            {
                if (ImGui::SmallButton(old_hash.c_str()))
                    RevealRevision(entry.old_hash);
            }
            else
                ImGui::TextDisabled("%s", old_hash.c_str());
            if (ImGui::IsItemHovered() && !entry.old_hash.empty())
                ImGui::SetTooltip("%s", entry.old_commit_available ? "Reveal old HEAD" : "Object is unavailable");
            if (ImGui::BeginPopupContextItem("reflog old context"))
            {
                if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy old commit ID", nullptr, !entry.old_hash.empty()))
                    ImGui::SetClipboardText(entry.old_hash.c_str());
                if (ActionMenuItem(ICON_MS_OPEN_IN_NEW, "Reveal old commit", nullptr,
                        entry.old_commit_available && !entry.old_hash.empty()))
                    RevealRevision(entry.old_hash);
                if (ActionMenuItem(ICON_MS_BOOKMARK_ADD, "Create branch from old commit...", nullptr,
                        entry.old_commit_available && !entry.old_hash.empty()))
                {
                    OpenDialog(Dialog::Branch);
                    _input_secondary = entry.old_hash;
                }
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(3);
            const std::string new_hash = ReflogHash(entry.new_hash);
            if (entry.new_commit_available && !entry.new_hash.empty())
            {
                if (ImGui::SmallButton(new_hash.c_str()))
                    RevealRevision(entry.new_hash);
            }
            else
                ImGui::TextDisabled("%s", new_hash.c_str());
            if (ImGui::IsItemHovered() && !entry.new_hash.empty())
                ImGui::SetTooltip("%s", entry.new_commit_available ? "Reveal new HEAD" : "Object is unavailable");
            if (ImGui::BeginPopupContextItem("reflog new context"))
            {
                if (ActionMenuItem(ICON_MS_CONTENT_COPY, "Copy new commit ID", nullptr, !entry.new_hash.empty()))
                    ImGui::SetClipboardText(entry.new_hash.c_str());
                if (ActionMenuItem(ICON_MS_OPEN_IN_NEW, "Reveal new commit", nullptr,
                        entry.new_commit_available && !entry.new_hash.empty()))
                    RevealRevision(entry.new_hash);
                if (ActionMenuItem(ICON_MS_BOOKMARK_ADD, "Create branch from new commit...", nullptr,
                        entry.new_commit_available && !entry.new_hash.empty()))
                {
                    OpenDialog(Dialog::Branch);
                    _input_secondary = entry.new_hash;
                }
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(4);
            if (entry.message.empty())
                ImGui::TextDisabled("(no message)");
            else
                ImGui::TextUnformatted(FirstLine(entry.message).c_str());
            if (ImGui::IsItemHovered() && !entry.message.empty())
                ImGui::SetTooltip("%s", entry.message.c_str());
            ImGui::PopID();
        }
        if (visible == 0)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("No matching reflog entries.");
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace Ggui

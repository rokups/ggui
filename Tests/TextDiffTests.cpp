// Copyright (c) 2026-2026 the ggui project.
// SPDX-License-Identifier: GPL-2.0-only
#include <TextDiff.h>
#include <gtest/gtest.h>

namespace Ggui
{
namespace
{

TEST(TextDiff, ReplacementLinesShareRowsAndKeepUnifiedMapping)
{
    TextDiff diff;
    diff.SetText("before\nold one\nold two\nafter", "before\nnew one\nnew two\nafter");
    const auto& rows = diff.GetSideBySideRows();
    ASSERT_EQ(rows.size(), 4U);
    EXPECT_EQ(rows[1].leftLine, 1);
    EXPECT_EQ(rows[1].rightLine, 1);
    EXPECT_EQ(rows[2].leftLine, 2);
    EXPECT_EQ(rows[2].rightLine, 2);
    EXPECT_EQ(rows[1].changeBegin, rows[2].changeBegin);
    EXPECT_EQ(rows[1].changeEnd - rows[1].changeBegin, 4);
    EXPECT_EQ(diff.GetSideBySideRow(rows[1].leftUnifiedLine), 1);
    EXPECT_EQ(diff.GetSideBySideRow(rows[1].rightUnifiedLine), 1);
    EXPECT_EQ(diff.GetSideBySideRow(5), 3);
    EXPECT_EQ(diff.GetSideBySideRow(-1), -1);
    EXPECT_EQ(diff.GetSideBySideRow(6), -1);
}

TEST(TextDiff, UnequalReplacementPadsOnlyShorterSide)
{
    TextDiff diff;
    diff.SetText("before\nold\nafter", "before\nnew one\nnew two\nnew three\nafter");
    const auto& rows = diff.GetSideBySideRows();
    ASSERT_EQ(rows.size(), 5U);
    EXPECT_EQ(rows[1].leftLine, 1);
    EXPECT_EQ(rows[1].rightLine, 1);
    EXPECT_EQ(rows[2].leftLine, -1);
    EXPECT_EQ(rows[2].rightLine, 2);
    EXPECT_EQ(rows[3].leftUnifiedLine, -1);
    EXPECT_EQ(rows[3].rightLine, 3);
    EXPECT_EQ(rows[4].leftLine, 2);
    EXPECT_EQ(rows[4].rightLine, 4);
}

TEST(TextDiff, DeletionAndAdditionKeepContextAligned)
{
    TextDiff diff;
    diff.SetText("before\nremoved\nmiddle\nafter", "before\nmiddle\ninserted\nafter");
    const auto& rows = diff.GetSideBySideRows();
    ASSERT_EQ(rows.size(), 5U);
    EXPECT_EQ(rows[1].leftLine, 1);
    EXPECT_EQ(rows[1].rightLine, -1);
    EXPECT_EQ(rows[2].leftLine, 2);
    EXPECT_EQ(rows[2].rightLine, 1);
    EXPECT_EQ(rows[3].leftLine, -1);
    EXPECT_EQ(rows[3].rightLine, 2);
    EXPECT_NE(rows[1].changeBegin, rows[3].changeBegin);
}

TEST(TextDiff, MoreDeletedLinesPadRightSide)
{
    TextDiff diff;
    diff.SetText("one\ntwo\nthree\nend", "new\nend");
    const auto& rows = diff.GetSideBySideRows();
    ASSERT_EQ(rows.size(), 4U);
    EXPECT_EQ(rows[0].rightLine, 0);
    EXPECT_EQ(rows[1].rightLine, -1);
    EXPECT_EQ(rows[2].rightLine, -1);
    EXPECT_EQ(rows[3].rightLine, 1);
}

TEST(TextDiff, EmptyAndUnterminatedDocumentsHaveValidRows)
{
    TextDiff diff;
    diff.SetText("", "");
    ASSERT_EQ(diff.GetSideBySideRows().size(), 1U);
    EXPECT_EQ(diff.GetSideBySideRows()[0].changeBegin, -1);
    diff.SetText("old", "");
    ASSERT_EQ(diff.GetSideBySideRows().size(), 1U);
    EXPECT_EQ(diff.GetSideBySideRows()[0].leftLine, 0);
    EXPECT_EQ(diff.GetSideBySideRows()[0].rightLine, -1);
    diff.SetText("", "new");
    ASSERT_EQ(diff.GetSideBySideRows().size(), 1U);
    EXPECT_EQ(diff.GetSideBySideRows()[0].leftLine, -1);
    EXPECT_EQ(diff.GetSideBySideRows()[0].rightLine, 0);
    diff.SetText("old", "new");
    ASSERT_EQ(diff.GetSideBySideRows().size(), 1U);
    EXPECT_EQ(diff.GetSideBySideRows()[0].changeEnd, 2);
    diff.SetText("same\n", "same");
    ASSERT_EQ(diff.GetSideBySideRows().size(), 2U);
    EXPECT_EQ(diff.GetSideBySideRows()[1].rightLine, -1);
}

TEST(TextDiff, LineNumberMappingRejectsEqualSizeButDifferentChangeKinds)
{
    TextDiff diff;
    diff.SetText("old\nsame", "new\nsame");
    diff.SetLineNumbers({{40, 0}, {0, 50}, {41, 51}});
    EXPECT_TRUE(diff.HasMappedLineNumbers());

    diff.SetLineNumbers({{0, 50}, {40, 0}, {41, 51}});
    EXPECT_FALSE(diff.HasMappedLineNumbers());
    diff.SetLineNumbers({{40, 50}, {0, 51}, {41, 52}});
    EXPECT_FALSE(diff.HasMappedLineNumbers());
    diff.SetLineNumbers({{40, 0}, {0, 50}, {41, 0}});
    EXPECT_FALSE(diff.HasMappedLineNumbers());
    diff.SetLineNumbers({{40, 0}, {0, 50}});
    EXPECT_FALSE(diff.HasMappedLineNumbers());
}

TEST(TextDiff, CommonRowsAllowHiddenContextMarkersButRejectNegativeLineNumbers)
{
    TextDiff diff;
    diff.SetText("same\n", "same\n");
    diff.SetLineNumbers({{40, 50}, {0, 0}});
    EXPECT_TRUE(diff.HasMappedLineNumbers());
    diff.SetLineNumbers({{-1, -1}, {0, 0}});
    EXPECT_FALSE(diff.HasMappedLineNumbers());
}

class TextDiffRender : public testing::Test
{
protected:
    void SetUp() override
    {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1200, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    void TearDown() override { ImGui::DestroyContext(); }

    void Render(TextDiff& diff)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1000, 600));
        ImGui::Begin("Diff test");
        diff.Render("diff", ImVec2(900, 500));
        ImGui::End();
        ImGui::Render();
    }
};

TEST_F(TextDiffRender, ModeSwitchPreservesUnifiedRowsAndSelection)
{
    TextDiff diff;
    diff.SetText("before\nold\nafter", "before\nnew\nafter");
    Render(diff);
    EXPECT_EQ(diff.GetText(), "before\nold\nnew\nafter");
    diff.SelectRegion(1, 0, 3, 0);
    EXPECT_TRUE(diff.AnyCursorHasSelection());
    diff.SetSideBySideMode(true);
    Render(diff);
    EXPECT_FALSE(diff.AnyCursorHasSelection());
    diff.SetSideBySideMode(false);
    Render(diff);
    EXPECT_TRUE(diff.AnyCursorHasSelection());
    EXPECT_EQ(diff.GetMainCursorSelection().start.line, 1);
    EXPECT_EQ(diff.GetMainCursorSelection().end.line, 3);
    EXPECT_EQ(diff.GetText(), "before\nold\nnew\nafter");
}

TEST_F(TextDiffRender, ControlsAppearOncePerChangeBlock)
{
    TextDiff diff;
    diff.SetText("old one\nold two\ncommon\nremoved", "new\ncommon\nadded");
    diff.SetSideBySideMode(true);
    std::vector<std::pair<int, int>> controls;
    diff.SetChangeControlsCallback([&controls](int first, int end, const ImVec2&, float) { controls.emplace_back(first, end); });
    Render(diff);
    ASSERT_EQ(controls.size(), 2U);
    EXPECT_EQ(controls[0], (std::pair{0, 3}));
    EXPECT_EQ(controls[1], (std::pair{4, 6}));
}

TEST_F(TextDiffRender, ControlsSitInsideMiddleLineNumberGutterWithoutReservingWidth)
{
    TextDiff diff;
    diff.SetText("old", "new");
    diff.SetSideBySideMode(true);
    ImVec2 controls_position;
    ImVec2 controls_size;
    float right_control_x = 0.0f;
    for (const int number : {9, 99, 9999})
    {
        diff.SetLineNumbers({{number, 0}, {0, number}});
        diff.SetChangeControlsCallback({});
        Render(diff);
        const float original_split = diff.GetSideBySideSplitX();
        diff.SetChangeControlsCallback([&](int, int, const ImVec2& size, float right_x) {
            controls_position = ImGui::GetCursorScreenPos();
            controls_size = size;
            right_control_x = right_x;
        });
        Render(diff);
        EXPECT_FLOAT_EQ(diff.GetSideBySideSplitX(), original_split);
        EXPECT_NEAR(controls_position.x, original_split, 0.01f);
        const float gutter_width = (std::to_string(number).size() + 4) * ImGui::CalcTextSize("#").x;
        EXPECT_NEAR(right_control_x + controls_size.x, original_split + gutter_width, 0.01f);
        EXPECT_GT(right_control_x, controls_position.x + controls_size.x);
        EXPECT_NEAR(controls_size.x, ImGui::CalcTextSize("#").x * 0.75f, 0.01f);
        EXPECT_NEAR(controls_size.y, ImGui::GetTextLineHeightWithSpacing(), 0.01f);
    }
}

TEST_F(TextDiffRender, EmptyDocumentsRenderInSideBySideMode)
{
    TextDiff diff;
    diff.SetText("", "");
    diff.SetSideBySideMode(true);
    Render(diff);
    ASSERT_EQ(diff.GetSideBySideRows().size(), 1U);
    EXPECT_EQ(diff.GetSideBySideRows()[0].leftLine, 0);
    EXPECT_EQ(diff.GetSideBySideRows()[0].rightLine, 0);
}

TEST_F(TextDiffRender, DraggingFromEitherGutterControlDoesNotStartTextSelection)
{
    TextDiff diff;
    diff.SetText("old text\nlast line", "new text\nlast line");
    diff.SetSideBySideMode(true);
    ImVec2 control_centers[2];
    diff.SetChangeControlsCallback([&](int, int, const ImVec2& size, float right_x) {
        const ImVec2 position = ImGui::GetCursorScreenPos();
        control_centers[0] = ImVec2(position.x + size.x * 0.5f, position.y + size.y * 0.5f);
        control_centers[1] = ImVec2(right_x + size.x * 0.5f, position.y + size.y * 0.5f);
        ImGui::InvisibleButton("parent", size);
        ImGui::SetCursorScreenPos(ImVec2(right_x, position.y));
        ImGui::InvisibleButton("child", size);
    });
    Render(diff);
    Render(diff);
    ImGuiIO& io = ImGui::GetIO();
    for (const ImVec2& center : control_centers)
    {
        io.AddMousePosEvent(center.x, center.y);
        Render(diff);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        Render(diff);
        io.AddMousePosEvent(center.x - 100.0f, center.y + 25.0f);
        Render(diff);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        Render(diff);
        EXPECT_FALSE(diff.AnyCursorHasSelection());
    }

    // The same gesture in text still selects across lines.
    const ImVec2 control_center = control_centers[0];
    io.AddMousePosEvent(control_center.x - 100.0f, control_center.y);
    Render(diff);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    Render(diff);
    io.AddMousePosEvent(control_center.x - 110.0f, control_center.y + 25.0f);
    Render(diff);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    Render(diff);
    EXPECT_TRUE(diff.AnyCursorHasSelection());
}

} // namespace
} // namespace Ggui

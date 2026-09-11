#include "mangrove/gui/GuiMenu.h"

#include "mangrove/core/Feature.h"
#include "mangrove/gui/GuiOverlay.h"
#include "mangrove/gui/GuiTheme.h"
#include "mangrove/gui/GuiWidgets.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>

namespace mangrove::gui {
namespace {

using ll::i18n_literals::operator""_tr;

/// 一列的最大宽度：屏幕再宽，内容也不会被拉成一条长线
constexpr float kMaxColumnWidth = 720.0f;

/// 整列的宽度 = min(屏幕可用宽度, kMaxColumnWidth)
float columnWidth() {
    auto const usable = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x * 2.0f;
    return std::max(1.0f, std::min(usable, kMaxColumnWidth));
}

/// 整列在窗口里的左边界。屏幕比上限宽时整列居中，否则贴着左边。
float columnLeft() {
    auto const usable = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x * 2.0f;
    return ImGui::GetStyle().WindowPadding.x + std::max(0.0f, (usable - columnWidth()) * 0.5f);
}

/// 顶部标题 + 右上角关闭按钮（标题左对齐，按钮右对齐到列的右边界）
/// @returns 是否请求关闭菜单
bool drawHeader() {
    auto const title      = "mangrove.gui.title"_tr();
    auto const closeLabel = "mangrove.gui.close"_tr();

    beginRow();
    ImGui::TextUnformatted(title.c_str());

    auto const closeWidth = ImGui::CalcTextSize(closeLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(rowRight() - closeWidth);
    return ImGui::Button(closeLabel.c_str());
}

} // namespace

void renderMenu() {
    auto const viewport = ImGui::GetIO().DisplaySize;
    if (viewport.x <= 0.0f || viewport.y <= 0.0f) return;

    // 以 900 逻辑高度为基准推算缩放，1080p / 4K / 小窗口下都保持可读
    auto const metrics = calculateMetrics(viewport, std::max(1.0f, viewport.y / 900.0f));
    applyTheme(metrics);

    constexpr ImGuiWindowFlags kWindowFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport, ImGuiCond_Always);

    auto const togglesTab = "mangrove.gui.page.toggles"_tr();
    auto const slidersTab = "mangrove.gui.page.sliders"_tr();
    auto const keysTab    = "mangrove.gui.page.keys"_tr();

    bool closeRequested = false;
    if (ImGui::Begin("##mangroveMenu", nullptr, kWindowFlags)) {
        // 所有内容都收在一列里，行边界由 GuiWidgets 的 rowLeft / rowRight 决定
        ImGui::SetCursorPosX(columnLeft());
        if (ImGui::BeginChild("##mangroveColumn", ImVec2(columnWidth(), 0.0f))) {
            closeRequested = drawHeader();
            ImGui::Separator();

            if (ImGui::BeginTabBar("##mangrovePages")) {
                if (ImGui::BeginTabItem(togglesTab.c_str())) {
                    setActivePage(MenuPage::Toggles);
                    core::FeatureManager::getInstance().addToMenu();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(slidersTab.c_str())) {
                    setActivePage(MenuPage::Sliders);
                    core::FeatureManager::getInstance().addToMenu();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(keysTab.c_str())) {
                    setActivePage(MenuPage::Keys);

                    // 菜单自己的开关热键（它不走 KeyInputManager，见 GuiOverlay）
                    addKeyBinder(GuiOverlay::kToggleBinding, "mangrove.gui.toggleKey"_tr());

                    core::FeatureManager::getInstance().addToMenu();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();

    if (closeRequested) GuiOverlay::getInstance().setVisible(false);
}

} // namespace mangrove::gui

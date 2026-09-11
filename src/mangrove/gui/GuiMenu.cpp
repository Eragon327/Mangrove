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

/// 顶部标题 + 右上角关闭按钮
/// @returns 是否请求关闭菜单
bool drawHeader(UiMetrics const& metrics) {
    auto const title      = "mangrove.gui.title"_tr();
    auto const closeLabel = "mangrove.gui.close"_tr();

    ImGui::TextUnformatted(title.c_str());
    auto const closeWidth = ImGui::CalcTextSize(closeLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(
        std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - closeWidth - metrics.outerPadding * 0.5f)
    );
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

    bool closeRequested = false;
    if (ImGui::Begin("##mangroveMenu", nullptr, kWindowFlags)) {
        closeRequested = drawHeader(metrics);

        // ---- 菜单自身的开关热键（它不走 KeyInputManager，见 GuiOverlay） ----
        addKeyBinder(GuiOverlay::kToggleBinding, "mangrove.gui.toggleKey"_tr());

        // ---- 各功能自己的菜单项（顺序 = ADD_FEATURE 的登记顺序） ----
        core::FeatureManager::getInstance().addToMenu();
    }
    ImGui::End();

    if (closeRequested) GuiOverlay::getInstance().setVisible(false);
}

} // namespace mangrove::gui

#include "mangrove/ui/Menu.h"

#include "mangrove/core/Feature.h"
#include "mangrove/ui/Overlay.h"
#include "mangrove/ui/Theme.h"
#include "mangrove/ui/Widgets.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>

namespace mangrove::ui::menu {
namespace {

using ll::i18n_literals::operator""_tr;

/// 单列的最大宽度：屏幕再宽，内容也不会被拉成一条长线
constexpr float kMaxColumnWidth = 720.0f;

[[nodiscard]] float usableWidth() { return ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x * 2.0f; }

[[nodiscard]] float columnWidth() { return std::max(1.0f, std::min(usableWidth(), kMaxColumnWidth)); }

/// 单列居中；屏幕不够宽时贴着左边
[[nodiscard]] float columnLeft() {
    return ImGui::GetStyle().WindowPadding.x + std::max(0.0f, (usableWidth() - columnWidth()) * 0.5f);
}

/// 顶部：标题（左对齐）+ 关闭按钮（右对齐到列右缘）
/// @returns 是否请求关闭菜单
bool drawHeader() {
    auto const title      = "mangrove.menu.title"_tr();
    auto const closeLabel = "mangrove.menu.close"_tr();

    widgets::beginRow();
    ImGui::TextUnformatted(title.c_str());

    auto const closeWidth = ImGui::CalcTextSize(closeLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(widgets::rowRight() - closeWidth);
    return ImGui::Button(closeLabel.c_str());
}

/// 功能页：一个功能一个折叠块，块内是本功能声明的设置项。
/// 功能本身不写任何界面代码 —— 这里完全按 `Setting::kind()` 通用渲染。
void drawFeatures() {
    auto const& features = core::FeatureManager::getInstance().features();
    if (features.empty()) {
        ImGui::TextDisabled("%s", "mangrove.menu.noFeatures"_tr().c_str());
        return;
    }

    for (auto* feature : features) {
        if (!feature) continue;

        auto const name = feature->displayName();
        if (!ImGui::CollapsingHeader(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;

        ImGui::Indent();
        for (auto* setting : feature->settings()) {
            if (setting) widgets::settingRow(*feature, *setting);
        }
        ImGui::Unindent();
        ImGui::Spacing();
    }
}

/// 快捷键页：菜单开关 + 所有声明了热键的功能
void drawHotkeys() {
    ImGui::TextDisabled("%s", "mangrove.menu.hotkeysHint"_tr().c_str());
    ImGui::Spacing();

    widgets::keyRow(Overlay::kMenuToggleBinding, "mangrove.menu.binding.menuToggle"_tr());

    for (auto* feature : core::FeatureManager::getInstance().features()) {
        if (feature && feature->defaultHotkey()) widgets::keyRow(feature->bindingName(), feature->displayName());
    }
}

} // namespace

void render() {
    auto const viewport = ImGui::GetIO().DisplaySize;
    if (viewport.x <= 0.0f || viewport.y <= 0.0f) return;

    // 以 900 逻辑高度为基准推算缩放，1080p / 4K / 小窗口下都保持可读
    theme::apply(viewport);

    constexpr ImGuiWindowFlags kWindowFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport, ImGuiCond_Always);

    bool closeRequested = false;
    if (ImGui::Begin("##mangroveMenu", nullptr, kWindowFlags)) {
        ImGui::SetCursorPosX(columnLeft());
        if (ImGui::BeginChild("##mangroveColumn", ImVec2(columnWidth(), 0.0f))) {
            closeRequested = drawHeader();
            ImGui::Separator();

            if (ImGui::BeginTabBar("##mangroveTabs")) {
                if (ImGui::BeginTabItem("mangrove.menu.tab.features"_tr().c_str())) {
                    drawFeatures();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("mangrove.menu.tab.hotkeys"_tr().c_str())) {
                    drawHotkeys();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();

    if (closeRequested) Overlay::getInstance().setVisible(false);
}

} // namespace mangrove::ui::menu

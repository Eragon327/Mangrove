#include "mangrove/ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace mangrove::ui {
namespace {

/// 菜单密度：字体图集按 2 倍字号构建，这里把逻辑字号再放大一点，
/// 让控件和命中区域更舒服。
constexpr float kDensity = 1.1f;

ImVec4 rgba(float r, float g, float b, float a = 1.0f) { return {r, g, b, a}; }

} // namespace

UiMetrics calculateMetrics(ImVec2 viewport, float uiScale) {
    UiMetrics metrics;
    metrics.viewport       = viewport;
    metrics.scale          = std::clamp(uiScale, 0.75f, 4.0f);
    metrics.gap            = 8.0f * metrics.scale;
    metrics.outerPadding   = 16.0f * metrics.scale;
    metrics.sectionPadding = 12.0f * metrics.scale;
    metrics.rounding       = 4.0f * metrics.scale;
    return metrics;
}

} // namespace mangrove::ui

namespace mangrove::ui::theme {
namespace {

bool       gBaseStyleReady{};
ImGuiStyle gBaseStyle{};
float      gLastScale{-1.0f};
ImVec2     gLastViewport{-1.0f, -1.0f};

} // namespace

void loadFonts() {
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();

    // 中文环境优先用微软雅黑；按 2 倍字号构建，渲染时再缩一半，边缘更干净
    constexpr char const* kChineseFont = "C:\\Windows\\Fonts\\msyh.ttc";
    if (std::filesystem::exists(kChineseFont)) {
        ImFontConfig config{};
        config.OversampleH = 2;
        config.OversampleV = 2;
        if (io.Fonts->AddFontFromFileTTF(kChineseFont, 32.0f, &config, io.Fonts->GetGlyphRangesChineseFull())) return;
    }
    io.Fonts->AddFontDefault();
}

void apply(UiMetrics const& metrics) {
    if (!gBaseStyleReady) {
        // ImGui 的默认样式里 `FramePadding` 之类会被就地缩放，所以基准样式只存一次
        gBaseStyle      = ImGui::GetStyle();
        gBaseStyleReady = true;
    }

    auto const viewportChanged =
        std::abs(gLastViewport.x - metrics.viewport.x) > 0.5f || std::abs(gLastViewport.y - metrics.viewport.y) > 0.5f;
    if (std::abs(gLastScale - metrics.scale) < 0.001f && !viewportChanged) return;

    auto style = gBaseStyle;
    style.ScaleAllSizes(metrics.scale * kDensity);
    style.WindowPadding     = {metrics.outerPadding, metrics.outerPadding * 0.75f};
    style.FramePadding      = {metrics.sectionPadding * 0.8f, metrics.sectionPadding * 0.42f};
    style.ItemSpacing       = {metrics.gap, metrics.gap};
    style.WindowRounding    = 0.0f; // 全屏窗口不需要圆角
    style.ChildRounding     = 0.0f;
    style.FrameRounding     = metrics.rounding;
    style.PopupRounding     = metrics.rounding;
    style.GrabRounding      = metrics.rounding;
    style.ScrollbarRounding = metrics.rounding;
    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 0.0f;
    style.FrameBorderSize   = 0.0f;

    auto& colors                          = style.Colors;
    colors[ImGuiCol_Text]                 = rgba(0.96f, 0.96f, 0.96f);
    colors[ImGuiCol_TextDisabled]         = rgba(0.58f, 0.58f, 0.60f);
    colors[ImGuiCol_WindowBg]             = rgba(0.075f, 0.075f, 0.075f);
    colors[ImGuiCol_ChildBg]              = rgba(0.075f, 0.075f, 0.075f);
    colors[ImGuiCol_PopupBg]              = rgba(0.115f, 0.115f, 0.115f);
    colors[ImGuiCol_TitleBg]              = rgba(0.115f, 0.115f, 0.115f);
    colors[ImGuiCol_TitleBgActive]        = rgba(0.115f, 0.115f, 0.115f);
    colors[ImGuiCol_TitleBgCollapsed]     = rgba(0.115f, 0.115f, 0.115f);
    colors[ImGuiCol_Border]               = rgba(0.24f, 0.24f, 0.25f);
    colors[ImGuiCol_Separator]            = rgba(0.18f, 0.18f, 0.19f);
    colors[ImGuiCol_FrameBg]              = rgba(0.145f, 0.145f, 0.145f);
    colors[ImGuiCol_FrameBgHovered]       = rgba(0.185f, 0.185f, 0.185f);
    colors[ImGuiCol_FrameBgActive]        = rgba(0.22f, 0.22f, 0.22f);
    colors[ImGuiCol_Button]               = rgba(0.16f, 0.16f, 0.16f);
    colors[ImGuiCol_ButtonHovered]        = rgba(0.21f, 0.21f, 0.21f);
    colors[ImGuiCol_ButtonActive]         = rgba(0.26f, 0.26f, 0.26f);
    colors[ImGuiCol_Header]               = rgba(0.16f, 0.16f, 0.16f);
    colors[ImGuiCol_HeaderHovered]        = rgba(0.20f, 0.20f, 0.20f);
    colors[ImGuiCol_HeaderActive]         = rgba(0.16f, 0.16f, 0.16f);
    colors[ImGuiCol_CheckMark]            = rgba(0.00f, 0.47f, 0.84f);
    colors[ImGuiCol_SliderGrab]           = rgba(0.00f, 0.47f, 0.84f);
    colors[ImGuiCol_SliderGrabActive]     = rgba(0.18f, 0.62f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]          = rgba(0.10f, 0.10f, 0.10f);
    colors[ImGuiCol_ScrollbarGrab]        = rgba(0.30f, 0.30f, 0.31f);
    colors[ImGuiCol_ScrollbarGrabHovered] = rgba(0.38f, 0.38f, 0.39f);
    colors[ImGuiCol_ScrollbarGrabActive]  = rgba(0.45f, 0.45f, 0.46f);
    colors[ImGuiCol_ModalWindowDimBg]     = rgba(0.0f, 0.0f, 0.0f, 0.62f);

    ImGui::GetStyle() = style;
    // 图集是 2 倍字号，渲染到一半即「逻辑字号」
    ImGui::GetIO().FontGlobalScale = metrics.scale * 0.5f * kDensity;

    gLastScale    = metrics.scale;
    gLastViewport = metrics.viewport;
}

void apply(ImVec2 viewport) {
    if (viewport.x <= 0.0f || viewport.y <= 0.0f) return;
    // 以 900 逻辑高度为基准推算缩放，1080p / 4K / 小窗口下都保持可读
    apply(calculateMetrics(viewport, std::max(1.0f, viewport.y / 900.0f)));
}

float scale() { return gLastScale > 0.0f ? gLastScale : 1.0f; }

void reset() {
    gBaseStyleReady = false;
    gLastScale      = -1.0f;
    gLastViewport   = {-1.0f, -1.0f};
    if (ImGui::GetCurrentContext()) ImGui::GetIO().FontGlobalScale = 1.0f;
}

} // namespace mangrove::ui::theme

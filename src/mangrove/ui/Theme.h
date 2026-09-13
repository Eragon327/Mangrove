#pragma once

#include <imgui.h>

namespace mangrove::ui {

/// 界面尺寸。按视口高度算缩放，1080p / 4K / 小窗口下都能看清。
struct UiMetrics {
    ImVec2 viewport;
    float  scale;
    float  gap;
    float  outerPadding;
    float  sectionPadding;
    float  rounding;
};

[[nodiscard]] UiMetrics calculateMetrics(ImVec2 viewport, float uiScale);

} // namespace mangrove::ui

namespace mangrove::ui::theme {

/// 装载字体。
/// @note ImGui 内置字体没有 CJK 字形，中文会整片变成问号，所以优先换成系统字体。
void loadFonts();

/// 套用配色与尺寸。视口与缩放都没变时是空操作。
void apply(UiMetrics const& metrics);

/// 按视口尺寸套用（缩放按 900 逻辑高度推算）。
/// @note 菜单与提示条都走这一个入口 —— 提示条在菜单关着的时候也要画，
///       所以调用点必须在「下帧之后、任何窗口之前」。
void apply(ImVec2 viewport);

/// 丢弃缓存的基准样式（ImGui 上下文销毁前调用）
void reset();

} // namespace mangrove::ui::theme

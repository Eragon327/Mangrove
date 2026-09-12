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

/// 丢弃缓存的基准样式（ImGui 上下文销毁前调用）
void reset();

} // namespace mangrove::ui::theme

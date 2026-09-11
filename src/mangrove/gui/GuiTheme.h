#pragma once

#include "imgui.h"

namespace mangrove::gui {

/// 界面度量：随视口尺寸与 uiScale 计算，所有尺寸都是"最终像素"。
struct UiMetrics {
    ImVec2 viewport{};
    float  scale{1.0f};
    float  gap{8.0f};
    float  outerPadding{16.0f};
    float  sectionPadding{12.0f};
    float  rounding{4.0f};
};

/// 由视口尺寸与 uiScale 计算度量。
[[nodiscard]] UiMetrics calculateMetrics(ImVec2 viewport, float uiScale);

/// 应用简洁的深色主题。
/// @note 只在 uiScale / 视口变化时重建样式，其余情况直接返回，避免每帧覆盖用户样式。
void applyTheme(UiMetrics const& metrics);

/// 还原基础样式，在销毁 ImGui 上下文前调用。
void resetTheme();

} // namespace mangrove::gui

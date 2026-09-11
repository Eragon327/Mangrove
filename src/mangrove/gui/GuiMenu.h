#pragma once

namespace mangrove::gui {

/// 绘制整个菜单：标题 / 关闭按钮 → 全局设置 → 各功能自己的菜单项。
/// @note 必须在 `ImGui::NewFrame()` 之后、`ImGui::Render()` 之前调用。
void renderMenu();

} // namespace mangrove::gui

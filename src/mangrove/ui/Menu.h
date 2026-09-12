#pragma once

namespace mangrove::ui::menu {

/// 画出整个菜单。
/// @note 必须在 `ImGui::NewFrame()` 之后、`ImGui::Render()` 之前调用。
void render();

} // namespace mangrove::ui::menu

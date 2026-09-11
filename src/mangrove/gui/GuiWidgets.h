#pragma once

#include <string>
#include <string_view>

namespace mangrove::gui {

/// 组合键重绑控件：左边标签，右边当前组合键。
/// 点按键区本身即进入捕获（没有单独的"重新绑定"按钮），捕获中实时预览按下的键。
/// @param bindingName 绑定的稳定标识（`GuiOverlay::kToggleBinding` 或 KeyInputManager 里的绑定名）
/// @param label       左侧标签（已翻译）
void addKeyBinder(std::string_view bindingName, std::string const& label);

/// 复选框。
/// @returns 用户是否改动了 @p value
bool addToggle(std::string const& label, bool& value);

/// 滑块。
/// @returns 用户是否改动了 @p value
bool addSlider(std::string const& label, float& value, float min, float max, char const* format = "%.2f");

/// 普通文字。
void addText(std::string const& text);

/// 灰字提示。
void addHint(std::string const& text);

/// 分组标题（分隔线 + 标题），框架在每个功能的菜单项前调用。
void addSectionHeader(std::string const& text);

} // namespace mangrove::gui

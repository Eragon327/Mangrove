#pragma once

#include "mangrove/input/KeyBind.h"

#include <string>
#include <string_view>

namespace mangrove::core {
class Feature;
class Setting;
} // namespace mangrove::core

namespace mangrove::ui::widgets {

/// 记下当前这一行的左右边界（左边界 = 当前光标 X，右边界 = 内容区右缘）。
/// 行内所有控件都靠 `rowRight()` 右对齐，标签左对齐。
void beginRow();

/// 当前行的右边界
[[nodiscard]] float rowRight();

/// 一行「左侧标签 + 右侧控件」，控件形态由 `setting.kind()` 决定。
/// 值一变就直接落到 `Setting` 上，并通知框架持久化 —— 功能不需要参与。
void settingRow(core::Feature& feature, core::Setting& setting);

/// 一行快捷键：左侧标签，右侧是当前键位 + 「重置」。
/// 点键位进入捕获：按下并松开一组键即绑定，**按 Esc 则解绑成空**。
/// @param defaultKeys 代码里的默认键位 —— 「重置」的落点，也是按钮置灰的判据
void keyRow(std::string_view bindingName, std::string const& label, input::KeyBind const& defaultKeys);

/// 清掉控件层的临时视图状态（数值控件当前是滑条还是输入框）。
/// 这是视图状态、不是设置，所以菜单关闭时由 `ui::Overlay` 调它复位。
void resetTransientState();

} // namespace mangrove::ui::widgets

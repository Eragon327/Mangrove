#pragma once

#include <string>
#include <string_view>

namespace mangrove::gui {

/// 菜单页。每一类控件只画在自己那一页上，见 addToggle / addSlider / addKeyBinder。
enum class MenuPage {
    Toggles, ///< 开关
    Sliders, ///< 滑条 / 数据配置
    Keys,    ///< 按键绑定
};

/// 当前正在画哪一页，由 GuiMenu 在每个标签页里设置
void                   setActivePage(MenuPage page);
[[nodiscard]] MenuPage activePage();
[[nodiscard]] bool     isActivePage(MenuPage page);

// ---------------------------------------------------------------------------
// 行布局：文字左对齐，控件右对齐到行的右边界
// ---------------------------------------------------------------------------

/// 在行首调用：记下这一行的左右边界（= 当前光标 X 与可用宽度）。
/// 用"可用宽度"而不是窗口宽度，是为了自动带上标签页 / 滚动条带来的缩进。
void beginRow();

/// 这一行的宽度（= rowRight() - rowLeft()）
[[nodiscard]] float rowWidth();

/// 这一行的左边界（窗口坐标）
[[nodiscard]] float rowLeft();

/// 这一行的右边界（窗口坐标）—— 控件的右边缘贴着它
[[nodiscard]] float rowRight();

// ---------------------------------------------------------------------------
// 控件
// ---------------------------------------------------------------------------

/// 组合键重绑控件（**按键页**）：左边标签，右边当前组合键。
/// 点按键区本身即进入捕获（没有单独的"重新绑定"按钮），捕获中实时预览按下的键。
/// @param bindingName 绑定的稳定标识（`GuiOverlay::kToggleBinding` 或 KeyInputManager 里的绑定名）
/// @param label       左侧标签（已翻译）
void addKeyBinder(std::string_view bindingName, std::string const& label);

/// 复选框（**开关页**）。左侧标签 + 右对齐的勾选框。
/// @note @p label 同时用作 ImGui 的控件 ID，同一页里别用重复的标签。
/// @returns 用户是否改动了 @p value
bool addToggle(std::string const& label, bool& value);

/// 滑条（**数据页**）。左侧标签 + 右对齐的滑条。
/// @note @p label 同时用作 ImGui 的控件 ID，同一页里别用重复的标签
///       （一个功能要放多个滑条时，用 `displayName() + " X"` 这种区分开）。
/// @returns 用户是否改动了 @p value
bool addSlider(std::string const& label, float& value, float min, float max, char const* format = "%.2f");

/// 普通文字。不分页 —— 当前是哪一页就画在哪一页，用于通用说明。
void addText(std::string const& text);

/// 灰字提示。同 addText。
void addHint(std::string const& text);

/// 整页标题（分隔线 + 标题），立即绘制。
/// @note 菜单默认不用分组标题 —— 功能名直接写在控件那一行上，别再加一行标题。
void addSectionHeader(std::string const& text);

} // namespace mangrove::gui

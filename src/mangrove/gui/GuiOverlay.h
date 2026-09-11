#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mangrove::gui {

/// Dear ImGui 覆盖层。
///
/// 通过钩住 `IDXGISwapChain::Present` 在游戏自己的渲染线程上叠加一层 ImGui 界面，
/// 并钩住游戏窗口过程来接管键鼠输入：
///   - 渲染：ImGui（Win32 后端 + DX11 / D3D11On12 后端）
///   - 菜单开关热键：默认 X + C，可在菜单里重绑并持久化。判定放在窗口消息层，
///     因为菜单打开时游戏输入会被拦截，`ll::event::KeyInputEvent` 收不到。
///   - 改键捕获：`beginRebind()` 之后所有键盘消息由覆盖层接管，直到候选键全部抬起
///   - 关闭：Esc，或菜单上的「关闭菜单」按钮
///
/// 不依赖 `ll::api::ui` 的 form，纯客户端窗口绘制。
class GuiOverlay {
public:
    /// 菜单开关热键的绑定名。它不走 KeyInputManager（见上），
    /// 但共用同一套改键捕获与 DataBase 持久化。
    static constexpr std::string_view kToggleBinding = "menu.toggle";

    static GuiOverlay& getInstance();

    GuiOverlay(GuiOverlay const&)            = delete;
    GuiOverlay& operator=(GuiOverlay const&) = delete;

    /// 安装 DXGI / 窗口过程钩子，并接入游戏输入拦截。重复调用安全。
    /// @returns 是否已就绪；游戏窗口尚未创建时返回 false
    bool install();

    [[nodiscard]] bool isInstalled() const;

    /// 卸载钩子、销毁 ImGui 上下文。重复调用安全。
    void uninstall();

    [[nodiscard]] bool isVisible() const;

    void setVisible(bool visible);

    void toggle();

    /// 菜单是否正在独占游戏输入（打开中，或刚关闭的输入收尾窗口内）。
    /// @note 供 MenuInputGuard 判断是否拦截游戏输入
    [[nodiscard]] bool shouldBlockGameInput() const;

    // ---------------------------------------------------------------------
    // 改键
    // ---------------------------------------------------------------------

    /// 菜单开关热键当前的组合键（Windows 虚拟键码 VK_*，按按下顺序）
    [[nodiscard]] std::vector<int> getToggleKeys() const;

    /// 设置菜单开关热键：立即生效并持久化（非法集合会被忽略）
    void setToggleKeys(std::vector<int> keys);

    /// 进入改键捕获：接下来按下的组合键会成为 @p bindingName 的新键，Esc 取消。
    /// 捕获期间键盘消息被覆盖层吃掉，不会触发开关，也不会漏进游戏。
    /// @param bindingName `kToggleBinding`，或 KeyInputManager 里的绑定名
    void beginRebind(std::string bindingName);

    /// 退出改键捕获（不修改任何绑定）
    void cancelRebind();

    [[nodiscard]] bool isRebinding() const;

    /// 当前捕获的是否就是这个绑定
    [[nodiscard]] bool isRebinding(std::string_view bindingName) const;

    /// 捕获中当前已按下的组合键，用于界面实时预览。
    /// 按下即出现（还没松开就会显示），未在捕获时返回空。
    [[nodiscard]] std::vector<int> getRebindPreview() const;

private:
    GuiOverlay() = default;
};

} // namespace mangrove::gui

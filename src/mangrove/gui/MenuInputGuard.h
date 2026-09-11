#pragma once

namespace mangrove::gui {

struct MenuInputGuardStatus {
    bool mouseInputHookInstalled{};
    bool keyDownInputHookInstalled{};
    bool keyUpInputHookInstalled{};
};

/// 在游戏输入层临时放行一段作用域。
/// GuiOverlay 需要向游戏补发"按键抬起"时用它绕过下面的拦截钩子。
class MenuInputHandoffScope final {
public:
    MenuInputHandoffScope();
    ~MenuInputHandoffScope();

    MenuInputHandoffScope(MenuInputHandoffScope const&)            = delete;
    MenuInputHandoffScope& operator=(MenuInputHandoffScope const&) = delete;
};

/// 安装游戏输入拦截钩子：菜单打开时不让游戏收到键鼠输入
/// （否则移动鼠标会转视角、点击会攻击/挖掘）。
///
/// 窗口消息层能挡掉一部分输入，但 Bedrock 自己的 HID 控制器仍能拿到，
/// 所以这里在 `MouseDevice::feed` / `HIDControllerGameCoreDesktop::onKeyDown|onKeyUp` 上再挡一层。
[[nodiscard]] MenuInputGuardStatus installMenuInputGuard();

/// 卸载输入拦截钩子（与 installMenuInputGuard 配对使用）。
void uninstallMenuInputGuard();

} // namespace mangrove::gui

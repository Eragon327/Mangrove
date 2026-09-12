#pragma once

namespace mangrove::ui {

struct InputGuardStatus {
    bool mouseHookInstalled{};
    bool keyDownHookInstalled{};
    bool keyUpHookInstalled{};
};

/// 安装游戏输入拦截钩子：菜单打开时不让游戏收到键鼠输入
/// （否则移动鼠标会转视角、点击会攻击 / 挖掘）。
///
/// 窗口消息层已经能挡掉一部分，但 Bedrock 自己的 HID 控制器仍能拿到，
/// 所以这里在 `MouseDevice::feed` / `HIDControllerGameCoreDesktop::onKeyDown|onKeyUp`
/// 上再挡一层。副作用正好是我想要的：被拦掉的按键不会发布 `KeyInputEvent`，
/// 于是菜单开着时功能热键也不会误触发。
///
/// **窗口 / 系统级按键除外**：`F11`（切全屏）在钩子这一层也放行。
/// `ui::Overlay` 在窗口过程层对同一批键也放行 —— 两条路径相互独立，
/// 游戏既可能从窗口消息拿键盘、也可能从 HID 控制器拿，只补一边等于没补。
///
/// 重复调用安全。
[[nodiscard]] InputGuardStatus installInputGuard();

/// 卸载输入拦截钩子。重复调用安全。
void uninstallInputGuard();

/// 覆盖层是否正在独占游戏输入（菜单打开中，或刚关闭的收尾窗口内）
[[nodiscard]] bool overlayOwnsGameInput();

/// 由 `ui::Overlay` 在显隐变化时调用。
/// @param capture true 表示从现在起独占；false 表示再独占一小段时间再释放，
///                这样触发键本身的抬起事件也不会漏进游戏
void setOverlayInputCapture(bool capture);

/// 临时放行一段作用域：`ui::Overlay` 向游戏补发「按键抬起」时用它绕过上面的钩子。
class InputHandoffScope {
public:
    InputHandoffScope();
    ~InputHandoffScope();

    InputHandoffScope(InputHandoffScope const&)            = delete;
    InputHandoffScope& operator=(InputHandoffScope const&) = delete;
};

} // namespace mangrove::ui

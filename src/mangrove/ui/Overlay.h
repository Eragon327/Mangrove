#pragma once

#include "mangrove/input/KeyBind.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mangrove::ui {

/// ImGui 覆盖层 —— 客户端 mod 唯一能画「常驻界面」的途径。
///
/// 客户端 mod 拿不到服务端表单（`ll::form` 和 DDUI 都要由服务端发包），所以走
/// 「钩住 DXGI Present + 游戏窗口过程」这条路：
///
///   - 渲染：ImGui（Win32 后端 + DX11 / D3D11On12 后端）
///   - 输入：菜单打开时由窗口过程接管，再配合 `InputGuard` 拦掉游戏自己收到的输入
///   - 菜单开关热键：默认 X + C。菜单打开时游戏输入被拦，`KeyInputEvent` / `MouseInputEvent`
///     都不会发布，所以这个热键只能在窗口过程层判定
///   - 改键：`beginCapture()` 之后按键与鼠标消息全被吃掉，等候选键全部抬起才提交；
///     捕获期间不会触发菜单开关，也不会漏进游戏
///
/// 键位不区分键盘和鼠标：鼠标键就编码在同一个 VK 空间里（`input::kMouseLeft` 等），
/// 所以「X + 右键」这种组合和纯键盘组合走的是完全同一条路径。
///
/// 打开方式：热键，或客户端指令 `/mangrove`。关闭方式：Esc、菜单里的关闭按钮、或再按一次热键。
class Overlay {
public:
    /// 菜单开关热键的绑定名。它不经过 `input::KeyManager`（原因见上）。
    static constexpr std::string_view kMenuToggleBinding = "menu.toggle";

    /// 改键结果回调。
    /// @param target 绑定的稳定名
    /// @param result 三态：
    ///        - 有值且非空 —— 绑到这一组键；
    ///        - **有值但为空** —— 玩家按了 Esc，**解绑**（清成空）；
    ///        - `std::nullopt` —— 捕获被**取消**（菜单关了之类），什么都别做。
    using CaptureCallback = std::function<void(std::string const& target, std::optional<input::KeyBind> result)>;

    static Overlay& getInstance();

    Overlay(Overlay const&)            = delete;
    Overlay& operator=(Overlay const&) = delete;

    /// 安装 DXGI / 窗口过程钩子。游戏窗口还没创建时返回 false，可以稍后重试。
    bool install();

    [[nodiscard]] bool isInstalled() const;

    /// 卸载钩子并销毁 ImGui 上下文。重复调用安全。
    void uninstall();

    [[nodiscard]] bool isVisible() const;
    void               setVisible(bool visible);
    void               toggle();

    // -----------------------------------------------------------------------
    // 菜单开关热键
    // -----------------------------------------------------------------------

    [[nodiscard]] input::KeyBind menuToggleKeys() const;
    void                         setMenuToggleKeys(input::KeyBind const& keys);

    /// 菜单开关热键的代码默认值（X + C）。
    /// @note 快捷键页的「重置」按钮用它判断「已经回到默认」，也是解绑后的恢复值。
    [[nodiscard]] static input::KeyBind defaultMenuToggleKeys();

    // -----------------------------------------------------------------------
    // 改键捕获
    // -----------------------------------------------------------------------

    /// 进入捕获：接下来按下的组合键会通过 @p onFinished 交给调用方去生效与持久化。
    /// 捕获期间按 Esc 提交一个**空组合** —— 那是「解绑」，不是「取消」。
    void beginCapture(std::string target, CaptureCallback onFinished);

    /// 放弃当前捕获（不修改任何绑定）
    void cancelCapture();

    /// 当前是否正在为 @p target 捕获
    [[nodiscard]] bool isCapturing(std::string_view target) const;

    /// 捕获中已按下的键（按按下顺序），用于界面实时预览；未捕获时为空
    [[nodiscard]] std::vector<int> capturePreview() const;

    // -----------------------------------------------------------------------
    // 提示
    // -----------------------------------------------------------------------

    /// 屏幕底部的短暂提示。菜单关着时也会画出来，用于热键切换后的反馈。
    /// @note 同一时间只显示一条：新提示直接顶掉上一条，不堆叠。
    void notify(std::string message);

private:
    Overlay() = default;
};

} // namespace mangrove::ui

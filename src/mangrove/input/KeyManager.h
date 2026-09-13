#pragma once

#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"

#include "mangrove/input/KeyBind.h"

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mangrove::input {

/// 世界内热键管理器。
///
/// 数据来源是 `ll::event::KeyInputEvent` 和 `ll::event::MouseInputEvent`，
/// 键盘和鼠标按键走**同一条**判定逻辑 —— 鼠标键只是编码在 VK 空间里的另外几个键，
/// 所以「X + 右键」这种组合不需要任何特殊处理。
///
/// **界面打开时一律不派发**，包括菜单开关热键（那个在窗口过程层判定，见 `ui::Overlay`）。
/// 压制有两道：
///   1. `ui::InputGuard` 把游戏输入拦掉 → LeviLamina 的事件根本不会发布（主路径）；
///   2. `setSuppressed()` 由 `ui::Overlay` 在显隐时驱动 —— 钩子万一没装上也能兜住。
///
/// 绑定名同时是持久化用的 key（`feature.<id>`），所以功能改键后无需再关心存盘。
class KeyManager {
public:
    using Handler = std::function<void()>;

    static KeyManager& getInstance();

    KeyManager(KeyManager const&)            = delete;
    KeyManager& operator=(KeyManager const&) = delete;

    /// 订阅键盘 / 鼠标事件。重复调用安全。
    void install();

    /// 取消订阅并清空所有绑定。重复调用安全。
    void uninstall();

    /// 注册热键。若配置里有改键记录，@p defaultKeys 会被玩家的设置覆盖。
    /// @param name 稳定标识，形如 `feature.FreeCamera`
    void add(std::string name, KeyBind defaultKeys, Handler handler);

    /// 改键：更新内存里的绑定并立即落盘。
    /// @returns 是否找到该绑定
    bool rebind(std::string_view name, KeyBind keys);

    [[nodiscard]] std::optional<KeyBind> keys(std::string_view name) const;

    /// 界面（菜单）打开时压制所有热键派发。
    /// @note 这是第二道保险：主路径上 `ui::InputGuard` 已经让事件不发布了。
    ///       恢复时按住状态会一并清掉，所以「菜单开着时按住的键」不会在关掉后补触发。
    void setSuppressed(bool suppressed);

    /// 界面是否正在独占输入。
    /// @note 功能层用它判断「现在输入归界面」：例如自由视角要清掉按住状态，
    ///       否则菜单打开期间松开的键会永远停在按住，关掉菜单后相机一直飞。
    [[nodiscard]] bool isSuppressed() const { return mSuppressed.load(std::memory_order_acquire); }

    /// 所有绑定（按注册顺序），供快捷键页展示
    struct Entry {
        std::string name;
        KeyBind     keys;
    };
    [[nodiscard]] std::vector<Entry> entries() const;

private:
    KeyManager() = default;

    void onKeyEvent(ll::event::KeyInputEvent& event);
    void onMouseEvent(ll::event::MouseInputEvent& event);

    /// 键盘与鼠标共用的判定入口：维护按住状态、找出命中的绑定、派发。
    /// @param virtualKey VK 键码；鼠标键用的是同一套编码
    void handlePress(int virtualKey, bool down);

    /// 清掉按住状态。界面打开 / 失焦后调用，避免残留状态让组合键错判。
    void resetHeld();

    struct Binding {
        std::string name;
        KeyBind     keys;
        /// 代码里的默认键。改回它时就不该再往配置里写记录了（只存 diff）。
        KeyBind defaultKeys;
        Handler handler;
    };

    /// 绑定表会被渲染 / 窗口线程改（改键），派发在游戏线程，所以加锁
    mutable std::mutex   mMutex;
    std::vector<Binding> mBindings;

    /// 以下只在游戏线程访问，不需要锁
    std::array<bool, kMaxVirtualKey> mHeld{};
    bool                             mDispatching{};

    /// 显隐可能来自渲染线程（菜单里点关闭）/ 窗口线程（热键）/ 游戏线程（指令），
    /// 所以它是原子的
    std::atomic_bool mSuppressed{false};

    ll::event::ListenerPtr mKeyListener;
    ll::event::ListenerPtr mMouseListener;
};

} // namespace mangrove::input

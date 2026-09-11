#pragma once

#include "ll/api/base/StdInt.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/input/KeyInputEvent.h"

#include "mangrove/input/Keys.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mangrove::input {

/// 是否为修饰键（Shift / Ctrl / Alt / Win）。组合键中修饰键只作为前置条件，不作为触发键。
/// @note 键码为 Windows 虚拟键码（VK_*），实现见 KeyInputManager.cpp
[[nodiscard]] bool isModifierKey(int vk);

/// 一个按键绑定：组合键 + 回调。
/// keys 中最后一个非修饰键为触发键，只有它在上升沿按下、且组合内所有键都处于按住状态时才触发。
struct KeyBinding {
    std::string           name;        // 稳定标识，用于改键与持久化
    std::vector<int>      keys;        // 组合内所有键（Windows 虚拟键码 VK_*）
    int                   trigger = 0; // 触发键
    std::function<void()> callback;
};

/// 功能热键管理器（事件驱动）。
///
/// 只处理「菜单关着的时候」的组合键：菜单打开时游戏输入被 MenuInputGuard 拦掉，
/// `ll::event::KeyInputEvent` 不再发布，所以菜单自己的开关热键放在 GuiOverlay 的窗口过程层判定。
class KeyInputManager {
public:
    static constexpr size_t MaxKeyCode = 256;

    KeyInputManager(KeyInputManager const&)            = delete;
    KeyInputManager& operator=(KeyInputManager const&) = delete;

    static KeyInputManager& getInstance();

    /// 注册按键绑定。注册时会套用 DataBase 里玩家改过的键，没有记录就用 @p keys。
    /// @param name     稳定标识（用于改键与持久化），如 `feature.FreeCamera`
    /// @param keys     默认组合键（Windows 虚拟键码 VK_*）
    /// @param callback 触发时调用
    /// @returns 0 表示失败，否则返回唯一 id
    uint64 add(std::string name, std::vector<int> const& keys, std::function<void()> callback);

    void remove(uint64 id);

    bool contains(uint64 id) const;

    /// 订阅键盘事件（重复调用安全），在 mod 载入时调用
    void install();

    /// 取消订阅并清空所有绑定与按键状态，在 mod 卸载时调用
    void uninstall();

    /// 改键：替换指定名字绑定的组合键（保持回调不变），并持久化到 DataBase
    /// @returns 是否找到该名字
    bool rebind(std::string_view name, std::vector<int> const& keys);

    /// 查询指定名字当前的组合键
    [[nodiscard]] std::optional<std::vector<int>> getKeys(std::string_view name) const;

    /// 列出所有绑定（名字 + 当前组合键），供 HUD / 菜单展示
    [[nodiscard]] std::vector<std::pair<std::string, std::vector<int>>> listBindings() const;

private:
    KeyInputManager() = default;

    static int  resolveTrigger(std::vector<int> const& keys);
    static bool validKeys(std::vector<int> const& keys);

    size_t heldCount() const;
    void   dispatch(KeyBinding const& binding);

    /// 事件入口：由 install() 订阅的 ll::event::KeyInputEvent 转发到这里
    void onKeyEvent(ll::event::KeyInputEvent& event);

    std::unordered_map<uint64, KeyBinding> mBindings;
    std::array<bool, MaxKeyCode>           mHeld{};
    uint64                                 mNextId      = 1;
    bool                                   mDispatching = false;

    ll::event::ListenerPtr mKeyEventListener;
};

} // namespace mangrove::input

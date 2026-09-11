#pragma once

#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/input/KeyInputEvent.h"

#include <Windows.h>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

class KeyInputManager {
public:
    static constexpr size_t MaxKeyCode = 256;

private:
    std::unordered_map<uint64, KeyBinding> mBindings;
    std::array<bool, MaxKeyCode>           mHeld{};
    uint64                                 mNextId      = 1;
    bool                                   mDispatching = false;

    ll::event::ListenerPtr mKeyEventListener;

    KeyInputManager() = default;

public:
    KeyInputManager(const KeyInputManager&)            = delete;
    KeyInputManager& operator=(const KeyInputManager&) = delete;

    static KeyInputManager& getInstance();

    /// 注册按键绑定。
    /// @param name  稳定标识（用于改键），建议与 KEY_INPUT 的 name 一致
    /// @returns 0 表示失败，否则返回唯一 id
    uint64 add(std::string name, const std::vector<int>& keys, std::function<void()> callback);

    void remove(uint64 id);

    bool contains(uint64 id) const;

    /// 初始化：订阅键盘事件（重复调用安全），在 mod 载入时调用
    void init();

    /// 清除：取消订阅并清空所有绑定与按键状态，在 mod 卸载时调用
    void clear();

    /// 改键：替换指定名字绑定的组合键（保持回调不变）
    /// @returns 是否找到该名字
    bool rebind(std::string_view name, const std::vector<int>& keys);

    /// 查询指定名字当前的组合键
    std::optional<std::vector<int>> getKeys(std::string_view name) const;

private:
    static int  resolveTrigger(const std::vector<int>& keys);
    static bool validKeys(const std::vector<int>& keys);

    size_t heldCount() const;
    void   dispatch(KeyBinding const& binding);

    /// 事件入口：由 init() 订阅的 ll::event::KeyInputEvent 转发到这里
    void onKeyEvent(ll::event::KeyInputEvent& event);
};

} // namespace mangrove::input

#define KEY_INPUT(name, ...)                                                                                             \
    struct name {                                                                                                        \
        inline static uint64 mId = 0;                                                                                    \
        static void          onKeyInput();                                                                               \
        static void          subscribe() {                                                                               \
            mId = mangrove::input::KeyInputManager::getInstance().add(#name, {__VA_ARGS__}, &name::onKeyInput); \
        }                                                                                                                \
        static void rebind(std::vector<int> const& keys) {                                                               \
            mangrove::input::KeyInputManager::getInstance().rebind(#name, keys);                                         \
        }                                                                                                                \
        static void unsubscribe() { mangrove::input::KeyInputManager::getInstance().remove(mId); }                       \
    };                                                                                                                   \
    inline void name::onKeyInput()

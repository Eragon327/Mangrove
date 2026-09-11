#include "mangrove/entry/KeyInputManager.h"
#include "mangrove/Mangrove.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/service/TargetedBedrock.h"
#include "mc/client/game/ClientInstance.h"

#include <algorithm>

namespace mangrove::input {

bool isModifierKey(int vk) {
    switch (vk) {
    case VK_SHIFT:
    case VK_CONTROL:
    case VK_MENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_LMENU:
    case VK_RMENU:
        return true;
    default:
        return false;
    }
}

KeyInputManager& KeyInputManager::getInstance() {
    static KeyInputManager instance;
    return instance;
}

int KeyInputManager::resolveTrigger(const std::vector<int>& keys) {
    // 触发键取最后一个非修饰键；若组合全是修饰键，则取最后一个
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        if (!isModifierKey(*it)) return *it;
    }
    return keys.back();
}

bool KeyInputManager::validKeys(const std::vector<int>& keys) {
    if (keys.empty()) return false;
    return std::ranges::all_of(keys, [](int key) { return key >= 0 && key < static_cast<int>(MaxKeyCode); });
}

uint64 KeyInputManager::add(std::string name, const std::vector<int>& keys, std::function<void()> callback) {
    if (!validKeys(keys) || !callback) return 0;

    uint64 id = mNextId++;
    mBindings.emplace(id, KeyBinding{std::move(name), keys, resolveTrigger(keys), std::move(callback)});
    return id;
}

void KeyInputManager::remove(uint64 id) { mBindings.erase(id); }

bool KeyInputManager::contains(uint64 id) const { return mBindings.find(id) != mBindings.end(); }

void KeyInputManager::init() {
    if (mKeyEventListener) return;

    mKeyEventListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::KeyInputEvent>(
        [](ll::event::KeyInputEvent& event) { KeyInputManager::getInstance().onKeyEvent(event); }
    );
}

void KeyInputManager::clear() {
    if (mKeyEventListener) {
        ll::event::EventBus::getInstance().removeListener(mKeyEventListener);
        mKeyEventListener.reset();
    }

    mBindings.clear();
    mHeld.fill(false);
    mNextId      = 1;
    mDispatching = false;
}

bool KeyInputManager::rebind(std::string_view name, const std::vector<int>& keys) {
    if (!validKeys(keys)) return false;

    for (auto& [id, binding] : mBindings) {
        if (binding.name == name) {
            binding.keys    = keys;
            binding.trigger = resolveTrigger(keys);
            return true;
        }
    }
    return false;
}

std::optional<std::vector<int>> KeyInputManager::getKeys(std::string_view name) const {
    for (auto const& [id, binding] : mBindings) {
        if (binding.name == name) return binding.keys;
    }
    return std::nullopt;
}

size_t KeyInputManager::heldCount() const { return static_cast<size_t>(std::ranges::count(mHeld, true)); }

void KeyInputManager::dispatch(KeyBinding const& binding) {
    if (!binding.callback) return;
    try {
        binding.callback();
    } catch (const std::exception& e) {
        Mangrove::getInstance().getSelf().getLogger().error(
            "Exception in key binding '{}': {}",
            binding.name,
            e.what()
        );
    } catch (...) {
        Mangrove::getInstance().getSelf().getLogger().error("Unknown exception in key binding '{}'", binding.name);
    }
}

void KeyInputManager::onKeyEvent(ll::event::KeyInputEvent& event) {
    int code = event.keyCode();
    if (code < 0 || code >= static_cast<int>(MaxKeyCode)) return;

    auto index = static_cast<size_t>(code);

    // 抬起：只更新「按住」状态
    if (!event.isDown()) {
        mHeld[index] = false;
        return;
    }

    mHeld[index] = true;

    // 菜单 / 聊天框等界面打开时不触发，避免打字误触
    if (auto client = ll::service::getClientInstance(); client && client->isShowingMenu()) return;

    if (mDispatching) return;

    struct DispatchGuard {
        bool& flag;
        explicit DispatchGuard(bool& f) : flag(f) { flag = true; }
        ~DispatchGuard() { flag = false; }
    } guard(mDispatching);

    // 先收集命中的绑定，避免回调内改键 / 注销导致迭代器失效
    size_t              held = heldCount();
    std::vector<uint64> hits;
    hits.reserve(mBindings.size());

    for (auto const& [id, binding] : mBindings) {
        if (binding.trigger != code) continue;     // 只在触发键按下时判定
        if (held != binding.keys.size()) continue; // 精确匹配，避免单键与组合键互相误触
        if (!std::ranges::all_of(binding.keys, [this](int key) { return mHeld[static_cast<size_t>(key)]; })) {
            continue;
        }
        hits.push_back(id);
    }

    for (uint64 id : hits) {
        if (auto it = mBindings.find(id); it != mBindings.end()) dispatch(it->second);
    }
}

} // namespace mangrove::input
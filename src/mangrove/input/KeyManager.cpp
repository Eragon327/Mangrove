#include "mangrove/input/KeyManager.h"

#include "mangrove/Mangrove.h"
#include "mangrove/core/Config.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/input/MouseAction.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace mangrove::input {
namespace {

auto& logger() { return Mangrove::getInstance().getSelf().getLogger(); }

} // namespace

KeyManager& KeyManager::getInstance() {
    static KeyManager instance;
    return instance;
}

void KeyManager::install() {
    if (mKeyListener) return;

    auto& bus      = ll::event::EventBus::getInstance();
    mKeyListener   = bus.emplaceListener<ll::event::KeyInputEvent>([](ll::event::KeyInputEvent& event) {
        KeyManager::getInstance().onKeyEvent(event);
    });
    mMouseListener = bus.emplaceListener<ll::event::MouseInputEvent>([](ll::event::MouseInputEvent& event) {
        KeyManager::getInstance().onMouseEvent(event);
    });

    if (!mKeyListener) logger().error("Failed to subscribe to key input events");
    if (!mMouseListener) logger().error("Failed to subscribe to mouse input events");
}

void KeyManager::uninstall() {
    auto& bus = ll::event::EventBus::getInstance();
    if (mKeyListener) {
        bus.removeListener(mKeyListener);
        mKeyListener.reset();
    }
    if (mMouseListener) {
        bus.removeListener(mMouseListener);
        mMouseListener.reset();
    }

    {
        std::lock_guard lock(mMutex);
        mBindings.clear();
    }
    resetHeld();
    mDispatching = false;
}

void KeyManager::add(std::string name, KeyBind defaultKeys, Handler handler) {
    // 保留一份代码默认键：改回它时要能判断出「这不算改键」
    auto const codeDefault = defaultKeys;

    // 注册时套用玩家改过的键；没有记录就沿用调用点给的默认值
    if (auto const stored = KeyBind::make(core::Config::getInstance().getKeys(name))) defaultKeys = *stored;

    std::lock_guard lock(mMutex);
    mBindings.push_back(Binding{std::move(name), std::move(defaultKeys), std::move(codeDefault), std::move(handler)});
}

bool KeyManager::rebind(std::string_view name, KeyBind keys) {
    auto const stored = keys.keys();

    bool sameAsDefault = false;
    {
        std::lock_guard lock(mMutex);
        auto const      iterator = std::ranges::find(mBindings, name, &Binding::name);
        if (iterator == mBindings.end()) return false;

        sameAsDefault  = keys.equals(iterator->defaultKeys);
        iterator->keys = std::move(keys);
    }

    // 只存 diff：改回默认键就删掉记录，让配置文件自己变干净
    core::Config::getInstance().setKeys(name, sameAsDefault ? std::vector<int>{} : stored);
    return true;
}

std::optional<KeyBind> KeyManager::keys(std::string_view name) const {
    std::lock_guard lock(mMutex);
    auto const      iterator = std::ranges::find(mBindings, name, &Binding::name);
    if (iterator == mBindings.end()) return std::nullopt;
    return iterator->keys;
}

std::vector<KeyManager::Entry> KeyManager::entries() const {
    std::lock_guard lock(mMutex);

    std::vector<Entry> result;
    result.reserve(mBindings.size());
    for (auto const& binding : mBindings) result.push_back(Entry{binding.name, binding.keys});
    return result;
}

void KeyManager::resetHeld() { mHeld.fill(false); }

void KeyManager::onKeyEvent(ll::event::KeyInputEvent& event) { handlePress(event.keyCode(), event.isDown()); }

void KeyManager::onMouseEvent(ll::event::MouseInputEvent& event) {
    // 只有按 / 抬鼠标键才参与判定；移动和滚轮返回 0
    int const code = mouseActionToKey(event.actionButtonId());
    if (code == 0) return;

    handlePress(code, event.buttonData() == MouseAction::DataDown);
}

void KeyManager::setSuppressed(bool suppressed) { mSuppressed.store(suppressed, std::memory_order_release); }

void KeyManager::handlePress(int virtualKey, bool down) {
    if (!isValidKey(virtualKey)) return;

    // 界面打开时所有热键一律不响应。
    // 主路径上事件根本走不到这里（InputGuard 拦了输入），这是钩子没装上时的保险。
    if (mSuppressed.load(std::memory_order_acquire)) {
        resetHeld();
        return;
    }

    auto const index = static_cast<size_t>(virtualKey);

    // 有界面（菜单 / 聊天 / 容器 / 表单）打开时不派发，避免打字误触；
    // 但按住状态要清掉，否则界面关掉后可能留下「一直按着」的假象
    if (auto client = ll::service::getClientInstance(); client && client->isShowingMenu()) {
        resetHeld();
        return;
    }

    if (!down) {
        mHeld[index] = false;
        return;
    }
    if (mHeld[index]) return; // 系统的自动重复
    mHeld[index] = true;

    if (mDispatching) return;

    // 同一次按下可能命中多条绑定（例如 F 与 X+F）。这里只触发「键数最多」的那条，
    // 既消解了歧义，又不会出现「按住 W 走路时按不出热键」的问题。
    std::vector<Handler> matches;
    size_t               longest = 0;
    {
        std::lock_guard lock(mMutex);
        for (auto const& binding : mBindings) {
            if (binding.keys.trigger() != virtualKey) continue;
            if (binding.keys.size() < longest) continue;
            if (!binding.keys.allHeld(mHeld)) continue;

            if (binding.keys.size() > longest) {
                longest = binding.keys.size();
                matches.clear();
            }
            matches.push_back(binding.handler);
        }
    }
    if (matches.empty()) return;

    mDispatching = true;
    for (auto& handler : matches) {
        try {
            if (handler) handler();
        } catch (std::exception const& error) {
            logger().error("Hotkey handler threw: {}", error.what());
        } catch (...) {
            logger().error("Hotkey handler threw an unknown exception");
        }
    }
    mDispatching = false;
}

} // namespace mangrove::input

#include "mangrove/Mangrove.h"
#include "mangrove/entry/KeyInputManager.h"
#include "mangrove/entry/Keys.h"

#include "ll/api/mod/RegisterHelper.h"

namespace mangrove {

namespace {

// 测试快捷键：按住 X 再按 C 触发（两个键都是普通键，触发键为最后按下的 C）
KEY_INPUT(TestKeyInput, VK_X, VK_C) { Mangrove::getInstance().getSelf().getLogger().info("Test key input: X + C"); }

} // namespace

Mangrove& Mangrove::getInstance() {
    static Mangrove instance;
    return instance;
}

bool Mangrove::load() {
    // 初始化按键管理器（内部订阅键盘事件）
    input::KeyInputManager::getInstance().init();

    // 注册测试快捷键
    TestKeyInput::subscribe();

    return true;
}

bool Mangrove::enable() { return true; }

bool Mangrove::disable() { return true; }

bool Mangrove::unload() {
    TestKeyInput::unsubscribe();

    // 取消订阅并清空按键管理器
    input::KeyInputManager::getInstance().clear();

    return true;
}

} // namespace mangrove

LL_REGISTER_MOD(mangrove::Mangrove, mangrove::Mangrove::getInstance());

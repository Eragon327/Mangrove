#include "mangrove/gui/MenuInputGuard.h"

#include "mangrove/gui/GuiOverlay.h"

#include "ll/api/memory/Hook.h"

#include "mc/deps/input/Keyboard.h"
#include "mc/deps/input/MouseDevice.h"
#include "mc/deps/input/win/HIDControllerGameCoreDesktop.h"

#include <cstdint>

namespace mangrove::gui {
namespace {

MenuInputGuardStatus       gInstallStatus{};
thread_local std::uint32_t gInputHandoffDepth{};

/// 菜单是否应该独占游戏输入。
/// @note 补发按键抬起时（MenuInputHandoffScope）临时放行，避免我们的补发事件被自己挡掉。
bool menuOwnsGameInput() { return gInputHandoffDepth == 0 && GuiOverlay::getInstance().shouldBlockGameInput(); }

LL_TYPE_INSTANCE_HOOK(
    MenuMouseInputHook,
    ll::memory::HookPriority::Highest,
    MouseDevice,
    &MouseDevice::feed,
    void,
    char  actionButtonId,
    schar buttonData,
    short x,
    short y,
    short dx,
    short dy,
    bool  forceMotionlessPointer
) {
    if (menuOwnsGameInput()) return;
    origin(actionButtonId, buttonData, x, y, dx, dy, forceMotionlessPointer);
}

LL_TYPE_INSTANCE_HOOK(
    MenuKeyDownInputHook,
    ll::memory::HookPriority::Highest,
    HIDControllerGameCoreDesktop,
    &HIDControllerGameCoreDesktop::$onKeyDown,
    void,
    int                                                 keyCode,
    Bedrock::Input::KeyboardEventProcessor::InputOrigin originType
) {
    // F11（全屏切换）始终交给游戏处理
    if (menuOwnsGameInput() && keyCode != Keyboard::F11) return;
    origin(keyCode, originType);
}

LL_TYPE_INSTANCE_HOOK(
    MenuKeyUpInputHook,
    ll::memory::HookPriority::Highest,
    HIDControllerGameCoreDesktop,
    &HIDControllerGameCoreDesktop::$onKeyUp,
    void,
    int keyCode
) {
    if (menuOwnsGameInput() && keyCode != Keyboard::F11) return;
    origin(keyCode);
}

} // namespace

MenuInputHandoffScope::MenuInputHandoffScope() { ++gInputHandoffDepth; }

MenuInputHandoffScope::~MenuInputHandoffScope() { --gInputHandoffDepth; }

MenuInputGuardStatus installMenuInputGuard() {
    if (!gInstallStatus.mouseInputHookInstalled) {
        gInstallStatus.mouseInputHookInstalled = MenuMouseInputHook::hook() == 0;
    }
    if (!gInstallStatus.keyDownInputHookInstalled) {
        gInstallStatus.keyDownInputHookInstalled = MenuKeyDownInputHook::hook() == 0;
    }
    if (!gInstallStatus.keyUpInputHookInstalled) {
        gInstallStatus.keyUpInputHookInstalled = MenuKeyUpInputHook::hook() == 0;
    }
    return gInstallStatus;
}

void uninstallMenuInputGuard() {
    if (gInstallStatus.mouseInputHookInstalled) {
        MenuMouseInputHook::unhook();
        gInstallStatus.mouseInputHookInstalled = false;
    }
    if (gInstallStatus.keyDownInputHookInstalled) {
        MenuKeyDownInputHook::unhook();
        gInstallStatus.keyDownInputHookInstalled = false;
    }
    if (gInstallStatus.keyUpInputHookInstalled) {
        MenuKeyUpInputHook::unhook();
        gInstallStatus.keyUpInputHookInstalled = false;
    }
    gInputHandoffDepth = 0;
}

} // namespace mangrove::gui

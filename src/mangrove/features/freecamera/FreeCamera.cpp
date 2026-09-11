#include "mangrove/features/freecamera/FreeCamera.h"

#include "mangrove/Mangrove.h"
#include "mangrove/gui/GuiWidgets.h"
#include "mangrove/input/Keys.h"

namespace mangrove::features {

FreeCamera& FreeCamera::getInstance() {
    static FreeCamera instance;
    return instance;
}

void FreeCamera::toggle() { setEnabled(!isEnabled()); }

bool FreeCamera::isEnabled() const { return mEnabled; }

void FreeCamera::addToMenu() {
    bool enabled = isEnabled();
    if (gui::addToggle(label("enabled", "Enabled"), enabled)) setEnabled(enabled);

    gui::addKeyBinder(bindingName(), label("hotkey", "Hotkey"));
}

void FreeCamera::shutdown() { setEnabled(false); }

void FreeCamera::setEnabled(bool enabled) {
    if (mEnabled == enabled) return;
    mEnabled = enabled;

    Mangrove::getInstance().getSelf().getLogger().info(
        "{}",
        label(enabled ? "enabledLog" : "disabledLog", enabled ? "enabled" : "disabled")
    );
}

// 默认热键：F
ADD_FEATURE(FreeCamera, VK_F)

} // namespace mangrove::features

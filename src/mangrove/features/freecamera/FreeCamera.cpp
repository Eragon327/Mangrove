#include "mangrove/features/freecamera/FreeCamera.h"
#include "mangrove/Mangrove.h"
#include "mangrove/core/Utils.h"
#include "mangrove/gui/GuiWidgets.h"
#include "mangrove/input/Keys.h"

#include "ll/api/i18n/I18n.h"

namespace mangrove::features {
using ll::i18n_literals::operator""_tr;

FreeCamera& FreeCamera::getInstance() {
    static FreeCamera instance;
    return instance;
}

void FreeCamera::toggle() { setEnabled(!isEnabled()); }

bool FreeCamera::isEnabled() const { return mEnabled; }

void FreeCamera::addToMenu() {
    // 功能名直接当标签：开关页 / 按键页都是一行「自由视角 [控件]」，
    // 不用再单独占一行分组标题
    bool enabled = isEnabled();
    if (gui::addToggle(displayName(), enabled)) setEnabled(enabled);

    gui::addKeyBinder(bindingName(), displayName());
}

void FreeCamera::shutdown() { setEnabled(false); }

void FreeCamera::setEnabled(bool enabled) {
    if (mEnabled == enabled) return;
    mEnabled = enabled;

    utils::sendTipsToClient(
        enabled ? "mangrove.feature.FreeCamera.enabled"_tr() : "mangrove.feature.FreeCamera.disabled"_tr()
    );
}

// 默认热键：F
ADD_FEATURE(FreeCamera, VK_F)

} // namespace mangrove::features

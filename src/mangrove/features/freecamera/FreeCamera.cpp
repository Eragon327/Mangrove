#include "mangrove/features/freecamera/FreeCamera.h"

namespace mangrove::features {

FreeCamera& FreeCamera::getInstance() {
    static FreeCamera instance;
    return instance;
}

FreeCamera::FreeCamera() : core::Feature("FreeCamera") {
    add(mEnabled);
    add(mSpeed);
    add(mFov);
}

std::optional<input::KeyBind> FreeCamera::defaultHotkey() const { return input::KeyBind::single(VK_F); }

void FreeCamera::onHotkey() { apply(!mEnabled.enabled()); }

void FreeCamera::onSettingsLoaded() {
    // 上次退出时开着的功能，这次启动要接着生效：
    //     if (mEnabled.enabled()) apply(true);
    // 相机钩子还没接上，先不做，免得每次启动都弹一句「已启用」。
}

void FreeCamera::onShutdown() {
    // 真的挂上相机钩子后，在这里摘干净
}

void FreeCamera::apply(bool enabled) {
    mEnabled.setValue(enabled ? 1.0 : 0.0);
    core::FeatureManager::notifyChanged(*this, mEnabled);

    // TODO: 真正挂 / 摘相机钩子的地方（LevelRendererPlayer::setupCamera）

    core::FeatureManager::getInstance().notify(label(enabled ? "on" : "off"));
}

} // namespace mangrove::features

#pragma once

#include "mangrove/core/Feature.h"

namespace mangrove::features {

/// 自由视角。
///
/// 它同时是**新增功能的参考模板**：一个功能 = `features/<名字>/` 一个目录，
/// 里面一个 `Feature` 子类（构造里声明设置项、重写需要的行为回调），
/// 再去 `Mangrove::addFeatures()` 登记一行。界面、持久化、热键都不用自己写。
///
/// @warning 相机改写本身还没接上 —— 那是功能自己的实现细节，不属于这套框架。
///          它现在的作用是把「新增一个功能」这条路完整跑通并被验证：
///          开关 + 热键 + 滑条 + 数字输入 + 全部持久化 + 热键反馈。
///          真正的实现加在 `apply()` 里（钩住 `LevelRendererPlayer::setupCamera`）。
class FreeCamera : public core::Feature {
public:
    static FreeCamera& getInstance();

    [[nodiscard]] std::optional<input::KeyBind> defaultHotkey() const override;

    void onHotkey() override;
    void onSettingsLoaded() override;
    void onShutdown() override;

private:
    FreeCamera();

    /// 统一入口：热键和菜单里的开关都走这里
    void apply(bool enabled);

    // 声明即得控件 + 持久化。声明顺序就是菜单里的顺序。
    core::ToggleSetting mEnabled{"enabled", false};
    core::NumberSetting mSpeed{"speed", 0.25, 4.0, 0.05, 1.0};
    core::NumberSetting mFov{"fov", 30.0, 110.0, 1.0, 70.0};
};

} // namespace mangrove::features

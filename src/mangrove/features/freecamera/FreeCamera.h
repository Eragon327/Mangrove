#pragma once

#include "mangrove/core/Feature.h"

namespace mangrove::features {

/// 自由视角：让相机离开玩家自由飞行（**纯客户端**）。
///
/// 做法与边界：
///   - **玩家实体一动不动**：只覆写渲染管线里的相机姿态，不改玩家位置、不改游戏模式、
///     不发任何网络包，所以服务端和其他玩家看到的永远是「你站在原地」。
///   - **WASD / 空格 / 左Shift 与鼠标移动在钩子层被吞掉**，游戏收不到，玩家自然不会跟着走；
///     相机位置由功能自己积分。鼠标**按键**放行，鼠标绑定的热键照旧可用。
///   - 开关是**瞬移式**的：启用时以本帧原生相机姿态为起点（位置就是玩家眼睛），
///     关闭时直接交还原生相机，不做插值。相机位置刻意**不持久化**。
///   - **能看见自己**：接管期间把玩家的视角选项临时切成第三人称（退出时原样还回去），
///     于是引擎照常渲染本地玩家的模型 —— 动作、手持物、身上装备都是真的，
///     而不是另建一个只会站着不动的假实体。
///   - **不接管 FOV**（连 `getFov` 都不钩）：取景角度完全维持原样。
///   - **不限制**离玩家的距离。代价是区块由服务端按**玩家**位置流送，
///     飞太远就是虚空 —— 这是方案本身的上限，不是 bug。
///   - 界面（菜单）打开时输入归界面：相机冻结在原地、清掉按住状态，菜单背后的画面不跳。
///
/// 实现分三个文件：本文件（对外接口）、`FreeCamera.cpp`（开关 / 状态积分）、
/// `FreeCameraHooks.cpp`（输入与相机钩子）。
class FreeCamera : public core::Feature {
public:
    static FreeCamera& getInstance();

    [[nodiscard]] std::optional<input::KeyBind> defaultHotkey() const override;

    void onHotkey() override;
    void onSettingsLoaded() override;
    void onSettingChanged(core::Setting& setting) override;
    void onShutdown() override;

    /// 把设置项同步进运行时状态。
    /// @note 渲染线程每帧调一次：钩子已经装好时，滑条拖动立即生效。
    void publishRuntimeConfig() const;

private:
    FreeCamera();

    /// 统一入口：热键、菜单开关、启动时套用配置都走这里。**幂等**。
    void apply(bool enabled);

    /// 改开关的值并落盘
    void storeEnabled(bool enabled);

    /// 进 / 出自由视角时接管与交还的渲染选项（第三人称：让玩家能看见自己的身体）
    void takeOverRenderOptions();
    void restoreRenderOptions();

    // 声明即得控件 + 持久化。声明顺序就是菜单里的顺序。
    core::ToggleSetting mEnabled{"enabled", false};
    core::NumberSetting mSpeed{"speed", 0.25, 10.0, 0.05, 1.0};

    /// 接管前的渲染选项，退出时原样还回去
    std::optional<int> mSavedViewPerspective;
};

} // namespace mangrove::features

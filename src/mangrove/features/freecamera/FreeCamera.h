#pragma once

#include "mangrove/core/Feature.h"

namespace mangrove::features {

/// 自由视角 —— **目前只是空壳占位**。
///
/// 相机钩子（`LevelRendererPlayer::setupCamera` 改写位姿 + 视图矩阵栈顶）已全部移除，
/// 只留下开关状态和一条日志。留着它是为了验证功能框架本身跑得通：
/// `ADD_FEATURE` 注册、热键（默认 F）、菜单开关、改键持久化。
///
/// 真正的实现见 git 历史；需要恢复时按 `core/Feature.h` 的说明实现 `toggle()` 即可。
class FreeCamera : public core::Feature {
public:
    static FreeCamera& getInstance();

    /// 热键按下 / 菜单里点开关时的入口
    void toggle() override;

    [[nodiscard]] bool isEnabled() const override;

    void addToMenu() override;

    /// mod 停用 / 卸载时把开关复位
    void shutdown() override;

private:
    FreeCamera() = default;

    void setEnabled(bool enabled);

    bool mEnabled{false};
};

} // namespace mangrove::features

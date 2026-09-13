#pragma once

#include "mangrove/core/Feature.h"

#include <limits>

namespace mangrove::features {

class AutoTool : public core::Feature {
public:
    static AutoTool& getInstance();

    [[nodiscard]] std::optional<input::KeyBind> defaultHotkey() const override;

    void onHotkey() override;
    void onSettingsLoaded() override;
    void onSettingChanged(core::Setting& setting) override;
    void onShutdown() override;

    /// 把设置项同步进运行时状态（钩子从原子读取，所以改完立刻生效）。
    /// @note 只管**数值参数**；开关旗标由 `apply()` 负责，它是唯一的写点。
    void publishRuntimeConfig() const;

private:
    AutoTool();

    /// 统一入口：热键、菜单开关、启动时套用配置都走这里。**幂等**。
    void apply(bool enabled);

    /// 改开关的值并落盘
    void storeEnabled(bool enabled);

    // 声明即得控件 + 持久化。声明顺序就是菜单里的顺序。
    core::ToggleSetting mEnabled{"enabled", false};
    // 纯输入框（没有滑条）：耐久下限想填多大填多大。
    // 上限取 int 的极值而不是 inf —— inf 写不进 config/Config.json（JSON 里没有 inf）。
    core::NumberInputSetting
        mMinDamage{"minDamage", 0.0, static_cast<double>(std::numeric_limits<int>::max()), 1.0, 0.0};
};

} // namespace mangrove::features
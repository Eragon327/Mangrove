#pragma once

#include "mangrove/core/Setting.h"
#include "mangrove/input/KeyBind.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mangrove::core {

class FeatureManager;

/// 一个功能模块。
///
/// 新增功能的完整步骤：
///   1. 继承 `Feature`，构造里用 `add()` 声明设置项，按需要重写行为回调；
///   2. 提供 `getInstance()` 单例；
///   3. 在 `Mangrove::load()` 的登记表里加一行。
///
/// 界面渲染与持久化由框架统一处理，功能本身不写任何 UI 代码 ——
/// 声明一条设置就等于同时得到了控件、存盘和读回。
class Feature {
public:
    Feature(Feature const&)            = delete;
    Feature& operator=(Feature const&) = delete;
    virtual ~Feature()                 = default;

    /// 功能标识（等于类名），同时是 i18n 与配置里的命名空间
    [[nodiscard]] std::string const& id() const { return mId; }

    /// 显示名：语言文件里的 `mangrove.feature.<id>.name`，取不到时退回 id
    [[nodiscard]] std::string displayName() const;

    /// 读本功能在语言文件里的字段，例如 `label("speed")` 查
    /// `mangrove.feature.<id>.speed`；查不到时用 @p fallback，再兜底为字段名本身。
    [[nodiscard]] std::string label(std::string_view field, std::string_view fallback = {}) const;

    /// 热键绑定的稳定标识，形如 `feature.FreeCamera`
    [[nodiscard]] std::string bindingName() const;

    /// 本功能的设置项，顺序即界面顺序
    [[nodiscard]] std::vector<Setting*> const& settings() const { return mSettings; }

    // -----------------------------------------------------------------------
    // 行为回调
    // -----------------------------------------------------------------------

    /// 默认热键。返回 `std::nullopt` 表示这个功能不占热键。
    [[nodiscard]] virtual std::optional<input::KeyBind> defaultHotkey() const { return std::nullopt; }

    /// 热键按下时调用
    virtual void onHotkey() {}

    /// 持久化设置已经套用到各个 `Setting` 之后调用一次。
    /// 需要「把配置真正生效」的功能（例如按开关状态装钩子）在这里做。
    virtual void onSettingsLoaded() {}

    /// 某个设置项的值被界面改动后调用（勾选框 / 滑条 / 数字输入）。
    /// 需要「值一变就生效」的功能在这里处理；持久化由框架负责。
    ///
    /// @note 派发点在 `FeatureManager::notifyChanged`，而功能自己改回设置值
    ///       （例如钩子装不上就退回关闭）也会再触发一次，所以这里的处理必须**幂等**。
    virtual void onSettingChanged(Setting& /*setting*/) {}

    /// mod 停用 / 卸载时清理（摘钩子、关功能）
    virtual void onShutdown() {}

protected:
    /// @param id 功能标识，建议与类名一致（i18n 与配置都用它做命名空间）
    explicit Feature(std::string id) : mId(std::move(id)) {}

    /// 声明一个设置项，返回它本身以便链式写法。只在构造函数里调用。
    template <class T>
    T& add(T& setting) {
        mSettings.push_back(&setting);
        return setting;
    }

private:
    std::string           mId;
    std::vector<Setting*> mSettings;
};

/// 功能注册表。
///
/// 负责两件功能自己不该操心的事：把持久化的设置值套回 `Setting`，以及把默认热键
/// （连同玩家改过的键）注册进 `input::KeyManager`。
class FeatureManager {
public:
    static FeatureManager& getInstance();

    FeatureManager(FeatureManager const&)            = delete;
    FeatureManager& operator=(FeatureManager const&) = delete;

    /// 登记一个功能。重复登记同一实例会被忽略。
    void add(Feature& feature);

    /// 套用持久化设置 + 注册热键。重复调用安全。
    void install();

    /// 让各功能收拾自己并复位。重复调用安全。
    void uninstall();

    [[nodiscard]] std::vector<Feature*> const& features() const { return mFeatures; }

    /// 设置项被改动后调用：先让功能对新值生效，再持久化（`SettingsStore` 直接写库，
    /// 没有「攒着一起落盘」这一层）。
    static void notifyChanged(Feature& feature, Setting& setting);

    /// 设置反馈出口。由 `Mangrove` 接到界面上，功能只喊一声、不认识界面层。
    using Notifier = std::function<void(std::string)>;
    void setNotifier(Notifier notifier);

    /// 给玩家一句反馈（例如「自由视角已启用」）。没有出口时退化成日志。
    /// @note 功能调这个就够了，不需要 include 任何界面头文件。
    void notify(std::string message);

    /// 设置项在设置库（`config/Config.json` 里则是滑条区间）里的完整 key：
    /// `<feature>.<setting>`
    [[nodiscard]] static std::string settingKey(Feature const& feature, Setting const& setting);

private:
    FeatureManager() = default;

    std::vector<Feature*> mFeatures;
    Notifier              mNotifier;
    bool                  mInstalled{};
};

} // namespace mangrove::core

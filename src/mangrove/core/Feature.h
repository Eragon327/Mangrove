#pragma once

#include "ll/api/base/StdInt.h"

#include <string>
#include <string_view>
#include <vector>

namespace mangrove::core {

class FeatureManager;

/// 功能基类。
///
/// 加一个功能只需要三步：
///   1. 继承 `Feature`，实现 `toggle()`（开关逻辑）与 `addToMenu()`（菜单控件）；
///   2. 提供 `getInstance()` 单例；
///   3. 在该功能的 .cpp 里、**命名空间作用域**下写 `ADD_FEATURE(YourFeature, VK_F)`。
///
/// 框架负责的部分：热键注册、改键结果的持久化与恢复、把菜单项按登记顺序画进菜单。
class Feature {
public:
    Feature(Feature const&)            = delete;
    Feature& operator=(Feature const&) = delete;
    virtual ~Feature()                 = default;

    /// 功能名，等于 `ADD_FEATURE` 的类名（如 `FreeCamera`）
    [[nodiscard]] std::string const& name() const { return mName; }

    /// 热键绑定的稳定标识，形如 `feature.FreeCamera`（KeyInputManager / DataBase 用）
    [[nodiscard]] std::string bindingName() const;

    /// 默认组合键（Windows 虚拟键码 VK_*），由 `ADD_FEATURE` 写入
    [[nodiscard]] std::vector<int> const& defaultKeys() const { return mDefaultKeys; }

    /// 取本功能在语言文件里的字段，例如 `label("hotkey")` 查 `mangrove.feature.<name>.hotkey`。
    /// 查不到时返回 @p fallback；@p fallback 为空则返回字段名本身。
    [[nodiscard]] std::string label(std::string_view field, std::string_view fallback = {}) const;

    /// 触发：按下热键、或从菜单点开关时调用。**子类在这里写开关逻辑。**
    virtual void toggle() = 0;

    /// 当前是否开启。菜单用它显示开关状态；不想暴露就保持默认实现。
    [[nodiscard]] virtual bool isEnabled() const { return false; }

    /// 把自己的控件加进菜单，用 `gui::add*` 系列。
    /// @note 外层分组标题由框架负责，这里不用再画。
    virtual void addToMenu() {}

    /// 模组停用 / 卸载时的清理。挂了钩子的功能要在这里摘干净。
    virtual void shutdown() {}

protected:
    /// 功能都是单例，只能由子类的 `getInstance()` 构造
    Feature() = default;

private:
    friend class FeatureManager;

    std::string      mName;
    std::vector<int> mDefaultKeys;
};

/// 功能注册表。
///
/// `ADD_FEATURE` 宏在静态初始化期只登记指针（不碰任何游戏状态）；
/// `Mangrove::load()` 里再调 `install()` 统一注册热键 —— 默认键与持久化改键
/// 因此只有一个入口，不会散落在各个功能里。
class FeatureManager {
public:
    static FeatureManager& getInstance();

    FeatureManager(FeatureManager const&)            = delete;
    FeatureManager& operator=(FeatureManager const&) = delete;

    /// 登记一个功能（由 ADD_FEATURE 宏调用）。重复登记同一个实例会被忽略。
    void add(Feature& feature, std::string name, std::vector<int> defaultKeys);

    /// 注册所有功能的热键（KeyInputManager 装好之后调用）。重复调用安全。
    void install();

    /// 注销热键并让各功能收拾自己（mod disable 时调用）。重复调用安全。
    void uninstall();

    /// 把所有功能的菜单项画进当前 ImGui 窗口。
    void addToMenu();

    [[nodiscard]] std::vector<Feature*> const& features() const { return mFeatures; }

private:
    FeatureManager() = default;

    std::vector<Feature*> mFeatures;
    std::vector<uint64>   mBindingIds;
    bool                  mInstalled{};
};

} // namespace mangrove::core

/// 注册一个功能（在功能的 .cpp 里、命名空间作用域下调用）：
///
///     ADD_FEATURE(FreeCamera, VK_F)
///
/// 第一个参数是类名，同时作为功能名与热键绑定名；
/// 后面的参数是默认组合键（Windows 虚拟键码 VK_*）。
#define ADD_FEATURE(Type, ...)                                                                                         \
    namespace {                                                                                                        \
    struct Type##FeatureRegistrar {                                                                                    \
        Type##FeatureRegistrar() {                                                                                     \
            ::mangrove::core::FeatureManager::getInstance().add(Type::getInstance(), #Type, {__VA_ARGS__});            \
        }                                                                                                              \
    };                                                                                                                 \
    Type##FeatureRegistrar Type##FeatureRegistrarInstance;                                                             \
    }

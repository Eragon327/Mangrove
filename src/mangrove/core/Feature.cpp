#include "mangrove/core/Feature.h"

#include "mangrove/Mangrove.h"
#include "mangrove/core/Config.h"
#include "mangrove/input/KeyManager.h"

#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mangrove::core {
namespace {

/// 判断「值是否等于默认值」的容差。
/// 滑条是浮点数，经过步长吸附后未必能精确等于默认值，所以不能直接用 `==`。
constexpr double kSameValueEpsilon = 1e-9;

} // namespace

std::string Feature::displayName() const { return label("name", mId); }

std::string Feature::label(std::string_view field, std::string_view fallback) const {
    std::string const key = "mangrove.feature." + mId + "." + std::string{field};

    // `_tr` 只吃编译期字面量，运行期拼出来的 key 只能走 `get()`；
    // 查不到时 `get()` 会把 key 原样返回，所以拿它和 key 比一次。
    if (auto const value = ll::i18n::getInstance().get(key, {}); !value.empty() && value != key) {
        return std::string{value};
    }
    return fallback.empty() ? std::string{field} : std::string{fallback};
}

std::string Feature::bindingName() const { return "feature." + mId; }

FeatureManager& FeatureManager::getInstance() {
    static FeatureManager instance;
    return instance;
}

void FeatureManager::add(Feature& feature) {
    if (std::ranges::find(mFeatures, &feature) != mFeatures.end()) return;

    // id 同时是配置 key、i18n 命名空间和热键绑定名，重了会互相踩。
    // 最容易出现的情况是复制一个功能后忘了改 `core::Feature("...")`，所以宁可报一声。
    if (std::ranges::any_of(mFeatures, [&](Feature const* other) { return other->id() == feature.id(); })) {
        Mangrove::getInstance().getSelf().getLogger().warn(
            "Duplicate feature id '{}'; settings and hotkeys will collide",
            feature.id()
        );
    }

    mFeatures.push_back(&feature);
}

void FeatureManager::install() {
    if (mInstalled) return;
    mInstalled = true;

    auto& config     = Config::getInstance();
    auto& keyManager = input::KeyManager::getInstance();

    for (auto* feature : mFeatures) {
        if (!feature) continue;

        // 1) 把存过的值套回每个 Setting；没有记录就保留声明时的默认值
        for (auto* setting : feature->settings()) {
            if (auto const stored = config.getNumber(settingKey(*feature, *setting))) {
                setting->setValue(*stored);
            }
        }

        // 2) 让功能把配置真正生效（装钩子之类）
        feature->onSettingsLoaded();

        // 3) 注册热键。KeyManager 会再套一次改键记录。
        if (auto const hotkey = feature->defaultHotkey()) {
            keyManager.add(feature->bindingName(), *hotkey, [feature] { feature->onHotkey(); });
        }
    }
}

void FeatureManager::uninstall() {
    for (auto* feature : mFeatures) {
        if (feature) feature->onShutdown();
    }
    mInstalled = false;
}

void FeatureManager::notifyChanged(Feature const& feature, Setting const& setting) {
    auto const key = settingKey(feature, setting);

    // 只存 diff：值等于代码默认值时不该留记录，否则改默认值就会「迁不动」
    if (std::abs(setting.value() - setting.defaultValue()) <= kSameValueEpsilon) {
        Config::getInstance().removeNumber(key);
    } else {
        Config::getInstance().setNumber(key, setting.value());
    }
}

void FeatureManager::setNotifier(Notifier notifier) { mNotifier = std::move(notifier); }

void FeatureManager::notify(std::string message) {
    if (message.empty()) return;

    if (mNotifier) {
        mNotifier(std::move(message));
    } else {
        // 界面还没就绪（例如 load 阶段就把功能打开了）时不该丢掉信息
        Mangrove::getInstance().getSelf().getLogger().debug("{}", message);
    }
}

std::string FeatureManager::settingKey(Feature const& feature, Setting const& setting) {
    return feature.id() + "." + setting.id();
}

} // namespace mangrove::core

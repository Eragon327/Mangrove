#include "mangrove/core/Feature.h"

#include "mangrove/Mangrove.h"
#include "mangrove/core/Config.h"
#include "mangrove/core/SettingsStore.h"
#include "mangrove/input/KeyManager.h"

#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <utility>

namespace mangrove::core {

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

    auto& store      = SettingsStore::getInstance();
    auto& keyManager = input::KeyManager::getInstance();
    auto& config     = Config::getInstance();

    for (auto* feature : mFeatures) {
        if (!feature) continue;

        // 1) 先按框架配置调区间（config/Config.json 里的滑条上下限 / 步长），
        //    再套玩家存过的值 —— 顺序不能反，值要按最终的区间夹一遍。
        for (auto* setting : feature->settings()) {
            if (!setting) continue;
            auto const key = settingKey(*feature, *setting);

            if (setting->isNumeric()) {
                auto const range = config.rangeOf(key);
                setting->overrideRange(range.min, range.max, range.step);
            }
            if (auto const stored = store.getNumber(key)) setting->setValue(*stored);
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

void FeatureManager::notifyChanged(Feature& feature, Setting& setting) {
    // 先让功能对新值做出反应（装 / 摘钩子之类），再写库。
    // 顺序反过来的话，功能把值改回去（例如钩子装不上就退回关闭）时存的还是旧值。
    feature.onSettingChanged(setting);

    auto const key = settingKey(feature, setting);

    // 只存 diff：值等于代码默认值时不该留记录，否则改默认值就会「迁不动」
    if (setting.isDefault()) {
        SettingsStore::getInstance().removeNumber(key);
    } else {
        SettingsStore::getInstance().setNumber(key, setting.value());
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

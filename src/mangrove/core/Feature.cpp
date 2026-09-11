#include "mangrove/core/Feature.h"
#include "mangrove/input/KeyInputManager.h"

#include "ll/api/i18n/I18n.h"

#include <utility>

namespace mangrove::core {

std::string Feature::bindingName() const { return "feature." + mName; }

std::string Feature::label(std::string_view field, std::string_view fallback) const {
    std::string const key = "mangrove.feature." + mName + "." + std::string{field};

    // `_tr` 只吃编译期字面量，运行期拼出来的 key 得走 `get()`
    if (auto const value = ll::i18n::getInstance().get(key, {}); !value.empty()) return std::string{value};

    return fallback.empty() ? std::string{field} : std::string{fallback};
}

FeatureManager& FeatureManager::getInstance() {
    static FeatureManager instance;
    return instance;
}

void FeatureManager::add(Feature& feature, std::string name, std::vector<int> defaultKeys) {
    for (auto const* registered : mFeatures) {
        if (registered == &feature) return;
    }

    feature.mName        = std::move(name);
    feature.mDefaultKeys = std::move(defaultKeys);
    mFeatures.push_back(&feature);
}

void FeatureManager::install() {
    if (mInstalled) return;
    mInstalled = true;

    mBindingIds.clear();
    mBindingIds.reserve(mFeatures.size());

    auto& keys = input::KeyInputManager::getInstance();
    for (auto* feature : mFeatures) {
        if (!feature) continue;
        // 默认键来自 ADD_FEATURE；KeyInputManager::add 会套上玩家改过的键
        mBindingIds.push_back(keys.add(feature->bindingName(), feature->defaultKeys(), [feature] {
            feature->toggle();
        }));
    }
}

void FeatureManager::uninstall() {
    auto& keys = input::KeyInputManager::getInstance();
    for (auto id : mBindingIds) keys.remove(id);
    mBindingIds.clear();

    for (auto* feature : mFeatures) {
        if (feature) feature->shutdown();
    }

    mInstalled = false;
}

void FeatureManager::addToMenu() {
    for (auto* feature : mFeatures) {
        if (feature) feature->addToMenu();
    }
}

} // namespace mangrove::core

#include "mangrove/core/Config.h"

#include "mangrove/Mangrove.h"
#include "mangrove/core/Feature.h"

#include "ll/api/Config.h"

#include <exception>
#include <utility>

namespace mangrove::core {
namespace {

auto& logger() { return Mangrove::getInstance().getSelf().getLogger(); }

} // namespace

Config& Config::getInstance() {
    static Config instance;
    return instance;
}

void Config::load(std::filesystem::path const& file) {
    // 先把代码里声明的区间填进来：文件缺项时用的就是它，生成出来的默认文件也能当说明书看
    for (auto* feature : FeatureManager::getInstance().features()) {
        if (!feature) continue;
        for (auto* setting : feature->settings()) {
            // 只有数值项有区间；开关就是 0 / 1，没得调
            if (!setting || setting->kind() != Setting::Kind::Number) continue;

            auto& range = mData.sliders[FeatureManager::settingKey(*feature, *setting)];
            if (!range.min) range.min = setting->min();
            if (!range.max) range.max = setting->max();
            if (!range.step) range.step = setting->step();
        }
    }

    // 拿副本去读：文件坏了（抛异常）也不污染上面那套默认值，更不动人的文件
    ConfigData loaded = mData;
    try {
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);

        if (!ll::config::loadConfig(loaded, file)) {
            // 文件不存在 / 版本变了：把默认值补进去重写一份，已有的值不会被覆盖
            ll::config::saveConfig(loaded, file);
        }
        mData = std::move(loaded);
    } catch (std::exception const& error) {
        logger().error("Malformed config '{}' ({}); using the declared ranges", file.string(), error.what());
    }
}

SliderRange Config::rangeOf(std::string_view settingKey) const {
    auto const iterator = mData.sliders.find(std::string{settingKey});
    return iterator == mData.sliders.end() ? SliderRange{} : iterator->second;
}

} // namespace mangrove::core

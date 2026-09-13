#include "mangrove/core/SettingsStore.h"

#include "mangrove/Mangrove.h"
#include "mangrove/input/KeyBind.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <string>

namespace mangrove::core {
namespace {

/// 两套 key 的前缀，见头文件里的说明
constexpr std::string_view kSettingPrefix = "setting/";
constexpr std::string_view kKeybindPrefix = "keybind/";

auto& logger() { return Mangrove::getInstance().getSelf().getLogger(); }

[[nodiscard]] std::string prefixed(std::string_view prefix, std::string_view key) {
    std::string result{prefix};
    result += key;
    return result;
}

/// 数值存成 JSON 文本：这是能往返的最短写法（`std::to_string` 只给 6 位小数）
[[nodiscard]] std::string encodeNumber(double value) { return nlohmann::json(value).dump(); }

/// 解析数值；坏掉 / 类型不对返回空。开关手写成 `true` / `false` 也认。
[[nodiscard]] std::optional<double> decodeNumber(std::string_view text) {
    auto const parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_boolean()) return parsed.get<bool>() ? 1.0 : 0.0;
    if (!parsed.is_number()) return std::nullopt;
    return parsed.get<double>();
}

} // namespace

SettingsStore& SettingsStore::getInstance() {
    static SettingsStore instance;
    return instance;
}

bool SettingsStore::load(std::filesystem::path const& path) {
    try {
        mDatabase = std::make_unique<ll::data::KeyValueDB>(path);
    } catch (std::exception const& error) {
        mDatabase.reset();
        logger().error(
            "Failed to open the settings database '{}' ({}); settings will not persist",
            path.string(),
            error.what()
        );
    }
    return mDatabase != nullptr;
}

void SettingsStore::close() { mDatabase.reset(); }

std::optional<double> SettingsStore::getNumber(std::string_view key) const {
    if (!mDatabase) return std::nullopt;

    auto const stored = mDatabase->get(prefixed(kSettingPrefix, key));
    if (!stored) return std::nullopt;
    return decodeNumber(*stored);
}

void SettingsStore::setNumber(std::string_view key, double value) {
    if (mDatabase) mDatabase->set(prefixed(kSettingPrefix, key), encodeNumber(value));
}

void SettingsStore::removeNumber(std::string_view key) {
    if (mDatabase) mDatabase->del(prefixed(kSettingPrefix, key));
}

std::optional<std::vector<int>> SettingsStore::getKeys(std::string_view name) const {
    if (!mDatabase) return std::nullopt;

    auto const stored = mDatabase->get(prefixed(kKeybindPrefix, name));
    if (!stored) return std::nullopt;

    auto const parsed = nlohmann::json::parse(*stored, nullptr, false);
    if (!parsed.is_array()) return std::nullopt;

    std::vector<int> keys;
    keys.reserve(parsed.size());
    for (auto const& element : parsed) {
        // 键码合法性交给 input 层判：和改键捕获用的是同一套规则
        if (!element.is_number_integer() || !input::isValidKey(element.get<int>())) return std::nullopt;
        keys.push_back(element.get<int>());
    }
    // 空数组照样是**有效**返回值：它是「玩家解绑了」，不能退化成「没有记录」
    return keys;
}

void SettingsStore::setKeys(std::string_view name, std::vector<int> const& keys) {
    if (mDatabase) mDatabase->set(prefixed(kKeybindPrefix, name), nlohmann::json(keys).dump());
}

void SettingsStore::removeKeys(std::string_view name) {
    if (mDatabase) mDatabase->del(prefixed(kKeybindPrefix, name));
}

} // namespace mangrove::core

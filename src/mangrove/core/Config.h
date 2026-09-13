#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mangrove::core {

/// 一条数值设置的区间覆盖。
/// 三个字段都是可选的：只想改上限就只写 `max`，其余照旧。
struct SliderRange {
    std::optional<double> min;
    std::optional<double> max;
    std::optional<double> step;
};

/// `config/Config.json` 的内容。
/// @note 成员名会被 LL 的反射 API 直接当成 JSON 的键 —— 改成员名等于改文件格式。
struct ConfigData {
    /// LL 的配置 API 靠它判断要不要把新字段补写进旧文件（改了上面的结构就 +1）
    int version{1};

    /// `<功能 id>.<设置项 id>` -> 滑条区间，例如 `"FreeCamera.speed"`。
    /// 文件里没写的项用代码里声明的区间（见 `Config::load` 的默认值回填）。
    std::unordered_map<std::string, SliderRange> sliders;
};

/// **框架配置** —— 落 `<mod>/config/Config.json`，手改，运行时只读。
///
/// 只回答一件事：**每个数值设置项的区间**（下限 / 上限 / 步长），
/// 也就是「菜单里这根滑条最多能拖到多少」。
///
/// 和玩家设置（`core::SettingsStore` 的 `data/Setting`）刻意分成两份：
/// 「能调到多少」是 mod 侧的设计参数，「现在调到了多少」是玩家的数据。
/// 混在一起写的话，作者改区间会把玩家的值冲掉，玩家改值又会被当成新的区间。
///
/// ```json
/// {
///   "version": 1,
///   "sliders": { "FreeCamera.speed": { "min": 0.1, "max": 20, "step": 0.1 } }
/// }
/// ```
///
/// 走 LL 的 `ll::config` 反射 API：缺字段用代码里的值补上，未知字段原样留着。
/// 文件不存在时生成一份，里面**已经填好代码里声明的区间** —— 想知道有哪些旋钮、
/// 当前是多少，看这个文件就行。
class Config {
public:
    static Config& getInstance();

    Config(Config const&)            = delete;
    Config& operator=(Config const&) = delete;

    /// 读 `Config.json`。重复调用安全。
    /// @param file 配置文件路径
    /// @note **必须排在功能登记之后调**：文件缺失时要照着各功能声明的设置项写默认值。
    ///       读坏了只报日志、不覆盖人的文件，内存里留代码默认值。
    void load(std::filesystem::path const& file);

    /// 某个设置项的区间覆盖；没配的字段是空 `optional`（调用方就不用动那一头）。
    /// @param settingKey `<功能 id>.<设置项 id>`，与 `FeatureManager::settingKey` 一致
    [[nodiscard]] SliderRange rangeOf(std::string_view settingKey) const;

private:
    Config() = default;

    ConfigData mData;
};

} // namespace mangrove::core

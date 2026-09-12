#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mangrove::core {

/// 全 mod 的持久化设置，落在 `<mod>/config/config.json` 一个文件里。
///
/// **只存 diff** —— 和代码默认值相同的项不写进文件。于是：
///   - 文件很小，改代码里的默认值不需要任何迁移；
///   - 手改这个文件只能表达「这一项和默认不同」这一件事；
///   - 设置回到默认时记录会被删掉，文件自己会变干净。
///
/// ```json
/// {
///   "version": 1,
///   "keybinds": { "menu.toggle": [88, 67] },
///   "settings": { "FreeCamera.speed": 2.5 }
/// }
/// ```
///
/// 读取时把文件当成**不可信输入**：key 缺失、类型不对、数值越界、JSON 语法坏，
/// 一律退化成代码默认值，不会崩、也不会把 mod 卡在启动阶段。
/// JSON 语法坏掉时把原文件改名成 `config.json.bak` 再重建 —— 不能让它一直坏着，
/// 否则每次启动都静默丢一次设置，玩家还不知道为什么。
///
/// 修改只落在内存并标脏，`flush()` 时才写盘（时机：菜单关闭、mod 停用 / 卸载），
/// 这样拖动滑条不会每帧写文件。
///
/// @note **不要把它当公开接口**：不承诺 key 稳定，改名就等于回到默认值。
class Config {
public:
    static Config& getInstance();

    Config(Config const&)            = delete;
    Config& operator=(Config const&) = delete;
    ~Config();

    /// 打开配置文件。文件不存在时写一份；坏掉时备份后重建。重复调用安全。
    /// @returns 是否可用（不可用时修改不落盘，但内存里的值仍然生效）
    bool load(std::filesystem::path const& file);

    /// 脏了才写盘。重复调用安全。
    void flush();

    /// 写盘并关闭。重复调用安全。
    void close();

    [[nodiscard]] bool isOpen() const;

    // -----------------------------------------------------------------------
    // 数值 / 开关设置
    // -----------------------------------------------------------------------

    /// 没有记录时返回 `std::nullopt`（= 用代码默认值）。
    /// @note 手改写成 `true` / `false` 的开关也认（视作 1 / 0）。
    [[nodiscard]] std::optional<double> getNumber(std::string_view key) const;

    /// 写一条设置。值等于默认值时请用 `removeNumber`，别写进来。
    void setNumber(std::string_view key, double value);

    /// 删掉一条设置（回到代码默认值）
    void removeNumber(std::string_view key);

    // -----------------------------------------------------------------------
    // 按键绑定
    // -----------------------------------------------------------------------

    /// 键码数组；没有记录时返回空
    [[nodiscard]] std::vector<int> getKeys(std::string_view name) const;

    /// 写一条改键记录；@p keys 为空表示**删除记录**（回到默认键）
    void setKeys(std::string_view name, std::vector<int> const& keys);

private:
    Config() = default;

    mutable std::mutex mMutex;
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace mangrove::core

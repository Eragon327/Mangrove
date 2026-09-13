#pragma once

#include "ll/api/data/KeyValueDB.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace mangrove::core {

/// **玩家设置**的持久化存储，落 `<mod>/data/Setting`。
///
/// 就是一个 LevelDB（`ll::data::KeyValueDB`）。用 KV 库而不是 JSON 文件，是因为这里的 key
/// 是**动态的**（有哪些设置项、有哪些改键记录，全看代码里登记了什么）；换成 JSON 的话
/// 每改一处都要把整份文件读出来、改一个字段、再整份写回去。
///
/// @note 放在 `data/` 而不是 `config/`：`config/Config.json` 那类「人写的设计参数」和这里
///       「机器写出来的玩家数据」是两回事。
///
/// 两套 key，各带一个前缀（前缀是必需的：功能 `menu` 的设置项 `toggle` 就叫 `menu.toggle`，
/// 和菜单开关那条键位同名）：
///   - `setting/<功能 id>.<设置项 id>` -> 数值
///   - `keybind/<绑定名>`             -> 键码数组
///
/// 值都写成 JSON 文本（数字 / 数组），读回来当**不可信输入**：解析不出来、类型不对、
/// 键码非法，一律当作这条记录不存在，调用方自然退回代码里的默认值。
///
/// **只存 diff**：等于默认值的记录会被删掉，所以库里就是「玩家的改动」这一份清单 ——
/// 改代码里的默认值不需要任何迁移。判「等不等于默认」由 `Setting::isDefault()` /
/// `KeyBind::equals()` 负责，这里只提供 `set` 和 `remove` 两个动作。
///
/// @note 没有 `flush()`：LevelDB 的写是 O(1) 追加，写下去就在库里，
///       不必像 JSON 那样攒着等合适时机整份重写。
/// @note 没有锁：`ll::data::KeyValueDB` 的读写是线程安全的，而库句柄只在 `load` /
///       `close` 里换 —— 这两个调用都发生在没有别人碰它的时候（启动早期、卸载时）。
/// @note 打不开库（磁盘坏、路径被占）不致命：读全返回空、写全丢弃，内存里的设置照常生效，
///       只是这一次启动不会存下来。
class SettingsStore {
public:
    static SettingsStore& getInstance();

    SettingsStore(SettingsStore const&)            = delete;
    SettingsStore& operator=(SettingsStore const&) = delete;

    /// 打开设置库（不存在就建一份）。重复调用安全。
    /// @returns 是否可用
    bool load(std::filesystem::path const& path);

    /// 关掉库句柄。重复调用安全。
    void close();

    // -----------------------------------------------------------------------
    // 数值 / 开关设置
    // -----------------------------------------------------------------------

    /// 没有记录时返回 `std::nullopt`（= 用代码默认值）。
    /// @note 手改成 `true` / `false` 的开关也认（视作 1 / 0）
    [[nodiscard]] std::optional<double> getNumber(std::string_view key) const;

    void setNumber(std::string_view key, double value);

    /// 删掉记录（回到代码默认值）
    void removeNumber(std::string_view key);

    // -----------------------------------------------------------------------
    // 按键绑定
    // -----------------------------------------------------------------------

    /// 键码数组；没有记录 / 记录不合法时返回空
    [[nodiscard]] std::vector<int> getKeys(std::string_view name) const;

    void setKeys(std::string_view name, std::vector<int> const& keys);

    /// 删掉记录（回到代码默认键）
    void removeKeys(std::string_view name);

private:
    SettingsStore() = default;

    std::unique_ptr<ll::data::KeyValueDB> mDatabase;
};

} // namespace mangrove::core

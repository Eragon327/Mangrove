#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ll::data {
class KeyValueDB;
}

namespace mangrove::data {

/// 键值存储（KV），落在 mod 的 `data/` 目录下。
///
/// 这里是全 mod 唯一的持久化入口：**没有配置文件**，所有设置都在游戏里改，
/// 改完直接写到这里。
///
/// 存的是「运行期产生、需要跨启动保留」的东西，目前有：
///   - 按键绑定：键 = 绑定的稳定名（`menu.toggle` / `feature.FreeCamera`），值 = 逗号分隔的 VK 码
///
/// @note 只存玩家改过的值（diff）。没写过的键读出来是 `std::nullopt`，调用方沿用代码里的默认值。
class DataBase {
public:
    DataBase();
    ~DataBase();

    DataBase(DataBase const&)            = delete;
    DataBase& operator=(DataBase const&) = delete;

    /// 打开数据库目录。重复调用安全。
    /// @param backendDir KeyValueDB 使用的目录，通常是 `<mod>/data/kv`
    /// @returns 是否可用
    bool open(std::filesystem::path const& backendDir);

    /// 关闭数据库。重复调用安全。
    void close();

    [[nodiscard]] bool isOpen() const;

    /// 读取字符串；键不存在时返回 `std::nullopt`
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;

    /// 写入 / 覆盖；@p value 为空时等价于删除
    bool set(std::string_view key, std::string_view value);

    bool remove(std::string_view key);

    /// 读取整数列表（`"88,67"` -> `{88,67}`）；键不存在或格式非法返回 `std::nullopt`
    [[nodiscard]] std::optional<std::vector<int>> getIntList(std::string_view key) const;

    /// 写入整数列表；@p values 为空时等价于删除
    bool setIntList(std::string_view key, std::vector<int> const& values);

private:
    std::unique_ptr<ll::data::KeyValueDB> mDatabase;
};

} // namespace mangrove::data

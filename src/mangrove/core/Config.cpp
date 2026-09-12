#include "mangrove/core/Config.h"

#include "mangrove/Mangrove.h"

#include "ll/api/io/FileUtils.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace mangrove::core {
namespace {

/// 配置结构版本。将来若要做真正的迁移，靠它判断；目前只是写进文件方便排查。
constexpr int kConfigVersion = 1;

auto& logger() { return Mangrove::getInstance().getSelf().getLogger(); }

} // namespace

class Config::Impl {
public:
    nlohmann::ordered_json data;
    std::filesystem::path  path;
    bool                   dirty{};
    bool                   open{};

    /// 保证 data 里有 keybinds / settings 两个对象
    void ensureSections() {
        if (!data.is_object()) data = nlohmann::ordered_json::object();
        if (!data.contains("version")) data["version"] = kConfigVersion;
        if (!data.contains("keybinds") || !data["keybinds"].is_object())
            data["keybinds"] = nlohmann::ordered_json::object();
        if (!data.contains("settings") || !data["settings"].is_object())
            data["settings"] = nlohmann::ordered_json::object();
    }

    /// 删掉某个 section 下的一条记录；真的删掉了才返回 true
    bool erase(std::string_view section, std::string_view key) {
        auto const iterator = data.find(section);
        if (iterator == data.end() || !iterator->is_object()) return false;
        return iterator->erase(std::string{key}) > 0;
    }
};

/// 把语法坏掉的配置文件挪到一边，让 `flush()` 重建一份干净的。
/// @returns 备份是否成功
bool backupBrokenFile(std::filesystem::path const& file) {
    auto const      backup = file.string() + ".bak";
    std::error_code error;
    // Windows 的 rename 不覆盖已存在的文件，先删掉旧的备份
    std::filesystem::remove(backup, error);
    std::filesystem::rename(file, backup, error);
    if (error) {
        logger().error("Malformed config file '{}' (backup failed: {})", file.string(), error.message());
        return false;
    }
    logger().warn("Malformed config file; moved to '{}' and regenerating defaults", backup);
    return true;
}

Config& Config::getInstance() {
    static Config instance;
    return instance;
}

Config::~Config() = default;

bool Config::load(std::filesystem::path const& file) {
    bool needFlush = false;
    {
        std::lock_guard lock(mMutex);

        mImpl       = std::make_unique<Impl>();
        mImpl->path = file;
        mImpl->ensureSections();

        auto const content = ll::file_utils::readFile(file);
        if (content && !content->empty()) {
            auto parsed = nlohmann::ordered_json::parse(*content, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                // 坏文件不能就这么留着：留着就会每次启动都静默丢设置，而玩家不知道原因。
                // 挪到 .bak，内存里保持空骨架并标脏，flush() 会写一份干净的。
                backupBrokenFile(file);
                mImpl->dirty = true;
            } else {
                mImpl->data = std::move(parsed);
                mImpl->ensureSections();
            }
        } else {
            // 首次运行 / 空文件：写一份出来（只含 version，因为只存 diff）
            mImpl->dirty = true;
        }

        mImpl->open = true;
        needFlush   = mImpl->dirty;
    }

    if (needFlush) flush();
    return isOpen();
}

void Config::flush() {
    std::lock_guard lock(mMutex);
    if (!mImpl || !mImpl->open || !mImpl->dirty) return;

    mImpl->dirty = false;
    std::error_code error;
    std::filesystem::create_directories(mImpl->path.parent_path(), error);
    if (!ll::file_utils::writeFile(mImpl->path, mImpl->data.dump(4))) {
        logger().warn("Failed to write the config file '{}'", mImpl->path.string());
    }
}

void Config::close() {
    flush();
    std::lock_guard lock(mMutex);
    if (!mImpl) return;
    mImpl->open = false;
    mImpl.reset();
}

bool Config::isOpen() const {
    std::lock_guard lock(mMutex);
    return mImpl && mImpl->open;
}

std::optional<double> Config::getNumber(std::string_view key) const {
    std::lock_guard lock(mMutex);
    if (!mImpl) return std::nullopt;

    auto const& settings = mImpl->data["settings"];
    auto const  iterator = settings.find(key);
    if (iterator == settings.end()) return std::nullopt;

    // 开关写成 true / false 也认。手改的时候这是最自然的写法，
    // 认下来总比静默当成「没设置」好 —— 后者看起来就像 mod 坏了。
    if (iterator->is_boolean()) return iterator->get<bool>() ? 1.0 : 0.0;

    if (!iterator->is_number()) return std::nullopt;
    return iterator->get<double>();
}

void Config::setNumber(std::string_view key, double value) {
    std::lock_guard lock(mMutex);
    if (!mImpl) return;

    auto& settings = mImpl->data["settings"];
    if (!settings.is_object()) settings = nlohmann::ordered_json::object();

    // 先查再写：不先插一个 null 进去，也避免持有引用跨过写入
    auto const iterator = settings.find(key);
    if (iterator != settings.end() && iterator->is_number() && iterator->get<double>() == value) {
        return; // 没变就别标脏
    }

    settings[std::string{key}] = value;
    mImpl->dirty               = true;
}

void Config::removeNumber(std::string_view key) {
    std::lock_guard lock(mMutex);
    if (!mImpl) return;
    if (mImpl->erase("settings", key)) mImpl->dirty = true;
}

std::vector<int> Config::getKeys(std::string_view name) const {
    std::lock_guard lock(mMutex);
    if (!mImpl) return {};

    auto const& keybinds = mImpl->data["keybinds"];
    auto const  iterator = keybinds.find(name);
    if (iterator == keybinds.end() || !iterator->is_array()) return {};

    std::vector<int> keys;
    keys.reserve(iterator->size());
    for (auto const& value : *iterator) {
        if (!value.is_number_integer()) return {};
        keys.push_back(value.get<int>());
    }
    return keys;
}

void Config::setKeys(std::string_view name, std::vector<int> const& keys) {
    bool changed = false;
    {
        std::lock_guard lock(mMutex);
        if (!mImpl) return;

        if (keys.empty()) {
            // 空 = 删除记录，回到代码里的默认键
            changed = mImpl->erase("keybinds", name);
        } else {
            if (!mImpl->data["keybinds"].is_object()) mImpl->data["keybinds"] = nlohmann::ordered_json::object();

            auto const& current  = mImpl->data["keybinds"];
            auto const  iterator = current.find(name);
            // 用 dump 做结构比较：手改出来的非法元素也不会抛异常
            changed = iterator == current.end() || iterator->dump() != nlohmann::ordered_json(keys).dump();
            if (changed) mImpl->data["keybinds"][std::string{name}] = keys;
        }

        mImpl->dirty = mImpl->dirty || changed;
    }

    // 改键是低频操作，而且丢了最影响体验，所以立即落盘
    if (changed) flush();
}

} // namespace mangrove::core

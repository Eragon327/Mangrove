#pragma once

#include "ll/api/mod/NativeMod.h"

#include <memory>

namespace mangrove::data {
class DataBase;
}

namespace mangrove {

class Mangrove {
    struct Impl;

public:
    static Mangrove& getInstance();

    Mangrove();
    ~Mangrove();

    Mangrove(Mangrove const&)            = delete;
    Mangrove& operator=(Mangrove const&) = delete;

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    /// mod 级资源（跟着 mod 生命周期走的东西）。
    /// 功能 manager 不在这里，它们仍是各自独立的单例（hook / 窗口过程都是静态上下文）。
    [[nodiscard]] data::DataBase& getDataBase();

    /// @return True if the mod is loaded successfully.
    bool load();

    /// @return True if the mod is enabled successfully.
    bool enable();

    /// @return True if the mod is disabled successfully.
    bool disable();

    /// @return True if the mod is unloaded successfully.
    bool unload();

private:
    std::unique_ptr<Impl> impl;
    ll::mod::NativeMod&   mSelf;
};

} // namespace mangrove

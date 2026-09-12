#pragma once

#include "ll/api/mod/NativeMod.h"

#include <memory>

namespace mangrove {

/// mod 入口。只做装配，不放业务逻辑。
class Mangrove {
    struct Impl;

public:
    static Mangrove& getInstance();

    Mangrove();
    ~Mangrove();

    Mangrove(Mangrove const&)            = delete;
    Mangrove& operator=(Mangrove const&) = delete;

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    /// @return True if the mod is loaded successfully.
    bool load();

    /// @return True if the mod is enabled successfully.
    bool enable();

    /// @return True if the mod is disabled successfully.
    bool disable();

    /// @return True if the mod is unloaded successfully.
    bool unload();

private:
    std::unique_ptr<Impl> mImpl;
    ll::mod::NativeMod&   mSelf;
};

} // namespace mangrove

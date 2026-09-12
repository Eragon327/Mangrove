#include "mangrove/Mangrove.h"

#include "mangrove/core/Config.h"
#include "mangrove/core/Feature.h"
#include "mangrove/features/freecamera/FreeCamera.h"
#include "mangrove/input/KeyManager.h"
#include "mangrove/ui/Overlay.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ClientCommandRegisterEvent.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/mod/RegisterHelper.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"

#include <memory>
#include <string>
#include <utility>

namespace mangrove {
namespace {

using ll::i18n_literals::operator""_tr;

/// 指令是否已经挂过。`ClientCommandRegisterEvent` 可能触发多次，
/// 而 `overload()` 每次都会新增一个重载，所以挂一次就够。
bool gCommandRegistered{};

/// 注册 `/mangrove`。热键失灵（比如改键改坏了）时的兜底入口。
///
/// 客户端重建指令表时才发 `ClientCommandRegisterEvent`，只有那时拿到的
/// `CommandRegistry` 才有效，所以注册写在事件回调里而不是 `enable()` 里。
void registerMenuCommand() {
    if (gCommandRegistered) return;

    auto& handle = ll::command::CommandRegistrar::getClientInstance().getOrCreateCommand(
        "mangrove",
        "mangrove.command.description"_tr(),
        CommandPermissionLevel::Any
    );
    handle.overload().execute([](::CommandOrigin const&, ::CommandOutput&) {
        ui::Overlay::getInstance().setVisible(true);
    });

    gCommandRegistered = true;
}

/// 功能登记表。
///
/// **新增一个功能**：在 `features/<名字>/` 里写一个 `Feature` 子类
/// （设置项、热键、行为全在它自己文件里），然后在这里加一行。
/// 菜单页、持久化、改变热键都不需要额外代码 —— 参考 `features/freecamera/`。
void addFeatures() {
    auto& manager = core::FeatureManager::getInstance();
    manager.add(features::FreeCamera::getInstance());
}

} // namespace

struct Mangrove::Impl {
    ll::event::ListenerPtr commandRegisterListener;
};

Mangrove& Mangrove::getInstance() {
    static Mangrove instance;
    return instance;
}

Mangrove::Mangrove() : mImpl(std::make_unique<Impl>()), mSelf(*ll::mod::NativeMod::current()) {}

Mangrove::~Mangrove() = default;

bool Mangrove::load() {
    auto& logger = getSelf().getLogger();

    // 1) 语言文件：src/lang -> <mod>/lang
    if (auto result = ll::i18n::getInstance().load(getSelf().getLangDir()); !result) {
        logger.error("Failed to load language files");
        result.error().log(logger);
    }

    // 2) 持久化：键位 + 各功能的设置值
    if (!core::Config::getInstance().load(getSelf().getConfigDir() / "config.json")) {
        logger.warn("Settings will not be persisted this session");
    }

    // 3) 登记功能
    addFeatures();

    // 4) 输入：先订阅键盘事件，再把功能热键（含玩家的改键）注册进去
    input::KeyManager::getInstance().install();
    core::FeatureManager::getInstance().install();

    // 5) 客户端指令
    mImpl->commandRegisterListener =
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientCommandRegisterEvent>(
            [](ll::event::ClientCommandRegisterEvent&) { registerMenuCommand(); }
        );
    if (!mImpl->commandRegisterListener) logger.error("Failed to listen for the client command register event");

    return true;
}

bool Mangrove::enable() {
    // 功能的反馈出口：功能只喊一声，由这里接到界面上
    core::FeatureManager::getInstance().setNotifier([](std::string message) {
        ui::Overlay::getInstance().notify(std::move(message));
    });

    if (!ui::Overlay::getInstance().install()) {
        getSelf().getLogger().error("Failed to install the UI overlay");
        return false;
    }

    return true;
}

bool Mangrove::disable() {
    // 顺序：先让功能收拾自己（摘钩子），再拆界面
    core::FeatureManager::getInstance().uninstall();
    ui::Overlay::getInstance().uninstall();

    // 界面没了，设置也不会再有改动，这时候落盘
    core::Config::getInstance().flush();
    return true;
}

bool Mangrove::unload() {
    if (mImpl->commandRegisterListener) {
        ll::event::EventBus::getInstance().removeListener(mImpl->commandRegisterListener);
        mImpl->commandRegisterListener.reset();
    }

    input::KeyManager::getInstance().uninstall();
    core::Config::getInstance().close();
    return true;
}

} // namespace mangrove

LL_REGISTER_MOD(mangrove::Mangrove, mangrove::Mangrove::getInstance());

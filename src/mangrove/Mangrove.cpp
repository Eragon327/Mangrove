#include "mangrove/Mangrove.h"
#include "mangrove/command/Command.h"
#include "mangrove/core/Feature.h"
#include "mangrove/data/DataBase.h"
#include "mangrove/gui/GuiOverlay.h"
#include "mangrove/input/KeyInputManager.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ClientCommandRegisterEvent.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/mod/RegisterHelper.h"

#include <memory>

namespace mangrove {

/// mod 级资源。
struct Mangrove::Impl {
    data::DataBase         mDataBase;
    ll::event::ListenerPtr mClientCommandRegisterListener;
};

Mangrove::Mangrove() : impl(std::make_unique<Impl>()), mSelf(*ll::mod::NativeMod::current()) {}

Mangrove::~Mangrove() = default;

Mangrove& Mangrove::getInstance() {
    static Mangrove instance;
    return instance;
}

data::DataBase& Mangrove::getDataBase() { return impl->mDataBase; }

bool Mangrove::load() {
    const auto& logger = getSelf().getLogger();

    // 1) 语言文件：src/lang -> <mod>/lang
    logger.debug("Loading i18n files...");
    if (!ll::i18n::getInstance().load(getSelf().getLangDir())) logger.error("Failed to load i18n files");

    // 2) 键值存储：按键绑定等运行期状态的持久化
    if (!getDataBase().open(getSelf().getDataDir() / "kv")) {
        logger.warn("Key-value storage is unavailable; nothing will be persisted");
    }

    // 3) 输入：订阅键盘事件
    input::KeyInputManager::getInstance().install();

    // 4) 功能：注册热键（会套上 DataBase 里存过的改键）
    core::FeatureManager::getInstance().install();

    // 5) 指令：客户端重建指令表时会发这个事件，只有那时拿到的 CommandRegistry 才有效，
    //    所以注册写在事件回调里，而不是 enable() 里直接调
    impl->mClientCommandRegisterListener =
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientCommandRegisterEvent>(
            [](ll::event::ClientCommandRegisterEvent&) { command::registerMenuCommand(); }
        );
    if (!impl->mClientCommandRegisterListener) {
        logger.error("Failed to listen for the client command register event");
    }

    return true;
}

bool Mangrove::enable() {
    if (!gui::GuiOverlay::getInstance().install()) {
        getSelf().getLogger().error("Failed to install the ImGui overlay");
        return false;
    }

    return true;
}

bool Mangrove::disable() {
    // 顺序：先让功能收拾自己（摘钩子），再拆 GUI
    core::FeatureManager::getInstance().uninstall();
    gui::GuiOverlay::getInstance().uninstall();
    return true;
}

bool Mangrove::unload() {
    if (impl->mClientCommandRegisterListener) {
        ll::event::EventBus::getInstance().removeListener(impl->mClientCommandRegisterListener);
        impl->mClientCommandRegisterListener.reset();
    }
    input::KeyInputManager::getInstance().uninstall();
    getDataBase().close();
    return true;
}

} // namespace mangrove

LL_REGISTER_MOD(mangrove::Mangrove, mangrove::Mangrove::getInstance());

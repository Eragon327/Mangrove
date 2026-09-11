#include "mangrove/command/Command.h"
#include "mangrove/gui/GuiOverlay.h"


#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/i18n/I18n.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"

namespace mangrove::command {
namespace {

using ll::i18n_literals::operator""_tr;

/// 指令是否已经挂过了。`ClientCommandRegisterEvent` 可能触发多次，而 `overload()`
/// 每次都会新增一个重载；LL 的 registrar 自己会把已登记的指令重新写进新的注册表，
/// 所以挂一次就够。
bool gRegistered{};

} // namespace

void registerMenuCommand() {
    if (gRegistered) return;

    // 本 mod 是客户端 mod，指令注册到客户端指令表，连别人的服务器也能用
    auto& handle = ll::command::CommandRegistrar::getClientInstance().getOrCreateCommand(
        "mangrove",
        "mangrove.command.description"_tr(),
        CommandPermissionLevel::Any
    );

    handle.overload().execute([](::CommandOrigin const&, ::CommandOutput&) {
        gui::GuiOverlay::getInstance().setVisible(true);
    });

    gRegistered = true;
}

} // namespace mangrove::command

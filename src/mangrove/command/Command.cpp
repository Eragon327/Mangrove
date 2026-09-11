#include "mangrove/command/Command.h"
#include "mangrove/Mangrove.h"
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

/// 本 mod 是客户端 mod（LL_PLAT_C），指令也要注册到客户端指令表，
/// 这样连到别人的服务器上也能用。
ll::command::CommandRegistrar& registrar() { return ll::command::CommandRegistrar::getClientInstance(); }

} // namespace

void registerMenuCommand() {
    auto& handle = registrar().getOrCreateCommand("mangrove", "Open the Mangrove menu", CommandPermissionLevel::Any);

    handle.overload().execute([](::CommandOrigin const&, ::CommandOutput&) {
        gui::GuiOverlay::getInstance().setVisible(true);
    });
}

} // namespace mangrove::command

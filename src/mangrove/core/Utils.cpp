#include "mangrove/core/Utils.h"

#include "ll/api/service/TargetedBedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/network/packet/TextPacket.h"


namespace mangrove::utils {

void sendTipsToClient(std::string const& message) {
    if (const auto client = ll::service::getClientInstance()) {
        TextPacket packet;
        packet.mBody = TextPacketPayload::MessageOnly{TextPacketType::Tip, message};
        packet.sendTo(*client->getLocalPlayer());
    }
}

} // namespace mangrove::utils
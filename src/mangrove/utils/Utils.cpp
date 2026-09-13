#include "mangrove/utils/Utils.h"

#include "mc/network/PacketSender.h"
#include "mc/network/packet/InventoryTransactionPacket.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/inventory/transaction/InventoryAction.h"


namespace mangrove::utils {

void sendInventorySwap(Player* player, int slot1, int slot2) {
    if (!player || slot1 == slot2) return;
    auto transaction = ComplexInventoryTransaction::fromType(ComplexInventoryTransaction::Type::NormalTransaction);
    if (!transaction) return;
    auto&                 invTx = transaction->mTransaction.get();
    InventorySource const source{
        InventorySourceType::ContainerInventory,
        ContainerID::Inventory,
        InventorySource::InventorySourceFlags::NoFlag
    };
    const auto& inventory = player->getInventory();
    ItemStack   item1     = inventory.getItem(slot1);
    ItemStack   item2     = inventory.getItem(slot2);
    invTx.addAction(InventoryAction{source, static_cast<uint>(slot1), item1, item2});
    invTx.addAction(InventoryAction{source, static_cast<uint>(slot2), item2, item1});
    InventoryTransactionPacket packet(InventoryTransactionPacketPayload{std::move(transaction), true});
    player->mPacketSender.sendToServer(packet);
}

} // namespace mangrove::utils
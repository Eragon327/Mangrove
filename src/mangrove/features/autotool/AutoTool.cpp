#include "mangrove/features/autotool/AutoTool.h"

#include "mangrove/Mangrove.h"
#include "mangrove/utils/Utils.h"

#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/SurvivalMode.h"
#include "mc/world/item/Item.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "ll/api/memory/Hook.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <optional>

namespace mangrove::features {
namespace {

/// 运行时状态。钩子跑在世界线程上，菜单在渲染线程上写设置 —— 两边只通过原子通信。
std::atomic_bool gEnabled{false};
std::atomic_int  gMinDamage{0};

/// 钩子是否已经装上。`hookEx` 对已挂钩的目标会报错，所以装过就不要再装。
std::atomic_bool gInstalled{false};

/// 快捷栏槽位数：不在手上的物品要先发交易包换进来，而不是直接 `setSelectedSlot`
constexpr int kHotbarSlots = 9;

auto& logger() { return Mangrove::getInstance().getSelf().getLogger(); }

struct ToolInfo {
    int value        = 0;
    int slot         = -1;
    int remainDamage = 0;
};

/// 物品的**剩余耐久**（最大耐久 - 已用掉的）。
#define GET_DAMAGE(item) ((item).mItem->getMaxDamage() - (item).getDamageValue())

/// 挑一把「最好的工具」：在剩余耐久够用的物品里取攻击力最高的那个。
/// @param minDamage 耐久下限，低于它的物品不碰 —— 免得拿一把马上要坏的镐子去挖
/// @return 槽位；没有合适的返回 -1
/// @note 目前只看攻击力，没看方块类型 / 挖掘速度（原实现如此，留待需要时扩展）。
int searchBestTool(Player const& player, int currentSlot, int minDamage) {
    auto const& inventory   = player.getInventory();
    auto const& currentItem = inventory.getItem(currentSlot);
    // 手上的够用就不换：换手本身有开销，也会打断连挖
    if (currentItem != ItemStack::EMPTY_ITEM() && GET_DAMAGE(currentItem) >= minDamage) return currentSlot;

    ToolInfo bestTool;
    for (int i = 0; i < inventory.getContainerSize(); ++i) {
        auto const& item = inventory.getItem(i);
        if (item == ItemStack::EMPTY_ITEM()) continue;
        if (GET_DAMAGE(item) < minDamage) continue;

        ToolInfo toolInfo;
        toolInfo.value        = item.mItem->getAttackDamage();
        toolInfo.slot         = i;
        toolInfo.remainDamage = GET_DAMAGE(item);
        if (toolInfo.value > bestTool.value) bestTool = toolInfo;
    }
    return bestTool.slot;
}

LL_TYPE_INSTANCE_HOOK(
    AutoToolHook,
    HookPriority::Normal,
    SurvivalMode,
    &SurvivalMode::$continueDestroyBlock,
    bool,
    ::BlockPos const& pos,
    uchar             face,
    ::Vec3 const&     playerPos,
    bool&             hasDestroyedBlock
) {
    // 钩子常驻：功能关着时入口先看旗标，为假就完全惰性（关闭状态零开销）
    if (!gEnabled.load(std::memory_order_acquire)) return origin(pos, face, playerPos, hasDestroyedBlock);

    int const currentSlot = mPlayer.getSelectedItemSlot();
    int const bestSlot    = searchBestTool(mPlayer, currentSlot, gMinDamage.load(std::memory_order_relaxed));
    if (bestSlot < 0 || bestSlot == currentSlot) return origin(pos, face, playerPos, hasDestroyedBlock);

    if (bestSlot >= kHotbarSlots) {
        utils::sendInventorySwap(&mPlayer, currentSlot, bestSlot);
    } else {
        mPlayer.setSelectedSlot(bestSlot);
    }
    return origin(pos, face, playerPos, hasDestroyedBlock);
}

/// 装钩子；装过就不再装（`hookEx` 对已挂钩的目标会报错，重复调会被当成失败）。
bool installHook() {
    if (gInstalled.load(std::memory_order_acquire)) return true;
    if (AutoToolHook::hook() != 0) return false;

    gInstalled.store(true, std::memory_order_release);
    logger().debug("Auto tool hook installed");
    return true;
}

} // namespace

// ---------------------------------------------------------------- 功能生命周期
AutoTool& AutoTool::getInstance() {
    static AutoTool instance;
    return instance;
}

AutoTool::AutoTool() : core::Feature("AutoTool") {
    add(mEnabled);
    add(mMinDamage);
}

/// 默认**空绑定**：快捷键页里列出这一行，但初始不占任何按键；玩家想用就自己绑一个。
/// （返回 `std::nullopt` 是「不占热键」，那连这一行都不会出现。）
std::optional<input::KeyBind> AutoTool::defaultHotkey() const { return input::KeyBind{}; }

void AutoTool::onHotkey() { apply(!mEnabled.enabled()); }

void AutoTool::onSettingsLoaded() {
    // 钩子**只在启动时装这一次**，之后一直留着。它在功能关着时是惰性的
    // （入口先看 `gEnabled`）—— 反复挂 / 卸会让 MinHook 每轮都「挂起全进程所有线程」。
    if (!installHook()) {
        logger().error("Unable to install the auto tool hook; the feature stays off");
        storeEnabled(false);
        return;
    }

    publishRuntimeConfig();
    // 上次退出时开着的，这次启动接着生效
    if (mEnabled.enabled()) apply(true);
}

void AutoTool::onSettingChanged(core::Setting& setting) {
    // 开关：走完整流程（钩子常驻，这里实际就是翻旗标 + 落盘）
    if (&setting == &mEnabled) {
        apply(mEnabled.enabled());
        return;
    }
    // 阈值之类：直接换值，钩子下一次进来读到的就是新值
    publishRuntimeConfig();
}

void AutoTool::onShutdown() {
    apply(false);
    // 只有卸载 mod 时才真的摘钩子
    if (gInstalled.exchange(false, std::memory_order_acq_rel)) (void)AutoToolHook::unhook();
}

void AutoTool::publishRuntimeConfig() const {
    // 声明区间是 [0, INT_MAX]，但 Config.json 能把上限调得更大 —— 落回 int 前再夹一道，
    // 否则超范围转 int 是 UB。
    gMinDamage.store(
        static_cast<int>(std::min(mMinDamage.value(), static_cast<double>(std::numeric_limits<int>::max()))),
        std::memory_order_relaxed
    );
}

void AutoTool::apply(bool enabled) {
    // 幂等：菜单勾选 → 改值 → `notifyChanged` → 又回到这里。
    // 判据是运行时旗标，而它的唯一写点就是本函数。
    if (enabled == gEnabled.load(std::memory_order_acquire)) return;

    // 钩子正常已经装好（启动时装过一次），这里只是兜底：装失败就退回关闭
    if (enabled && !installHook()) {
        logger().error("Unable to install the auto tool hook; the feature stays off");
        storeEnabled(false);
        core::FeatureManager::getInstance().notify(label("failed"));
        return;
    }

    // 先把旗标翻到新值：`storeEnabled()` 会经由 `onSettingChanged()` 再进来一次，
    // 那时幂等判据必须已经成立。参数跟着一起同步。
    gEnabled.store(enabled, std::memory_order_release);
    publishRuntimeConfig();

    storeEnabled(enabled);
    core::FeatureManager::getInstance().notify(label(enabled ? "on" : "off"));
}

void AutoTool::storeEnabled(bool enabled) {
    mEnabled.setValue(enabled ? 1.0 : 0.0);
    // 这一调会回到 onSettingChanged → apply()，由 apply 开头的幂等判断挡住
    core::FeatureManager::notifyChanged(*this, mEnabled);
}

} // namespace mangrove::features
#include "mangrove/features/freecamera/FreeCamera.h"

#include "mangrove/Mangrove.h"
#include "mangrove/features/freecamera/FreeCameraInternal.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/input/InputMode.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace mangrove::features {

namespace {

constexpr float kRadiansPerDegree = std::numbers::pi_v<float> / 180.0f;

/// 速度倍率为 1.0 时的移动速度（方块 / 秒）。原版走路约 4.3，自由相机给 12 起步比较好用。
constexpr float kBaseSpeed = 12.0f;

/// 单帧最大步进：卡顿一下之后不该瞬移一大段
constexpr float kMaxFrameSeconds = 0.25f;

/// 俯仰上下限，和原版一样留一点余量，避免在 ±90° 上翻转
constexpr float kMaxPitch = 89.0f;

/// 鼠标视角：每个鼠标计数转多少度。基准跟着原版鼠标灵敏度走，手感才一致。
constexpr float kLookDegreesAtZeroSensitivity = 0.05f;
constexpr float kLookDegreesPerSensitivity    = 0.25f;

/// 世界上方：空格 / 左Shift 是「上下平移」，不跟着俯仰走
Vec3 const kWorldUp{0.0f, 1.0f, 0.0f};

/// `Options::setPlayerViewPerspective` 的取值（同 `SharedTypes::v1_21_100::PlayerViewMode`）：
/// 0 = 第一人称，1 = 第三人称，2 = 第三人称（正面）。
///
/// 自由相机的相机姿态是我们自己写进去的，视角选项只影响「画不画自己的身体」：
/// 第一人称不画，第三人称画 —— 所以想要看见自己就用第三人称。
constexpr int kThirdPersonPerspective = 1;

auto& logger() { return Mangrove::getInstance().getSelf().getLogger(); }

float wrapDegrees(float degrees) {
    if (!std::isfinite(degrees)) return 0.0f;
    degrees = std::fmod(degrees, 360.0f);
    if (degrees > 180.0f) degrees -= 360.0f;
    if (degrees <= -180.0f) degrees += 360.0f;
    return degrees;
}

float lookDegreesPerCount() {
    float sensitivity = 0.5f;
    if (auto client = ll::service::getClientInstance()) {
        sensitivity = client->getOptions().getSensitivity(InputMode::Mouse);
    }
    return kLookDegreesAtZeroSensitivity + kLookDegreesPerSensitivity * std::clamp(sensitivity, 0.0f, 1.0f);
}

/// 世界是否已经加载（存档外不该开关功能 / 打开界面）
bool hasWorld() {
    auto client = ll::service::getClientInstance();
    return client && client->getLocalPlayer() != nullptr;
}

/// 姿态与输入。输入侧只写原子、姿态只在渲染线程读写 —— 见 `FreeCameraInternal.h`。
std::atomic_bool           gEnabled{false};
std::atomic<float>         gSpeed{1.0f};
std::atomic<std::uint32_t> gKeys{0};
std::atomic<float>         gLookDx{0.0f};
std::atomic<float>         gLookDy{0.0f};

/// 姿态只在渲染线程访问，不需要锁
std::optional<freecamera::Pose>       gPose;
std::chrono::steady_clock::time_point gLastFrame{};

} // namespace

// ---------------------------------------------------------------- 三轴换算
namespace freecamera {

Basis basisOf(Pose const& pose) {
    float const yaw      = pose.yaw * kRadiansPerDegree;
    float const pitch    = pose.pitch * kRadiansPerDegree;
    float const cosYaw   = std::cos(yaw);
    float const sinYaw   = std::sin(yaw);
    float const cosPitch = std::cos(pitch);
    float const sinPitch = std::sin(pitch);

    // 与原版一致：yaw 0 面向 +Z；forward 随 pitch 抬起 / 压下
    return {
        {-cosYaw,            0.0f,      -sinYaw          },
        {-sinYaw * sinPitch, cosPitch,  cosYaw * sinPitch},
        {-sinYaw * cosPitch, -sinPitch, cosYaw * cosPitch},
    };
}

} // namespace freecamera

namespace freecamera::state {

void setConfig(Config config) {
    gSpeed.store(
        std::isfinite(config.speed) ? std::clamp(config.speed, 0.01f, 100.0f) : 1.0f,
        std::memory_order_relaxed
    );

    // 关掉的时候顺手把输入清干净：下次启用不会带着上次的按住状态
    if (!config.enabled) {
        gKeys.store(0, std::memory_order_release);
        gLookDx.store(0.0f, std::memory_order_release);
        gLookDy.store(0.0f, std::memory_order_release);
    }
    gEnabled.store(config.enabled, std::memory_order_release);
}

bool active() { return gEnabled.load(std::memory_order_acquire); }

void setKey(std::uint32_t bit, bool down) {
    if (down) {
        gKeys.fetch_or(bit, std::memory_order_acq_rel);
    } else {
        gKeys.fetch_and(~bit, std::memory_order_acq_rel);
    }
}

void clearKeys() { gKeys.store(0, std::memory_order_release); }

void addLook(float dx, float dy) {
    // 累加而不是记最后一个：一帧里可能来好几个鼠标事件
    if (std::isfinite(dx)) gLookDx.fetch_add(dx, std::memory_order_acq_rel);
    if (std::isfinite(dy)) gLookDy.fetch_add(dy, std::memory_order_acq_rel);
}

void freezeInput() {
    clearKeys();
    gLookDx.store(0.0f, std::memory_order_release);
    gLookDy.store(0.0f, std::memory_order_release);
}

std::optional<Pose> update(bool frozen, std::optional<Pose> const& nativeStart) {
    if (!active()) {
        reset();
        return std::nullopt;
    }
    if (frozen) freezeInput();

    auto const now = std::chrono::steady_clock::now();

    // 启用后的第一帧：直接采用原生姿态（位置就是玩家眼睛），开启瞬间零跳变
    if (!gPose) {
        if (!nativeStart) return std::nullopt;
        gPose      = *nativeStart;
        gLastFrame = now;
        return gPose;
    }

    float dt = 0.0f;
    if (gLastFrame.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<float>(now - gLastFrame).count();
        dt = std::clamp(dt, 0.0f, kMaxFrameSeconds);
    }
    gLastFrame = now;

    if (!frozen && dt > 0.0f) {
        float const lookScale = lookDegreesPerCount();

        // 鼠标：与原版同向 —— 原版 yaw 增大是**右转**（面朝南时右转 = 面朝西），
        // 而 forward = (-sin yaw, .., cos yaw) 在 yaw 0→90° 正是 +Z 转向 -X，所以是 `+=`。
        // 下移 pitch 增大（原版 xRot 为正 = 低头）
        gPose->yaw   += gLookDx.exchange(0.0f, std::memory_order_acq_rel) * lookScale;
        gPose->pitch += gLookDy.exchange(0.0f, std::memory_order_acq_rel) * lookScale;
        gPose->pitch  = std::clamp(gPose->pitch, -kMaxPitch, kMaxPitch);
        gPose->yaw    = wrapDegrees(gPose->yaw);

        std::uint32_t const keys = gKeys.load(std::memory_order_acquire);
        if (keys != 0) {
            Basis const basis = basisOf(*gPose);

            // W/S 是**水平**前进：不跟着俯仰走。
            // 朝哪看就往哪飞会变成"想平移却在爬升"，非常难用；升降交给空格 / 左Shift。
            float const yawRadians = gPose->yaw * kRadiansPerDegree;
            Vec3 const  forwardXZ{-std::sin(yawRadians), 0.0f, std::cos(yawRadians)};

            Vec3 move{};
            if (keys & Forward) move += forwardXZ;
            if (keys & Back) move -= forwardXZ;
            if (keys & Right) move += basis.right;
            if (keys & Left) move -= basis.right;
            if (keys & Up) move += kWorldUp;
            if (keys & Down) move -= kWorldUp;

            // 归一化：斜着走不该比直着走快
            if (float const length = static_cast<float>(move.length()); length > 1.0e-4f) {
                Vec3 const  direction  = move.normalize();
                float const step       = kBaseSpeed * gSpeed.load(std::memory_order_relaxed) * dt;
                gPose->x              += direction.x * step;
                gPose->y              += direction.y * step;
                gPose->z              += direction.z * step;
            }
        }
    } else if (frozen) {
        // 冻结期间累积的鼠标位移也要丢掉，否则菜单关掉后会一次性转一大截
        gLookDx.store(0.0f, std::memory_order_release);
        gLookDy.store(0.0f, std::memory_order_release);
    }

    return gPose;
}

std::optional<Pose> peek() { return gPose; }

void reset() {
    gPose.reset();
    gLastFrame = {};
    freezeInput();
}

} // namespace freecamera::state

// ---------------------------------------------------------------- 功能生命周期
FreeCamera& FreeCamera::getInstance() {
    static FreeCamera instance;
    return instance;
}

FreeCamera::FreeCamera() : core::Feature("FreeCamera") {
    add(mEnabled);
    add(mSpeed);
}

std::optional<input::KeyBind> FreeCamera::defaultHotkey() const { return input::KeyBind::single(VK_F); }

void FreeCamera::onHotkey() {
    bool const wantEnabled = !mEnabled.enabled();
    // 只拦"开启"：关掉总是允许的（哪怕已经退到主菜单，玩家也该能把它关掉）
    if (wantEnabled && !hasWorld()) {
        logger().debug("Ignoring the free camera hotkey because no world is loaded");
        return;
    }
    apply(wantEnabled);
}

void FreeCamera::onSettingsLoaded() {
    // 钩子**只在启动时装这一次**，之后一直留着。
    // 它们在功能关着的时候是惰性的（各个钩子入口先看 `state::active()`，为假就直接返回），
    // 而每次开关都挂/卸一遍要付出 MinHook 逐个"挂起全进程所有线程 → 改指令 → 恢复"的代价 ——
    // 8 个钩子就是 8 轮全线程暂停，那才是"按 F 卡一下"的主要来源。
    if (!freecamera::hooks::install()) {
        logger().error("Unable to install the free camera hooks; the feature stays off");
        storeEnabled(false);
        return;
    }

    // 上次退出时开着的功能，这次启动接着生效。
    // 启动时可能还没有世界、拿不到玩家姿态 —— 姿态由渲染线程在第一帧补。
    if (mEnabled.enabled()) apply(true);
}

void FreeCamera::onSettingChanged(core::Setting& setting) {
    // 开关：走完整的接管 / 交还流程
    if (&setting == &mEnabled) apply(mEnabled.enabled());
    // 速度之类：直接换值就行（渲染侧每帧读原子）
    publishRuntimeConfig();
}

void FreeCamera::onShutdown() {
    apply(false);
    // 兜底：`apply()` 可能因为"本来就没开"直接返回，但渲染选项必须保证交还
    restoreRenderOptions();
    // 只有卸载 mod 时才真的摘钩子
    (void)freecamera::hooks::uninstall();
}

void FreeCamera::publishRuntimeConfig() const {
    freecamera::state::setConfig({
        mEnabled.enabled(),
        static_cast<float>(mSpeed.value()),
    });
}

void FreeCamera::apply(bool enabled) {
    // 幂等：菜单勾选 → 改值 → 通知 → 又回到这里，会再进来一次。
    // 判据是「渲染侧现在开没开」（`state::active()`）：它只由 `publishRuntimeConfig()` 写，
    // 而后者只从这里和 `onSettingChanged()` 调，所以它精确等于"最后一次生效的状态"。
    // （钩子现在常驻，以前那个「钩子装没装上」的判据会恒为真，不能再用。）
    if (enabled == freecamera::state::active()) return;

    // 先发旗标：渲染侧看到它就会开始 / 停止覆写相机
    publishRuntimeConfig();

    if (!enabled) {
        restoreRenderOptions();
        // 还按着的移动键重新按下，玩家不用松手就能接着走
        freecamera::hooks::handOverMovementKeys(false);
        // 必须回写开关值：不回写的话菜单里那个勾还在，下次按热键也会因为
        // “已经关着”而直接返回，看起来就是“关掉后再也开不起来”。
        storeEnabled(false);
        core::FeatureManager::getInstance().notify(label("off"));
        return;
    }

    // 钩子已常驻（启动时装好），这里只是兜底：启动时装失败的话，每次开启再试一次
    if (!freecamera::hooks::install()) {
        logger().error("Unable to install the free camera hooks; the feature stays off");
        storeEnabled(false);
        core::FeatureManager::getInstance().notify(label("failed"));
        return;
    }

    takeOverRenderOptions();
    // 按着 W 就按 F 的情况：把按着的键抾起（实体留在原地）并喂给相机（不用松手就能飞）
    freecamera::hooks::handOverMovementKeys(true);
    storeEnabled(true);
    core::FeatureManager::getInstance().notify(label("on"));
}

void FreeCamera::storeEnabled(bool enabled) {
    mEnabled.setValue(enabled ? 1.0 : 0.0);
    // 这一调会回到 onSettingChanged → apply()，由 apply 开头的幂等判断挡住
    core::FeatureManager::notifyChanged(*this, mEnabled);
}

void FreeCamera::takeOverRenderOptions() {
    auto client = ll::service::getClientInstance();
    if (!client) return;

    auto& options = client->getOptions();

    // 玩家的原值**只记一次**，之后一直沿用：万一某次没还干净（拿不到客户端 / 进程被强杀），
    // 也不会把"我们自己设的值"当成玩家的原意记下来 —— 那正是"视角再也回不来"的成因。
    if (!mSavedViewPerspective) mSavedViewPerspective = options.getPlayerViewPerspective();

    // **第三人称**：让玩家能看见自己的身体。这是引擎自己渲染的本地玩家真人模型，
    // 所以动作、手持物、身上穿的盔甲都是真的（不像另建一个假实体，只能站着不动）。
    //
    // 那只「跟着相机飞的手」是第一人称的视图模型，第三人称根本不画，所以不用再藏它。
    //
    // 这里**无条件写**，不要加"get() 和目标值不同才写"那种化化 —— 那等于假设这个选项的
    // 读和写是一对完好的接口。实测下来不是：写进去之后 `get()` 可能仍然返回旧值，
    // 于是"还原"那一步会被那个判断直接跳过，视角就再也回不来了。写一遍的开销是 0.0ms（量过）。
    options.setPlayerViewPerspective(kThirdPersonPerspective);
}

void FreeCamera::restoreRenderOptions() {
    auto client = ll::service::getClientInstance();
    if (!client) return; // 拿不到客户端：**保留记录**，留给下次（下次开关 / 卸载）再还

    // 同样无条件写，理由同上
    if (mSavedViewPerspective) client->getOptions().setPlayerViewPerspective(*mSavedViewPerspective);
}

} // namespace mangrove::features

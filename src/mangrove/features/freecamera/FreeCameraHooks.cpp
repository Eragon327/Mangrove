#include "mangrove/features/freecamera/FreeCamera.h"

#include "mangrove/Mangrove.h"
#include "mangrove/features/freecamera/FreeCameraInternal.h"
#include "mangrove/input/KeyManager.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/memory/LinearAllocator.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/input/Keyboard.h"
#include "mc/deps/input/MouseAction.h"
#include "mc/deps/input/MouseDevice.h"
#include "mc/deps/input/win/HIDControllerGameCoreDesktop.h"
#include "mc/deps/minecraft_camera/CameraRegistry.h"
#include "mc/deps/minecraft_camera/components/ActiveCameraComponent.h"
#include "mc/deps/minecraft_camera/components/CameraComponent.h"
#include "mc/deps/minecraft_camera/components/RenderCameraComponent.h"
#include "mc/deps/renderer/Camera.h"


#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace mangrove::features::freecamera {
namespace {

/// 覆写之后读回来比对用的容差：差得比这多就算没写进去
constexpr float kPositionTolerance  = 0.02f;
constexpr float kDirectionTolerance = 0.002f;

std::atomic_bool gInstalled{};
/// 校验失败只报一次，否则每帧一条会把日志冲爆
std::atomic_bool gFailureLogged{};

/// 用来识别「换世界 / 换维度」：玩家对象或维度变了就得把姿态丢掉重建
LocalPlayer* gLastPlayer{};
int          gLastDimension{};

auto& logger() { return Mangrove::getInstance().getSelf().getLogger(); }

// ---------------------------------------------------------------- 向量
/// 引擎的两种向量之间搭个桥：`mce::Camera` 用 glm，`LevelRendererPlayer` 用自己的 `Vec3`。
/// 其余计算一律用 `Vec3` —— 它自带 `dot` / `length` / `normalize` / `abs`。
Vec3      toVec3(glm::vec3 const& value) { return {value.x, value.y, value.z}; }
glm::vec3 toGlm(Vec3 const& value) { return {value.x, value.y, value.z}; }

bool finite(Vec3 const& value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }

/// 分量的最大绝对差；任一侧不是有限值就算「完全不一致」
float maxError(Vec3 const& actual, Vec3 const& expected) {
    auto const delta = ll::math::abs(actual - expected);
    if (!finite(delta)) return std::numeric_limits<float>::infinity();
    return std::max({delta.x, delta.y, delta.z});
}

/// 三轴 → 四元数。相机 ECS 存的是朝向四元数，不是矩阵。
glm::qua<float> orientationFromBasis(Basis const& basis) {
    // 相机空间的三个轴构成"相机→世界"旋转；注意 `Vec3` 没有一元负号，逐分量取反
    Vec3 const      back{-basis.forward.x, -basis.forward.y, -basis.forward.z};
    glm::mat3 const cameraToWorld{toGlm(basis.right), toGlm(basis.up), toGlm(back)};
    return glm::normalize(glm::quat_cast(cameraToWorld));
}

// ---------------------------------------------------------------- 相机写入
/// 视图矩阵：世界坐标 → 相机空间。
/// `mce::Camera` 用的是 glm 的列主序矩阵，所以 `result[i]` 是第 i **列**。
glm::mat4x4 viewMatrix(Vec3 const& position, Basis const& basis) {
    auto result = glm::mat4x4{1.0f};
    result[0]   = {basis.right.x, basis.up.x, -basis.forward.x, 0.0f};
    result[1]   = {basis.right.y, basis.up.y, -basis.forward.y, 0.0f};
    result[2]   = {basis.right.z, basis.up.z, -basis.forward.z, 0.0f};
    result[3]   = {
        static_cast<float>(-basis.right.dot(position)),
        static_cast<float>(-basis.up.dot(position)),
        static_cast<float>(basis.forward.dot(position)),
        1.0f,
    };
    return result;
}

/// 逆视图矩阵的平移列就是相机位置；第三列的负方向就是相机朝向
Vec3 inverseViewPosition(mce::Camera const& camera) {
    auto const& inverse = camera.mInverseViewMatrix.get();
    return {inverse[3].x, inverse[3].y, inverse[3].z};
}

Vec3 inverseViewForward(mce::Camera const& camera) {
    auto const& inverse = camera.mInverseViewMatrix.get();
    return Vec3{-inverse[2].x, -inverse[2].y, -inverse[2].z}.normalize();
}

/// 写入相机姿态。
/// @param position 世界坐标；传 `{}` 表示这个相机是**相机空间**的（位置恒为原点）
void writeCameraPose(mce::Camera& camera, Vec3 const& position, Basis const& basis, glm::mat4x4 const& view) {
    // FOV / 宽高比原样保留：自由视角不动 FOV，接管它只会多一个出问题的点
    float const nativeFov    = camera.mFov;
    float const nativeAspect = camera.mAspectRatio;

    camera.mPosition    = toGlm(position);
    camera.mRight       = toGlm(basis.right);
    camera.mUp          = toGlm(basis.up);
    camera.mForward     = toGlm(basis.forward);
    camera.mFov         = nativeFov;
    camera.mAspectRatio = nativeAspect;

    auto& stack = camera.viewMatrixStack->stack.get();
    if (!stack.empty()) {
        stack.back()._m                  = view;
        camera.viewMatrixStack->_isDirty = true;
    }
    // 必须**在写完字段之后**调：它会根据相机自身状态重建依赖
    // （逆视图矩阵 / 视锥），先调的话拿到的还是原生朝向 —— 表现就是"位置对了、朝向不跟着转"。
    camera.updateViewMatrixDependencies();
}

/// 世界空间的相机：位置就是世界坐标，视图矩阵 = 世界 → 相机。
/// 视锥 / 剔除 / 排序 / 天空盒全部由它派生，**它是唯一一处要写世界坐标的地方**。
void writeWorldCamera(mce::Camera& camera, Vec3 const& position, Basis const& basis, glm::mat4x4 const& view) {
    writeCameraPose(camera, position, basis, view);
}

/// 相机空间的相机（`setupCamera` 的入参、`ClientInstance::getCamera()`）：**位置恒为 0**。
///
/// 引擎自己会把两份合成世界相机（`mWorldSpaceCamera = mCameraPos + 这一份`）。
/// 往这里写世界坐标等于把位置算了两遍：世界相机跑到 **2 倍**坐标上，
/// 视锥跟着搬到别处 —— 表现就是「相机在动、剔除基准却在别的地方」。
/// 朝向仍必须写：合成出来的世界相机用的就是这一份朝向。
void writeRelativeCamera(mce::Camera& camera, Basis const& basis) {
    writeCameraPose(camera, Vec3{}, basis, viewMatrix(Vec3{}, basis));
}

/// 剔除与排序用的是这几个字段（相机 + 目标点 + 前后 / 上方向），
/// 只改渲染用的相机是不够的：不写它们，转头之后远处的区块会按玩家的位置被剔掉。
void writeLevelCameraPose(LevelRendererPlayer& level, Vec3 const& position, Basis const& basis) {
    level.mCameraPos = position;
    // `mCameraTargetPos` **不是**"往哪看"，而是「相机挂靠的那个点」：
    // 第一人称时它就是相机位置本身 —— 原生状态下 `target == mCameraPos`，探针里直接能看到。
    // 写成 `position + forward`（多走一格）会让渲染用的相机整体前进一格：
    // 表现就是"按下开关的瞬间画面往前突进一小段"，而且剔除用的 `mCameraPos` 与实际渲染的
    // 相机差一格，眼前一小块区块会被误剔。方向沿视线，所以看着就是"往前"。
    level.mCameraTargetPos = position;
    level.mCameraForward   = basis.forward;
    level.mCameraUp        = basis.up;
}

/// 相机 ECS：引擎自己的相机描述。
/// @note 只写"相机在哪、往哪看"这两件事，**不碰** `mSavedModelView` 之类的缓存矩阵：
///       它们由引擎自己维护，手写会让依赖它们的 pass（阴影 / 遮挡剔除）算错 ——
///       表现就是眼前一小块区块被错误剔除。
void writeCameraEcs(Vec3 const& position, Basis const& basis) {
    auto client   = ll::service::getClientInstance();
    auto registry = client ? client->getCameraRegistry() : Bedrock::NonOwnerPointer<CameraRegistry>{};
    if (!registry) return;

    auto const orientation = orientationFromBasis(basis);
    auto const enginePos   = toGlm(position);

    auto applyEntity = [&](EntityContext& entity, bool gameCamera) {
        auto* component = entity.tryGetComponent<MinecraftCamera::CameraComponent>().as_ptr();
        if (!component) return;
        // 只碰游戏相机和"正在生效"的相机实体，其余（预设相机等）不动
        if (!gameCamera && !entity.hasComponent<MinecraftCamera::ActiveCameraComponent>()
            && !entity.hasComponent<MinecraftCamera::RenderCameraComponent>()) {
            return;
        }
        component->mPosition    = enginePos;
        component->mOrientation = orientation;
    };

    if (auto& gameCamera = registry->mGameCamera.get(); gameCamera) applyEntity(*gameCamera, true);
    for (auto& cameraEntity : registry->mCameraEntities.get()) {
        if (cameraEntity) applyEntity(*cameraEntity, false);
    }
}

// ---------------------------------------------------------------- 校验
struct Verification {
    float positionError{std::numeric_limits<float>::infinity()};
    float directionError{std::numeric_limits<float>::infinity()};
    float viewPositionError{std::numeric_limits<float>::infinity()};
    float viewDirectionError{std::numeric_limits<float>::infinity()};

    [[nodiscard]] bool valid() const {
        return positionError <= kPositionTolerance && directionError <= kDirectionTolerance
            && viewPositionError <= kPositionTolerance && viewDirectionError <= kDirectionTolerance;
    }
};

Verification verifyCamera(mce::Camera const& camera, Vec3 const& position, Basis const& basis) {
    return {
        maxError(toVec3(camera.mPosition.get()), position),
        maxError(toVec3(camera.mForward.get()).normalize(), basis.forward),
        maxError(inverseViewPosition(camera), position),
        maxError(inverseViewForward(camera), basis.forward),
    };
}

/// 把姿态写进这一帧要用的所有相机，然后把结果读回来确认真的生效了。
/// 「写不进去」在别的 mod 抢同一个钩子时会真的发生，所以不能只写不验。
void applyPose(LevelRendererPlayer& level, mce::Camera& setupCamera, Pose const& pose) {
    Vec3 const  position{pose.x, pose.y, pose.z};
    Basis const basis = basisOf(pose);
    if (!finite(position) || !finite(basis.right) || !finite(basis.up) || !finite(basis.forward)) return;

    auto  client       = ll::service::getClientInstance();
    auto* clientCamera = client ? &client->getCamera() : nullptr;
    auto* worldCamera  = &level.mWorldSpaceCamera.get();

    writeWorldCamera(*worldCamera, position, basis, viewMatrix(position, basis));

    // 相机空间的两份：位置一律写 0，交给引擎去合成世界相机
    writeRelativeCamera(setupCamera, basis);
    bool const clientIsLocal = clientCamera && clientCamera != &setupCamera && clientCamera != worldCamera;
    if (clientIsLocal) writeRelativeCamera(*clientCamera, basis);

    writeLevelCameraPose(level, position, basis);
    writeCameraEcs(position, basis);

    // 期望值：世界空间那份是 `position`，相机空间那两份都是原点
    bool const setupOk    = verifyCamera(setupCamera, Vec3{}, basis).valid();
    auto const worldCheck = verifyCamera(*worldCamera, position, basis);
    bool const clientOk   = !clientIsLocal || verifyCamera(*clientCamera, Vec3{}, basis).valid();

    float const levelPositionError = maxError(level.mCameraPos.get(), position);
    // 方向要拿 `mCameraForward` 验，**不能**用 `mCameraTargetPos - mCameraPos`：
    // 后者只有在"我们把 target 写成前方一格"的那种错法下才等于视线方向，
    // 而正确写法（target 与 pos 是同一个点）差向量是零向量，归一化直接 NaN → 恒报 inf。
    // 那种验法其实是在验我们自己的约定，不是引擎的 —— 修好 bug 后它反而天天误报。
    float const levelDirectionError = maxError(level.mCameraForward.get().normalize(), basis.forward);

    bool const verified = setupOk && worldCheck.valid() && clientOk && levelPositionError <= kPositionTolerance
                       && levelDirectionError <= kDirectionTolerance;
    if (verified) return;
    if (gFailureLogged.exchange(true, std::memory_order_acq_rel)) return;

    logger().error(
        "Free camera pose did not reach the renderer (world=({}, {}, {}, {}), level=({}, {}), setup={}, client={})",
        worldCheck.positionError,
        worldCheck.viewPositionError,
        worldCheck.directionError,
        worldCheck.viewDirectionError,
        levelPositionError,
        levelDirectionError,
        setupOk,
        clientOk
    );
}

// ---------------------------------------------------------------- 起点
/// 玩家眼睛 + 玩家朝向。兜底用：拿不到返回 `nullopt`。
std::optional<Pose> playerEyePose() {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return std::nullopt;

    auto const eye      = player->getEyePos();
    auto const rotation = player->getRotation(); // Vec2{ pitch, yaw }
    if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z) || !std::isfinite(rotation.x)
        || !std::isfinite(rotation.y)) {
        return std::nullopt;
    }

    Pose pose;
    pose.x     = eye.x;
    pose.y     = eye.y;
    pose.z     = eye.z;
    pose.pitch = rotation.x;
    pose.yaw   = rotation.y;
    return pose;
}

/// 启用后第一帧的起点：**引擎这一帧正在用的那份世界相机**。
///
/// 为什么不是玩家眼睛：眼睛是 tick 之后的值，而渲染用它和上一个 tick 之间**插值**出来的相机，
/// 两者差着小半格。拿眼睛当起点，开启瞬间画面就会往前「突进」一小段。
/// 拿引擎自己的相机当起点则恒为零跳变：起点就是屏幕上此刻那个位置。
///
/// 朝向也从这份相机反解（forward → yaw/pitch），理由同上：朝向也是插值出来的。
/// 数值不合法（世界刚加载 / 相机还没算过）才退回玩家眼睛。
std::optional<Pose> nativeStartPose(LevelRendererPlayer& level) {
    constexpr float kDegreesPerRadian = 57.2957795f;

    auto const eye = playerEyePose();

    // 原版相机约定（与 `basisOf` 一致）：forward = (-sin yaw*cos pitch, -sin pitch, cos yaw*cos pitch)
    auto const& world = level.mWorldSpaceCamera.get();
    auto const  pos   = toVec3(world.mPosition.get());
    auto const  fwd   = toVec3(world.mForward.get());
    if (finite(pos) && finite(fwd) && fwd.lengthSqr() > 1.0e-6) {
        Vec3 const  dir   = fwd.normalize();
        float const pitch = -std::asin(std::clamp(dir.y, -1.0f, 1.0f)) * kDegreesPerRadian;
        float const yaw   = std::atan2(-dir.x, dir.z) * kDegreesPerRadian;
        if (std::isfinite(yaw) && std::isfinite(pitch)) {
            Pose pose;
            pose.x     = pos.x;
            pose.y     = pos.y;
            pose.z     = pos.z;
            pose.yaw   = yaw;
            pose.pitch = pitch;
            return pose;
        }
    }

    return eye;
}

/// 只刷新"引擎自己那份相机状态"（世界相机 + 剔除字段 + ECS），不碰 setup 的入参。
/// 给 `tickLevelRendererCamera` / `updateViewArea` 用：这两个阶段也会读相机算剔除 / 排序，
/// 必须让它们看到同一份姿态，否则剔除基准就慢半拍 —— 表现就是区块时隐时现。
void refreshEngineCamera(LevelRendererPlayer& level, Pose const& pose) {
    Vec3 const  position{pose.x, pose.y, pose.z};
    Basis const basis = basisOf(pose);
    if (!finite(position) || !finite(basis.right) || !finite(basis.up) || !finite(basis.forward)) return;

    writeCameraPose(level.mWorldSpaceCamera.get(), position, basis, viewMatrix(position, basis));
    writeLevelCameraPose(level, position, basis);
    writeCameraEcs(position, basis);
}

/// 游戏是否自己抓着鼠标。
/// `grabMouse` / `releaseMouse` 就是引擎的"进入游戏操作 / 打开界面"信号，
/// 比去猜 `hud_screen` 这类屏幕名可靠得多。
std::atomic_bool gGameHasMouse{true};

LL_TYPE_INSTANCE_HOOK(
    FreeCameraGrabMouseHook,
    ll::memory::HookPriority::Lowest,
    ClientInstance,
    &ClientInstance::$grabMouse,
    void
) {
    gGameHasMouse.store(true, std::memory_order_release);
    origin();
}

LL_TYPE_INSTANCE_HOOK(
    FreeCameraReleaseMouseHook,
    ll::memory::HookPriority::Lowest,
    ClientInstance,
    &ClientInstance::$releaseMouse,
    void
) {
    gGameHasMouse.store(false, std::memory_order_release);
    origin();
}

/// 只在「世界内 + 游戏自己抓着鼠标」时吞输入。
/// 游戏松开鼠标 = 有界面开着（暂停菜单 / 背包 / 聊天 / 加载），那时输入归界面：
/// 再吞下去就是「暂停菜单里鼠标动不了、点一下只会关掉菜单」。
bool shouldCaptureInput() {
    if (!state::active()) return false;
    auto client = ll::service::getClientInstance();
    if (!client || !client->getLocalPlayer()) return false;
    return gGameHasMouse.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------- 输入钩子
/// 被自由相机接管的移动键：键码 ↔ 相机按键位
struct MovementKey {
    int           keyCode;
    std::uint32_t bit;
};

constexpr MovementKey kMovementKeys[] = {
    {Keyboard::W,      state::Forward},
    {Keyboard::S,      state::Back   },
    {Keyboard::A,      state::Left   },
    {Keyboard::D,      state::Right  },
    {Keyboard::Space,  state::Up     },
    {Keyboard::Lshift, state::Down   },
};

/// 物理按住的移动键。**不管功能开没开都记**。
///
/// 按着 W 再按 F 时，要把"谁在按键"这件事交接干净：
///   - 游戏那边要把已经按着的键**抾起**，否则实体会一直往前走，而相机留在原地；
///   - 相机这边要把它们当**按着**，否则得先松手再按一次才能飞。
/// 两件事都靠这个掩码。
std::atomic<std::uint32_t> gPhysicalKeys{};

/// 最后一次见到的 HID 控制器实例（从按键钩子里取，那时它一定活着），补发事件用它。
std::atomic<HIDControllerGameCoreDesktop*> gHidController{};

/// 标记"这一发按键是我们替游戏补发的"：要原样放行，也不能污染物理掩码。
std::atomic_bool gSyntheticKey{false};

std::uint32_t movementBit(int keyCode) {
    for (auto const& key : kMovementKeys) {
        if (key.keyCode == keyCode) return key.bit;
    }
    return 0;
}

/// 吞掉鼠标**移动**（视角），放行鼠标**按键**。
///
/// 按键放行的理由：`MouseInputEvent` 是 LeviLamina 钩在同一个函数上、
/// 以 `HookPriority::Normal` 发布的。在这里把按键也拦掉，鼠标绑定的热键就再也不响应了。
/// 代价是自由视角时左键仍会作用于玩家原位 —— 这是刻意的取舍。
LL_TYPE_INSTANCE_HOOK(
    FreeCameraMouseHook,
    ll::memory::HookPriority::Highest,
    MouseDevice,
    &MouseDevice::feed,
    void,
    char  actionButtonId,
    schar buttonData,
    short x,
    short y,
    short dx,
    short dy,
    bool  forceMotionlessPointer
) {
    bool const motion = actionButtonId == MouseAction::ActionMove || actionButtonId == MouseAction::ActionMoveRelative;
    if (motion && shouldCaptureInput()) {
        state::addLook(static_cast<float>(dx), static_cast<float>(dy));
        return;
    }
    origin(actionButtonId, buttonData, x, y, dx, dy, forceMotionlessPointer);
}

LL_TYPE_INSTANCE_HOOK(
    FreeCameraKeyDownHook,
    ll::memory::HookPriority::Highest,
    HIDControllerGameCoreDesktop,
    &HIDControllerGameCoreDesktop::$onKeyDown,
    void,
    int                                                 keyCode,
    Bedrock::Input::KeyboardEventProcessor::InputOrigin originType
) {
    // 补发的按键：原样放行，也不碰掩码（物理上键还按着，掩码不能动）
    if (gSyntheticKey.load(std::memory_order_acquire)) {
        origin(keyCode, originType);
        return;
    }

    std::uint32_t const bit = movementBit(keyCode);
    if (bit != 0) {
        gHidController.store(static_cast<HIDControllerGameCoreDesktop*>(this), std::memory_order_release);
        gPhysicalKeys.fetch_or(bit, std::memory_order_acq_rel);
    }

    if (bit != 0 && shouldCaptureInput()) {
        state::setKey(bit, true);
        return;
    }
    origin(keyCode, originType);
}

LL_TYPE_INSTANCE_HOOK(
    FreeCameraKeyUpHook,
    ll::memory::HookPriority::Highest,
    HIDControllerGameCoreDesktop,
    &HIDControllerGameCoreDesktop::$onKeyUp,
    void,
    int keyCode
) {
    // 补发的按键：原样放行，也不碰掩码
    if (gSyntheticKey.load(std::memory_order_acquire)) {
        origin(keyCode);
        return;
    }

    std::uint32_t const bit = movementBit(keyCode);
    if (bit != 0) {
        gHidController.store(static_cast<HIDControllerGameCoreDesktop*>(this), std::memory_order_release);
        gPhysicalKeys.fetch_and(~bit, std::memory_order_acq_rel);
    }

    if (bit != 0 && shouldCaptureInput()) {
        state::setKey(bit, false);
        return;
    }
    origin(keyCode);
}

// ---------------------------------------------------------------- 相机钩子
/// 每帧的注入点：推进姿态 → 覆写相机。
///
/// 用 `Lowest` 优先级：让别的（原生 / 其他 mod）的钩子先跑，我们在最后覆盖，
/// 这样看到的 `camera` 已经是这一帧算好的原生结果，起点与校验都以它为准。
LL_TYPE_INSTANCE_HOOK(
    FreeCameraSetupHook,
    ll::memory::HookPriority::Lowest,
    LevelRendererPlayer,
    &LevelRendererPlayer::setupCamera,
    void,
    mce::Camera& camera,
    float        partialTick
) {
    origin(camera, partialTick);

    // 钩子常驻，所以「关着」的时候也必须在**这里**把旧姿态丢掉。
    //
    // 以前关功能会走 `hooks::uninstall()` → `state::reset()`，现在钩子一直挂着，
    // 如果这里直接 return，`state::update()` 就再也不会被调用、它里面那句
    // "没开就 reset" 也就永远跑不到 —— 关闭时留下的姿态会一直躺在那儿，
    // 下次开启时 `peek()` 有值 → 不重建起点 → 表现就是"记住上次退出的位置"。
    if (!state::active()) {
        if (state::peek()) state::reset();
        return;
    }

    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;

    auto& level = *static_cast<LevelRendererPlayer*>(this);

    // 换世界 / 换维度 / 死亡重生：玩家对象或维度变了，旧姿态在新世界里毫无意义。
    // 顺便把"按住的移动键"也清掉：换世界的瞬间手还按着 W 的话，掩码会一直留到下次开启，
    // 那时候自由相机会自己往前飘（而我们以为玩家还按着）。
    // HID 实例指针也一起丢：它可能已经换了主人。
    int const dimension = player ? static_cast<int>(player->getDimensionId()) : 0;
    if (player != gLastPlayer || dimension != gLastDimension) {
        gLastPlayer    = player;
        gLastDimension = dimension;
        state::reset();
        gPhysicalKeys.store(0, std::memory_order_release);
        gHidController.store(nullptr, std::memory_order_release);
    }
    if (!player) return;

    // 输入归界面（我们的菜单 / 游戏的暂停菜单 / 背包…）时冻结：
    // 清掉按住状态、姿态原地不动 —— 菜单背后的画面不会跳回玩家视角，
    // 而且那种时候我们根本不吞输入，暂停菜单的鼠标还能正常用。
    bool const frozen = !shouldCaptureInput() || input::KeyManager::getInstance().isSuppressed();

    std::optional<Pose> nativeStart;
    if (!state::peek()) nativeStart = nativeStartPose(level);

    if (auto const pose = state::update(frozen, nativeStart)) applyPose(level, camera, *pose);
}

/// 引擎每 tick 也会自己写一次相机（`tickLevelRendererCamera`）。
/// 我们在它之后跟一遍：否则剔除 / 排序会看到"上一帧的相机"。
LL_TYPE_INSTANCE_HOOK(
    FreeCameraTickCameraHook,
    ll::memory::HookPriority::Lowest,
    LevelRendererPlayer,
    &LevelRendererPlayer::$tickLevelRendererCamera,
    void
) {
    origin();

    if (!state::active()) return;

    auto&      level = *static_cast<LevelRendererPlayer*>(this);
    auto const pose  = state::peek();
    if (pose) refreshEngineCamera(level, *pose);
}

/// 视区（ = 剔除基准）就是在这里算的。
/// **在 `origin()` 之前**把相机刷成我们的姿态，算出来的 view area 才跟得上相机。
LL_TYPE_INSTANCE_HOOK(
    FreeCameraUpdateViewAreaHook,
    ll::memory::HookPriority::Lowest,
    LevelRendererPlayer,
    &LevelRendererPlayer::$updateViewArea,
    void,
    ::LevelRenderPreRenderUpdateParameters const& parameters
) {
    auto&      level = *static_cast<LevelRendererPlayer*>(this);
    auto const pose  = state::peek();

    if (pose && state::active()) refreshEngineCamera(level, *pose);
    origin(parameters);
}

// ---------------------------------------------------------------- 安装 / 卸载
struct HookState {
    bool mouse{};
    bool keyDown{};
    bool keyUp{};
    bool setup{};
    bool tickCamera{};
    bool updateViewArea{};
    bool grabMouse{};
    bool releaseMouse{};
};

HookState gHooks;

bool removeAll() {
    if (gHooks.releaseMouse && FreeCameraReleaseMouseHook::unhook()) gHooks.releaseMouse = false;
    if (gHooks.grabMouse && FreeCameraGrabMouseHook::unhook()) gHooks.grabMouse = false;
    if (gHooks.updateViewArea && FreeCameraUpdateViewAreaHook::unhook()) gHooks.updateViewArea = false;
    if (gHooks.tickCamera && FreeCameraTickCameraHook::unhook()) gHooks.tickCamera = false;
    if (gHooks.setup && FreeCameraSetupHook::unhook()) gHooks.setup = false;
    if (gHooks.keyUp && FreeCameraKeyUpHook::unhook()) gHooks.keyUp = false;
    if (gHooks.keyDown && FreeCameraKeyDownHook::unhook()) gHooks.keyDown = false;
    if (gHooks.mouse && FreeCameraMouseHook::unhook()) gHooks.mouse = false;
    return !gHooks.mouse && !gHooks.keyDown && !gHooks.keyUp && !gHooks.setup && !gHooks.tickCamera
        && !gHooks.updateViewArea && !gHooks.grabMouse && !gHooks.releaseMouse;
}

} // namespace

namespace hooks {

void handOverMovementKeys(bool capture) {
    auto const held = gPhysicalKeys.load(std::memory_order_acquire);

    if (capture) {
        // 相机这边：按住的键喂给相机 —— 不用松手重按就能飞
        for (auto const& key : kMovementKeys) {
            if ((held & key.bit) != 0) state::setKey(key.bit, true);
        }
    }
    if (held == 0) return;

    // 游戏那边：接管时把按着的键抾起（实体留在原地），交还时重新按下（不用松手就能继续走）。
    // 这两发都走**引擎自己的那个函数**（所以会被我们自己的钩子看到，由 `gSyntheticKey` 放行），
    // 而不是直接改引擎状态 —— 状态放在哪儿本案无从得知，事件入口却是确定的。
    auto* hid = gHidController.load(std::memory_order_acquire);
    if (!hid) return;

    gSyntheticKey.store(true, std::memory_order_release);
    for (auto const& key : kMovementKeys) {
        if ((held & key.bit) == 0) continue;
        if (capture) {
            hid->$onKeyUp(key.keyCode);
        } else {
            hid->$onKeyDown(key.keyCode, Bedrock::Input::KeyboardEventProcessor::InputOrigin::Unknown);
        }
    }
    gSyntheticKey.store(false, std::memory_order_release);
}

bool install() {
    if (gInstalled.load(std::memory_order_acquire)) return true;

    gFailureLogged.store(false, std::memory_order_release);
    state::reset();
    // 默认当作"游戏抓着鼠标"；真正的状态随后续的 grab / release 钩子自我纠正
    gGameHasMouse.store(true, std::memory_order_release);

    if (!gHooks.mouse) gHooks.mouse = FreeCameraMouseHook::hook() == 0;
    if (!gHooks.keyDown) gHooks.keyDown = FreeCameraKeyDownHook::hook() == 0;
    if (!gHooks.keyUp) gHooks.keyUp = FreeCameraKeyUpHook::hook() == 0;
    if (!gHooks.setup) gHooks.setup = FreeCameraSetupHook::hook() == 0;
    if (!gHooks.tickCamera) gHooks.tickCamera = FreeCameraTickCameraHook::hook() == 0;
    if (!gHooks.updateViewArea) gHooks.updateViewArea = FreeCameraUpdateViewAreaHook::hook() == 0;
    if (!gHooks.grabMouse) gHooks.grabMouse = FreeCameraGrabMouseHook::hook() == 0;
    if (!gHooks.releaseMouse) gHooks.releaseMouse = FreeCameraReleaseMouseHook::hook() == 0;

    bool const installed = gHooks.mouse && gHooks.keyDown && gHooks.keyUp && gHooks.setup && gHooks.tickCamera
                        && gHooks.updateViewArea && gHooks.grabMouse && gHooks.releaseMouse;
    if (!installed) {
        bool const mouse      = gHooks.mouse;
        bool const keyDown    = gHooks.keyDown;
        bool const keyUp      = gHooks.keyUp;
        bool const setup      = gHooks.setup;
        bool const tick       = gHooks.tickCamera;
        bool const viewArea   = gHooks.updateViewArea;
        bool const grab       = gHooks.grabMouse;
        bool const release    = gHooks.releaseMouse;
        bool const rolledBack = removeAll();
        logger().error(
            "Unable to install the free camera hooks (mouse={}, keyDown={}, keyUp={}, setup={}, tick={}, viewArea={}, "
            "grab={}, release={}, rollback={})",
            mouse,
            keyDown,
            keyUp,
            setup,
            tick,
            viewArea,
            grab,
            release,
            rolledBack
        );
        return false;
    }

    gInstalled.store(true, std::memory_order_release);
    logger().debug("Free camera hooks installed");
    return true;
}

bool uninstall() {
    if (!gInstalled.load(std::memory_order_acquire) && !gHooks.mouse && !gHooks.keyDown && !gHooks.keyUp
        && !gHooks.setup && !gHooks.tickCamera && !gHooks.updateViewArea && !gHooks.grabMouse && !gHooks.releaseMouse) {
        return true;
    }

    gInstalled.store(false, std::memory_order_release);
    state::reset();

    bool const removed = removeAll();
    if (!removed) {
        logger().error("Unable to remove the free camera hooks cleanly");
        return false;
    }
    logger().debug("Free camera hooks removed");
    return true;
}

} // namespace hooks

} // namespace mangrove::features::freecamera

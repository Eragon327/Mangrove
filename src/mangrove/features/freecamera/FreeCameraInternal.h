#pragma once

#include "mc/deps/core/math/Vec3.h"

#include <cstdint>
#include <optional>

namespace mangrove::features::freecamera {

/// 渲染层相机姿态。角度制，yaw / pitch 遵循原版约定：
/// yaw 0 = 朝 +Z（南），yaw 增大向左转；pitch 为正表示低头。
struct Pose {
    float x{};
    float y{};
    float z{};
    float yaw{};
    float pitch{};
};

/// 由 yaw / pitch 算出的三个单位轴（roll 恒为 0）。
/// 与 `mce::Camera` 的 `mRight` / `mUp` / `mForward` 同向。
struct Basis {
    Vec3 right{};
    Vec3 up{};
    Vec3 forward{};
};

/// @note 定义在 `FreeCamera.cpp`
[[nodiscard]] Basis basisOf(Pose const& pose);

/// 自由相机的运行时状态。
///
/// 跨线程规则只有一条：**输入侧只写原子，姿态只在渲染线程读写**。
///   - 输入钩子（窗口线程）：`setKey` / `addLook`；
///   - 相机钩子（渲染线程）：`update` / `peek`；
///   - 功能层：`setConfig` / `reset`。
namespace state {

/// 开关与参数。由 `FreeCamera::publishRuntimeConfig()` 在**设置变化时**写入
/// （以前是渲染线程每帧写，现在钩子常驻、旗标只在开关 / 改设置时发 —— 于是
///  `active()` 同时就是"最后一次生效的状态"，`FreeCamera::apply()` 的幂等判据用它）。
struct Config {
    bool  enabled{false};
    float speed{1.0f};
};

void setConfig(Config config);

/// 是否处于启用状态（输入钩子据此决定吞不吞按键）
[[nodiscard]] bool active();

/// 被自由相机吞掉的移动键（位掩码）
enum Key : std::uint32_t {
    Forward = 1u << 0,
    Back    = 1u << 1,
    Left    = 1u << 2,
    Right   = 1u << 3,
    Up      = 1u << 4,
    Down    = 1u << 5,
};

void setKey(std::uint32_t bit, bool down);
void clearKeys();
void addLook(float dx, float dy);

/// 界面独占输入时调用：丢掉按住状态和累积的鼠标位移。
/// 少了这一步，菜单打开期间松开的键会永远停在「按住」，关掉菜单后相机自己飞。
void freezeInput();

/// 每帧一次（渲染线程）。
/// @param frozen 界面正独占输入：清输入、姿态保持不动，菜单背后的画面不跳
/// @param nativeStart 本帧原生相机姿态（引擎**此刻正在渲染**的位置与朝向），
///                    用作启用后第一帧的起点 —— 取它才能做到开启瞬间零跳变
/// @return 需要覆写到相机上的姿态；未启用或拿不到起点时返回 `std::nullopt`
[[nodiscard]] std::optional<Pose> update(bool frozen, std::optional<Pose> const& nativeStart);

/// 只读当前姿态，不推进。相机钩子用它判断「是否已经拿到起点」。
[[nodiscard]] std::optional<Pose> peek();

/// 关闭：丢弃姿态。相机位置刻意不持久化，下次启用重新从玩家眼睛起步。
void reset();

} // namespace state

/// 钩子的安装 / 卸载（输入 + 相机）。
/// 任何一步失败都整体回滚，不会留下「装了一半」的状态。
namespace hooks {

/// 把"现在按着的移动键"在游戏与自由相机之间交接。
///
/// 按着 W 再按 F 时，两件事必须同时做对：游戏那边把按着的键抾起（否则实体一直往前走，
/// 相机却留在原地），相机这边把按着的键当成自己的（否则要松手重按才能飞）。
/// @param capture true = 自由相机接管；false = 交还给游戏
void handOverMovementKeys(bool capture);

[[nodiscard]] bool install();
[[nodiscard]] bool uninstall();

} // namespace hooks

} // namespace mangrove::features::freecamera

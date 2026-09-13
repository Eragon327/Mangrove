#pragma once

#include "mangrove/input/Keys.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace mangrove::input {

/// 组合键里的键一律用 Windows 虚拟键码（VK_*）表示，合法范围 0..255
inline constexpr int kMaxVirtualKey = 256;

// ---------------------------------------------------------------------------
// 鼠标按键
// ---------------------------------------------------------------------------
//
// 鼠标键就编码在同一个 VK 空间里（Windows 自己也是这么定义的：VK_LBUTTON..VK_XBUTTON2），
// 所以鼠标和键盘可以混在同一个组合里，例如「X + 右键」。
// 组合键的判定逻辑因此不需要任何鼠标相关的分支 —— 鼠标键就是「另一个键」。
//
// 这里不用 WinUser.h 的宏，免得 input/KeyBind.h 被拖进 <Windows.h>。

inline constexpr int kMouseLeft   = 0x01; ///< VK_LBUTTON
inline constexpr int kMouseRight  = 0x02; ///< VK_RBUTTON
inline constexpr int kMouseMiddle = 0x04; ///< VK_MBUTTON
inline constexpr int kMouseX1     = 0x05; ///< VK_XBUTTON1（侧键 1）
inline constexpr int kMouseX2     = 0x06; ///< VK_XBUTTON2（侧键 2）

/// 是否鼠标按键
[[nodiscard]] bool isMouseButton(int virtualKey);

/// 把 Bedrock 的鼠标动作编号（`MouseAction::Action*`）翻成键码。
/// 移动 / 滚轮这类不参与绑定的动作返回 0。
[[nodiscard]] int mouseActionToKey(int action);

/// 修饰键（Shift / Ctrl / Alt / Win）。组合键里它只作为前置条件，不作为触发键。
[[nodiscard]] bool isModifierKey(int virtualKey);

/// 键码是否可用
[[nodiscard]] bool isValidKey(int virtualKey);

/// 组合的触发键 = 最后一个非修饰键；全是修饰键时取最后一个
[[nodiscard]] int triggerKey(std::vector<int> const& keys);

/// 按键的显示名，例如 "X"、"Ctrl"、"F1"、"鼠标右键"。
/// 键盘走系统 `GetKeyNameText`（跟着 Windows 的显示语言走，不用维护翻译表）；
/// 鼠标键 Windows 没有名字，走 `mangrove.key.*` 的多语言表。
[[nodiscard]] std::string keyName(int virtualKey);

/// 一次按键组合，按「按下顺序」保存。
///
/// 判定语义：触发键按下的瞬间，组合里所有键都处于按住状态。
/// @note 额外要求「按住键数 == 组合长度」看似更严谨，但会让玩家按住 W 走路时按不出热键，
///       所以冲突消解放在 KeyManager 里做（只触发键数最多的那条绑定）。
class KeyBind {
public:
    KeyBind() = default;

    /// 校验并构造；键码非法或列表为空时返回 `std::nullopt`
    [[nodiscard]] static std::optional<KeyBind> make(std::vector<int> keys);

    /// 单键的便捷构造
    [[nodiscard]] static std::optional<KeyBind> single(int virtualKey) { return make({virtualKey}); }

    [[nodiscard]] std::vector<int> const& keys() const { return mKeys; }
    [[nodiscard]] int                     trigger() const { return mTrigger; }
    [[nodiscard]] size_t                  size() const { return mKeys.size(); }
    [[nodiscard]] bool                    empty() const { return mKeys.empty(); }

    /// 组合内所有键都按住
    [[nodiscard]] bool allHeld(std::array<bool, kMaxVirtualKey> const& held) const;

    /// 键码序列是否完全相同（用于判断有没有改回默认键）
    [[nodiscard]] bool equals(KeyBind const& other) const { return mKeys == other.mKeys; }

    /// 展示文本，例如 "X + C"；空组合显示为 "-"
    [[nodiscard]] std::string display() const;

private:
    explicit KeyBind(std::vector<int> keys);

    std::vector<int> mKeys;
    int              mTrigger{};
};

} // namespace mangrove::input

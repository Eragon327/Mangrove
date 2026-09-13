#include "mangrove/ui/Widgets.h"

#include "mangrove/core/Feature.h"
#include "mangrove/input/KeyManager.h"
#include "mangrove/ui/Overlay.h"
#include "mangrove/ui/Theme.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mangrove::ui::widgets {
namespace {

using ll::i18n_literals::operator""_tr;

/// 控件最多占行宽的这个比例，剩下的留给标签
constexpr float kControlRowRatio = 0.5f;
/// 控件自身的宽度上限（逻辑单位，会乘界面缩放）：行再宽也不把它拉长。
/// 滑条就在这个宽度里被两个小按钮再挤一下 —— **嫌滑条短、拖动太灵敏就调这两个数**。
constexpr float kMaxControlWidth = 320.0f;

float gRowLeft{};
float gRowRight{};

/// 每个数值控件的临时编辑方式。
///
/// 这是**视图状态**，不是设置 —— 所以既不进模型也不进配置，
/// 菜单关掉就清空（见 `resetTransientState`），下次打开又是默认的滑条。
struct FieldState {
    bool textMode{};     ///< true = 显示输入框，false = 显示滑条
    bool focusPending{}; ///< 刚从滑条切过来时，下一帧自动聚焦输入框
};
std::unordered_map<std::string, FieldState> gFieldStates;

/// 数值的显示格式：按步长决定小数位，免得 0.3 被显示成 0
[[nodiscard]] char const* valueFormat(double step) {
    if (step >= 1.0) return "%.0f";
    if (step >= 0.1) return "%.1f";
    return "%.2f";
}

/// 控件的宽度：行宽的一个比例，且有上限。两者都是**逻辑单位**，上限要乘界面缩放 ——
/// 否则 4K 全屏下控件不会跟着字体变大，滑条会显得又短又灵敏。
[[nodiscard]] float controlWidth() {
    float const maxWidth = kMaxControlWidth * theme::scale();
    return std::min(maxWidth, (gRowRight - gRowLeft) * kControlRowRatio);
}

/// 画左对齐的标签，并让光标停在标签后面
void labelRow(std::string const& label) {
    beginRow();
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine();
}

/// 把光标准备到右对齐控件的起始位置
void alignControlRight(float width) { ImGui::SetCursorPosX(rowRight() - width); }

/// 当前键位。空组合 = 这个槽没绑键（`display()` 会显示成 "-"）。
[[nodiscard]] input::KeyBind currentKeys(std::string_view bindingName) {
    if (bindingName == Overlay::kMenuToggleBinding) return Overlay::getInstance().menuToggleKeys();
    if (auto keys = input::KeyManager::getInstance().keys(bindingName)) return *keys;
    return {};
}

/// 这组键和**别的绑定**撞到什么程度。
///
/// 两种坏事，用颜色分：
///   - **完全重复**（`Exact`，红）—— 键数相同时 `handlePress` 会把两条**都触发**；
///   - **前缀重叠**（`Overlap`，黄）—— 比如 `X` 与 `X + C`：短的会在长的还没成形时就先响，
///     于是想用长的那条，必定先触发一次短的。反过来 `C` 与 `X + C` **不算**
///     （触发键都是 `C`，键数最多者胜，长的直接把它筛掉）。
/// @note 别指望 ImGui 的 ID 冲突来当检测 —— 那只是控件没有唯一 ID，和绑定内容无关。
[[nodiscard]] input::ConflictSeverity keyConflict(std::string_view self, input::KeyBind const& keys) {
    if (keys.empty()) return input::ConflictSeverity::None;

    auto severity = input::KeyManager::getInstance().conflictOf(self, keys);
    // 菜单开关不走 `KeyManager`（它在窗口过程层判定），单独比一次，取更严重的那个
    if (self != Overlay::kMenuToggleBinding) {
        severity = std::max(severity, input::classifyConflict(keys, Overlay::getInstance().menuToggleKeys()));
    }
    return severity;
}

/// 撞键时围在键位按钮外的那圈边框。用纯色 —— 主题里没有语义化的「警告色」，就近定义。
/// 厚度另外算（`theme::scale()`），不然 4K 下这 1px 细得看不见。
constexpr ImVec4 kDuplicateBorder{1.0f, 0.0f, 0.0f, 1.0f}; // 纯红：完全重复
constexpr ImVec4 kOverlapBorder{1.0f, 1.0f, 0.0f, 1.0f};   // 纯黄：包含关系

/// 捕获过程中已按下的组合键（可能还没松手）
[[nodiscard]] std::string formatCombo(std::vector<int> const& keys) {
    if (keys.empty()) return "-";

    std::string result;
    for (size_t index = 0; index < keys.size(); ++index) {
        if (index != 0) result += " + ";
        result += input::keyName(keys[index]);
    }
    return result;
}

/// 让改键结果生效。菜单开关归 Overlay，其余归 KeyManager（它会一并落盘）。
void applyRebind(std::string const& target, input::KeyBind const& keys) {
    if (target == Overlay::kMenuToggleBinding) {
        Overlay::getInstance().setMenuToggleKeys(keys);
    } else {
        input::KeyManager::getInstance().rebind(target, keys);
    }
}

} // namespace

void beginRow() {
    // 让标签和右侧控件的排版基线对齐，不影响 X
    ImGui::AlignTextToFramePadding();

    gRowLeft  = ImGui::GetCursorPosX();
    gRowRight = gRowLeft + ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(gRowLeft);
}

float rowRight() { return gRowRight; }

void resetTransientState() { gFieldStates.clear(); }

void settingRow(core::Feature& feature, core::Setting& setting) {
    auto const label = feature.label(setting.id());
    // 控件 ID 用「功能.设置项」，避免不同功能里同名的设置互相串 ID
    auto const key      = feature.id() + "." + setting.id();
    auto const widgetId = "##" + key;

    labelRow(label);

    bool changed = false;
    switch (setting.kind()) {
    case core::Setting::Kind::Toggle: {
        bool value = setting.enabled();
        // 复选框是正方形，边长就是帧高
        alignControlRight(ImGui::GetFrameHeight());
        changed = ImGui::Checkbox(widgetId.c_str(), &value);
        if (changed) setting.setValue(value ? 1.0 : 0.0);
        break;
    }
    case core::Setting::Kind::Number:
    case core::Setting::Kind::NumberInput: {
        // 滑条和输入框是同一个控件的两种编辑方式。右边的小按钮：能画滑条时是
        // 「换编辑方式」，任何时候都有「回默认值」。
        //
        // `NumberInput` 一开始就只要输入框；`Number` 是「要滑条」，但区间有一头是 `±inf`
        // 时滑条画不出来，兜底也退回输入框。
        bool const  sliderCapable = setting.kind() == core::Setting::Kind::Number && setting.hasFiniteRange();
        auto&       state         = gFieldStates[key];
        auto const& style         = ImGui::GetStyle();

        auto const totalWidth  = controlWidth();
        auto const buttonText  = "mangrove.widget.numberInput"_tr();
        auto const resetText   = "mangrove.widget.reset"_tr();
        auto const buttonWidth = ImGui::CalcTextSize(buttonText.c_str()).x + style.FramePadding.x * 2.0f;
        auto const resetWidth  = ImGui::CalcTextSize(resetText.c_str()).x + style.FramePadding.x * 2.0f;
        // 纯输入框没有切换按钮，省下的宽度归输入框 —— 不然控件右边会空一截
        auto const toggleWidth = sliderCapable ? buttonWidth + style.ItemInnerSpacing.x : 0.0f;
        auto const fieldWidth  = std::max(40.0f, totalWidth - toggleWidth - resetWidth - style.ItemInnerSpacing.x);

        alignControlRight(totalWidth);
        ImGui::SetNextItemWidth(fieldWidth);

        double       value    = setting.value();
        double const minValue = setting.min();
        double const maxValue = setting.max();

        // 无界时无视上次的编辑方式，直接按输入框画
        bool const textMode = !sliderCapable || state.textMode;

        if (textMode) {
            if (state.focusPending) {
                // 切过来的这一帧直接聚焦，玩家点完就能打字
                ImGui::SetKeyboardFocusHere();
                state.focusPending = false;
            }
            // CharsDecimal 只收数字字符；AutoSelectAll 聚焦即全选，直接覆盖原值
            changed = ImGui::InputDouble(
                widgetId.c_str(),
                &value,
                0.0,
                0.0,
                valueFormat(setting.step()),
                ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll
            );
        } else {
            changed = ImGui::SliderScalar(
                widgetId.c_str(),
                ImGuiDataType_Double,
                &value,
                &minValue,
                &maxValue,
                valueFormat(setting.step())
            );
        }
        // 拖动 / 输入过程中就写回，功能能立刻看到新值；写库由 SettingsStore 当场完成
        if (changed) setting.setValue(value);

        // 只有两端都有限的区间才给「换成滑条」这个按钮
        if (sliderCapable) {
            ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);

            // 输入态把按钮点亮，这样「现在是哪种编辑方式」一眼能看出来。
            //
            // @note 颜色先算好，push / pop **无条件成对**、紧贴着按钮写。
            //       不能写成「if (textMode) push ... if (textMode) pop」——
            //       按钮一点下去 textMode 就翻转了，两边条件不一致，push/pop 必然配平失败，
            //       ImGui 会报错并在窗口上画一圈红框。
            auto const&  colors = style.Colors;
            bool const   lit    = state.textMode;
            ImVec4 const button = lit ? colors[ImGuiCol_SliderGrab] : colors[ImGuiCol_Button];
            ImVec4 const hover  = lit ? colors[ImGuiCol_SliderGrabActive] : colors[ImGuiCol_ButtonHovered];
            ImVec4 const active = lit ? colors[ImGuiCol_SliderGrabActive] : colors[ImGuiCol_ButtonActive];

            ImGui::PushStyleColor(ImGuiCol_Button, button);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
            bool const toggled = ImGui::Button((buttonText + "##" + key).c_str());
            ImGui::PopStyleColor(3);

            if (toggled) {
                state.textMode     = !state.textMode;
                state.focusPending = state.textMode;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "mangrove.widget.numberInputHint"_tr().c_str());
        }

        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);

        // 回默认值。已经等于默认值时置灰，但**照样画出来**：
        // 不画的话这一行里滑条的宽度会在「可重置 / 不可重置」之间跳动。
        //
        // @note 和上面那个按钮同理，`BeginDisabled` / `EndDisabled` 必须无条件成对。
        bool const canReset = !setting.isDefault();

        if (!canReset) ImGui::BeginDisabled();
        bool const reset = ImGui::Button((resetText + "##reset." + key).c_str());
        if (!canReset) ImGui::EndDisabled();

        if (reset) {
            setting.reset();
            changed = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", "mangrove.widget.resetHint"_tr().c_str());
        }
        break;
    }
    }

    if (changed) core::FeatureManager::notifyChanged(feature, setting);
}

void keyRow(std::string_view bindingName, std::string const& label, input::KeyBind const& defaultKeys) {
    auto&      overlay   = Overlay::getInstance();
    auto const target    = std::string{bindingName};
    bool const capturing = overlay.isCapturing(target);
    auto const current   = currentKeys(target);

    auto const text = capturing && overlay.capturePreview().empty()
                        ? "mangrove.menu.capturing"_tr()
                        : (capturing ? formatCombo(overlay.capturePreview()) : current.display());

    labelRow(label);

    auto const& style      = ImGui::GetStyle();
    auto const  resetText  = "mangrove.widget.reset"_tr();
    auto const  resetWidth = ImGui::CalcTextSize(resetText.c_str()).x + style.FramePadding.x * 2.0f;
    auto const  keyWidth   = ImGui::CalcTextSize(text.c_str()).x + style.FramePadding.x * 2.0f;
    // 捕获中不画重置：那时布局越安静越好（预览文本还在变宽）
    auto const totalWidth = capturing ? keyWidth : keyWidth + style.ItemInnerSpacing.x + resetWidth;

    // 控件 ID 必须**每行唯一**，不能拿显示文本当 label：两行都绑 F（或都没绑，都显示 "-"）
    // 就会撞成同一个 ID（`##` 只是把后半段藏起来，整串照样被哈希），ImGui 报
    // "visible items with conflicting ID" 而且按钮会互相抢状态。
    auto const keyId = text + "##" + target;

    alignControlRight(totalWidth);

    if (capturing) {
        ImGui::BeginDisabled();
        ImGui::Button(keyId.c_str());
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", "mangrove.menu.unbindHint"_tr().c_str());
        }
        return;
    }

    auto const severity   = keyConflict(target, current);
    bool const conflicted = severity != input::ConflictSeverity::None;

    // 撞键就围一圈边框（红 = 完全重复，黄 = 包含关系）。样式栈照旧**无条件成对**：
    // `severity` 在本帧内不会再变（它只看绑定内容，和 hover / 点击无关），所以两边条件一致。
    if (conflicted) {
        ImGui::PushStyleColor(
            ImGuiCol_Border,
            severity == input::ConflictSeverity::Exact ? kDuplicateBorder : kOverlapBorder
        );
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, theme::scale()));
    }
    bool const clicked = ImGui::Button(keyId.c_str());
    if (conflicted) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    if (clicked) {
        overlay.beginCapture(target, [](std::string const& name, std::optional<input::KeyBind> result) {
            // 空的 `optional` = 捕获被取消（菜单关了），不动绑定；
            // 有值但为空 = 玩家按了 Esc，**解绑**
            if (!result) return;
            applyRebind(name, *result);
            Overlay::getInstance().notify(
                result->empty() ? "mangrove.menu.unbound"_tr() : "mangrove.menu.rebound"_tr()
            );
        });
    }
    if (ImGui::IsItemHovered()) {
        // 严重程度不同、提示也不同（`_tr` 只吃字面量，所以老老实实分三支写）
        if (severity == input::ConflictSeverity::Exact) {
            ImGui::SetTooltip("%s", "mangrove.menu.keyConflict"_tr().c_str());
        } else if (severity == input::ConflictSeverity::Overlap) {
            ImGui::SetTooltip("%s", "mangrove.menu.keyOverlap"_tr().c_str());
        } else {
            ImGui::SetTooltip("%s", "mangrove.menu.rebindHint"_tr().c_str());
        }
    }

    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);

    // 回默认键位。已经等于默认时置灰，但**照样画出来** —— 否则行内宽度会在
    // 「可重置 / 不可重置」之间跳（和数值控件那一行同理）。
    bool const canReset = !current.equals(defaultKeys);

    if (!canReset) ImGui::BeginDisabled();
    bool const reset = ImGui::Button((resetText + "##reset." + target).c_str());
    if (!canReset) ImGui::EndDisabled();

    if (reset) {
        applyRebind(target, defaultKeys);
        Overlay::getInstance().notify(defaultKeys.empty() ? "mangrove.menu.unbound"_tr() : "mangrove.menu.reset"_tr());
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", "mangrove.widget.resetHint"_tr().c_str());
    }
}

} // namespace mangrove::ui::widgets

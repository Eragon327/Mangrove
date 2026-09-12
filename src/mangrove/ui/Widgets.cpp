#include "mangrove/ui/Widgets.h"

#include "mangrove/core/Feature.h"
#include "mangrove/input/KeyManager.h"
#include "mangrove/ui/Overlay.h"

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
constexpr float kControlRowRatio = 0.45f;
/// 控件自身的宽度上限（行再宽也不把它拉长）
constexpr float kMaxControlWidth = 260.0f;

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

[[nodiscard]] float controlWidth() { return std::min(kMaxControlWidth, (gRowRight - gRowLeft) * kControlRowRatio); }

/// 画左对齐的标签，并让光标停在标签后面
void labelRow(std::string const& label) {
    beginRow();
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine();
}

/// 把光标准备到右对齐控件的起始位置
void alignControlRight(float width) { ImGui::SetCursorPosX(rowRight() - width); }

/// 当前键位的显示文本
[[nodiscard]] std::string currentKeysDisplay(std::string_view bindingName) {
    if (bindingName == Overlay::kMenuToggleBinding) return Overlay::getInstance().menuToggleKeys().display();
    if (auto keys = input::KeyManager::getInstance().keys(bindingName)) return keys->display();
    return "-";
}

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
    case core::Setting::Kind::Number: {
        // 滑条和输入框是同一个控件的两种编辑方式：右边一个小按钮来回切。
        auto&       state       = gFieldStates[key];
        auto const& style       = ImGui::GetStyle();
        auto const  totalWidth  = controlWidth();
        auto const  buttonText  = "mangrove.widget.numberInput"_tr();
        auto const  buttonWidth = ImGui::CalcTextSize(buttonText.c_str()).x + style.FramePadding.x * 2.0f;
        auto const  fieldWidth  = std::max(40.0f, totalWidth - buttonWidth - style.ItemInnerSpacing.x);

        alignControlRight(totalWidth);
        ImGui::SetNextItemWidth(fieldWidth);

        double       value    = setting.value();
        double const minValue = setting.min();
        double const maxValue = setting.max();

        if (state.textMode) {
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
        // 拖动 / 输入过程中就写回，功能能立刻看到新值；落盘由 Config 决定时机
        if (changed) setting.setValue(value);

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
        break;
    }
    }

    if (changed) core::FeatureManager::notifyChanged(feature, setting);
}

void keyRow(std::string_view bindingName, std::string const& label) {
    auto&      overlay   = Overlay::getInstance();
    auto const target    = std::string{bindingName};
    bool const capturing = overlay.isCapturing(target);

    auto const text = capturing && overlay.capturePreview().empty()
                        ? "mangrove.menu.capturing"_tr()
                        : (capturing ? formatCombo(overlay.capturePreview()) : currentKeysDisplay(target));

    labelRow(label);

    auto const width = ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    alignControlRight(width);

    if (capturing) {
        ImGui::BeginDisabled();
        ImGui::Button(text.c_str());
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", "mangrove.menu.cancelHint"_tr().c_str());
        }
        return;
    }

    if (ImGui::Button(text.c_str())) {
        overlay.beginCapture(target, [](std::string const& name, std::optional<input::KeyBind> result) {
            if (!result) return;
            applyRebind(name, *result);
            Overlay::getInstance().notify("mangrove.menu.rebound"_tr());
        });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "mangrove.menu.rebindHint"_tr().c_str());
}

} // namespace mangrove::ui::widgets

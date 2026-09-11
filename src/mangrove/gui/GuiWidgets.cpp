#include "mangrove/gui/GuiWidgets.h"

#include "mangrove/gui/GuiOverlay.h"
#include "mangrove/input/KeyInputManager.h"



#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <Windows.h>

#include <string>
#include <vector>

namespace mangrove::gui {
namespace {

using ll::i18n_literals::operator""_tr;

/// 虚拟键码 -> 系统本地化的按键名（"X"、"Ctrl"、"左 Shift" …）
std::string virtualKeyName(int vk) {
    UINT const scanCode = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);

    LONG keyData = static_cast<LONG>(scanCode) << 16;
    switch (vk) {
    // 这些键在扫描码里带"扩展"标记，不加的话 GetKeyNameText 会取到错误的名字
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_DIVIDE:
    case VK_RCONTROL:
    case VK_RMENU:
        keyData |= 1 << 24;
        break;
    default:
        break;
    }

    wchar_t name[64]{};
    if (GetKeyNameTextW(keyData, name, 64) <= 0) return "?";

    int const bytes = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return "?";

    std::string result(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name, -1, result.data(), bytes, nullptr, nullptr);
    return result;
}

/// 组合键 -> 展示文本，例如 "X + C"
std::string formatKeyCombo(std::vector<int> const& keys) {
    std::string result;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i != 0) result += " + ";
        result += virtualKeyName(keys[i]);
    }
    return result.empty() ? "-" : result;
}

/// 某个绑定当前的组合键。菜单开关热键不在 KeyInputManager 里，单独取。
std::vector<int> currentKeys(std::string_view bindingName) {
    if (bindingName == GuiOverlay::kToggleBinding) return GuiOverlay::getInstance().getToggleKeys();
    if (auto keys = input::KeyInputManager::getInstance().getKeys(bindingName)) return *keys;
    return {};
}

/// 右侧控件宽度：别把窗口撑坏，也别太窄看不清
float widgetWidth() { return ImGui::GetWindowWidth() * 0.3f; }

/// 左标签 + 右控件的公共排版
void beginLabelledRow(std::string const& label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine();
}

} // namespace

void addKeyBinder(std::string_view bindingName, std::string const& label) {
    auto& overlay = GuiOverlay::getInstance();
    auto  name    = std::string{bindingName};

    beginLabelledRow(label);

    if (overlay.isRebinding(bindingName)) {
        // 实时预览：按下什么就显示什么（按下 F 还没松就先显示 F）
        auto const preview = overlay.getRebindPreview();
        auto const text    = preview.empty() ? "mangrove.gui.capturing"_tr() : formatKeyCombo(preview);

        auto const widget = text + "##" + name;
        ImGui::BeginDisabled();
        ImGui::Button(widget.c_str());
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled("%s", "mangrove.gui.cancelHint"_tr().c_str());
        return;
    }

    auto const keysText = formatKeyCombo(currentKeys(bindingName));
    if (ImGui::Button((keysText + "##" + name).c_str())) overlay.beginRebind(name);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "mangrove.gui.rebindHint"_tr().c_str());
}

bool addToggle(std::string const& label, bool& value) {
    beginLabelledRow(label);
    return ImGui::Checkbox(("##" + label).c_str(), &value);
}

bool addSlider(std::string const& label, float& value, float min, float max, char const* format) {
    beginLabelledRow(label);
    ImGui::SetNextItemWidth(widgetWidth());
    return ImGui::SliderFloat(("##" + label).c_str(), &value, min, max, format);
}

void addText(std::string const& text) { ImGui::TextUnformatted(text.c_str()); }

void addHint(std::string const& text) { ImGui::TextDisabled("%s", text.c_str()); }

void addSectionHeader(std::string const& text) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted(text.c_str());
    ImGui::Spacing();
}

} // namespace mangrove::gui

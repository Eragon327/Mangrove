#include "mangrove/gui/GuiWidgets.h"

#include "mangrove/gui/GuiOverlay.h"
#include "mangrove/input/KeyInputManager.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <Windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace mangrove::gui {
namespace {

using ll::i18n_literals::operator""_tr;

/// 滑条最多占行宽的这个比例，剩下的留给标签
constexpr float kSliderRowRatio = 0.45f;
/// 滑条自身的宽度上限（行再宽也不把它拉长）
constexpr float kMaxSliderWidth = 260.0f;

MenuPage gActivePage = MenuPage::Toggles;

/// 当前行的左右边界，由 beginRow() 记录
float gRowLeft{};
float gRowRight{};

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

/// 行首：记下这一行的边界，再画左侧标签（左对齐）
void labelRow(std::string const& label) {
    beginRow();
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine();
}

} // namespace

void setActivePage(MenuPage page) { gActivePage = page; }

MenuPage activePage() { return gActivePage; }

bool isActivePage(MenuPage page) { return gActivePage == page; }

void beginRow() {
    // 对齐到帧高的排版基线不影响 X，可以放在取值之前
    ImGui::AlignTextToFramePadding();

    gRowLeft  = ImGui::GetCursorPosX();
    gRowRight = gRowLeft + ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(gRowLeft);
}

float rowWidth() { return std::max(1.0f, gRowRight - gRowLeft); }

float rowLeft() { return gRowLeft; }

float rowRight() { return gRowRight; }

void addKeyBinder(std::string_view bindingName, std::string const& label) {
    if (!isActivePage(MenuPage::Keys)) return;

    auto&      overlay   = GuiOverlay::getInstance();
    auto       name      = std::string{bindingName};
    bool const capturing = overlay.isRebinding(bindingName);

    auto const text = capturing ? (overlay.getRebindPreview().empty() ? "mangrove.gui.capturing"_tr()
                                                                      : formatKeyCombo(overlay.getRebindPreview()))
                                : formatKeyCombo(currentKeys(bindingName));

    labelRow(label);

    // 右对齐：按钮右边缘贴着行的右边界（宽度随文字变，但右边缘不动）
    auto const width = ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(rowRight() - width);

    auto const widget = text + "##" + name;
    if (capturing) {
        ImGui::BeginDisabled();
        ImGui::Button(widget.c_str());
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "mangrove.gui.cancelHint"_tr().c_str());
        return;
    }

    if (ImGui::Button(widget.c_str())) overlay.beginRebind(name);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "mangrove.gui.rebindHint"_tr().c_str());
}

bool addToggle(std::string const& label, bool& value) {
    if (!isActivePage(MenuPage::Toggles)) return false;

    labelRow(label);

    // 勾选框是正方形，边长就是帧高
    ImGui::SetCursorPosX(rowRight() - ImGui::GetFrameHeight());
    return ImGui::Checkbox(("##" + label).c_str(), &value);
}

bool addSlider(std::string const& label, float& value, float min, float max, char const* format) {
    if (!isActivePage(MenuPage::Sliders)) return false;
    labelRow(label);

    auto const width = std::min(kMaxSliderWidth, rowWidth() * kSliderRowRatio);
    ImGui::SetCursorPosX(rowRight() - width);
    ImGui::SetNextItemWidth(width);
    return ImGui::SliderFloat(("##" + label).c_str(), &value, min, max, format);
}

void addText(std::string const& text) {
    beginRow();
    ImGui::TextUnformatted(text.c_str());
}

void addHint(std::string const& text) {
    beginRow();
    ImGui::TextDisabled("%s", text.c_str());
}

void addSectionHeader(std::string const& text) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    beginRow();
    ImGui::TextUnformatted(text.c_str());
    ImGui::Spacing();
}

} // namespace mangrove::gui

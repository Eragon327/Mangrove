#include "mangrove/input/KeyBind.h"

#include "ll/api/i18n/I18n.h"

#include "mc/deps/input/MouseAction.h"

#include <Windows.h>

#include <algorithm>

namespace mangrove::input {

bool isModifierKey(int virtualKey) {
    switch (virtualKey) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

bool isValidKey(int virtualKey) { return virtualKey > 0 && virtualKey < kMaxVirtualKey; }

int triggerKey(std::vector<int> const& keys) {
    for (auto iterator = keys.rbegin(); iterator != keys.rend(); ++iterator) {
        if (!isModifierKey(*iterator)) return *iterator;
    }
    return keys.empty() ? 0 : keys.back();
}

bool isMouseButton(int virtualKey) {
    switch (virtualKey) {
    case kMouseLeft:
    case kMouseRight:
    case kMouseMiddle:
    case kMouseX1:
    case kMouseX2:
        return true;
    default:
        return false;
    }
}

int mouseActionToKey(int action) {
    switch (action) {
    case MouseAction::ActionLeft:
        return kMouseLeft;
    case MouseAction::ActionRight:
        return kMouseRight;
    case MouseAction::ActionMiddle:
        return kMouseMiddle;
    case MouseAction::ActionX1:
        return kMouseX1;
    case MouseAction::ActionX2:
        return kMouseX2;
    default:
        // ActionMove / ActionWheel / ActionMoveRelative：不参与绑定
        return 0;
    }
}

std::string keyName(int virtualKey) {
    if (!isValidKey(virtualKey)) return "?";

    if (isMouseButton(virtualKey)) {
        // Windows 没给鼠标键起名字，`GetKeyNameText` 也问不出来，所以走多语言表
        char const* field = nullptr;
        switch (virtualKey) {
        case kMouseLeft:
            field = "mangrove.key.mouseLeft";
            break;
        case kMouseRight:
            field = "mangrove.key.mouseRight";
            break;
        case kMouseMiddle:
            field = "mangrove.key.mouseMiddle";
            break;
        case kMouseX1:
            field = "mangrove.key.mouseX1";
            break;
        default:
            field = "mangrove.key.mouseX2";
            break;
        }
        auto const value = ll::i18n::getInstance().get(field, {});
        // 查不到时 `get()` 会把 key 原样返回
        if (!value.empty() && value != field) return std::string{value};
        return "Mouse";
    }

    LONG keyData = static_cast<LONG>(MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC)) << 16;
    // 这些键的扫描码带「扩展」标记，不补上会取到错误的名字
    switch (virtualKey) {
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

KeyBind::KeyBind(std::vector<int> keys) : mKeys(std::move(keys)), mTrigger(triggerKey(mKeys)) {}

std::optional<KeyBind> KeyBind::make(std::vector<int> keys) {
    if (keys.empty()) return std::nullopt;
    if (!std::ranges::all_of(keys, [](int key) { return isValidKey(key); })) return std::nullopt;
    return KeyBind{std::move(keys)};
}

bool KeyBind::allHeld(std::array<bool, kMaxVirtualKey> const& held) const {
    return std::ranges::all_of(mKeys, [&held](int key) { return held[static_cast<size_t>(key)]; });
}

std::string KeyBind::display() const {
    if (mKeys.empty()) return "-";

    std::string result;
    for (size_t index = 0; index < mKeys.size(); ++index) {
        if (index != 0) result += " + ";
        result += keyName(mKeys[index]);
    }
    return result;
}

} // namespace mangrove::input

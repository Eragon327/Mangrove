#include "mangrove/ui/Overlay.h"

#include "mangrove/Mangrove.h"
#include "mangrove/core/Config.h"
#include "mangrove/input/KeyManager.h"
#include "mangrove/ui/InputGuard.h"
#include "mangrove/ui/Menu.h"
#include "mangrove/ui/Theme.h"

#include <Windows.h>

// 部分 SDK 版本的 d3d12.h 会和 d3d11on12.h 重复定义这几个结构体，先改名绕开
#define D3D12_FEATURE_DATA_D3D12_OPTIONS D3D12_FEATURE_DATA_D3D12_OPTIONS_LEGACY
#define D3D12_FEATURE_DATA_ARCHITECTURE  D3D12_FEATURE_DATA_ARCHITECTURE_LEGACY
#define D3D12_RAYTRACING_GEOMETRY_DESC   D3D12_RAYTRACING_GEOMETRY_DESC_LEGACY
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#undef D3D12_FEATURE_DATA_D3D12_OPTIONS
#undef D3D12_FEATURE_DATA_ARCHITECTURE
#undef D3D12_RAYTRACING_GEOMETRY_DESC

#include <MinHook.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// 新版 ImGui 的头文件里不再声明这个函数，需要自己引入
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace mangrove::ui {
namespace {

// ---------------------------------------------------------------------------
// DXGI / D3D 入口点
// ---------------------------------------------------------------------------

using PresentFn       = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn      = HRESULT(__stdcall*)(IDXGISwapChain1*, UINT, UINT, DXGI_PRESENT_PARAMETERS const*);
using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn =
    HRESULT(__stdcall*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, UINT const*, IUnknown* const*);
using ExecuteCommandListsFn = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// 虚表下标是稳定的 ABI 位置，用一次性临时对象取地址即可
constexpr size_t kPresentVtableIndex             = 8;
constexpr size_t kResizeBuffersVtableIndex       = 13;
constexpr size_t kPresent1VtableIndex            = 22;
constexpr size_t kResizeBuffers1VtableIndex      = 39;
constexpr size_t kExecuteCommandListsVtableIndex = 10;

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

/// 菜单开关热键的默认值：按住 X 再按 C（与 input::KeyBind 的组合语义一致）
constexpr std::array<int, 2> kDefaultMenuToggleKeys{VK_X, VK_C};

/// 提示的存活时长
constexpr ULONGLONG kToastLifetimeMs = 2200;

/// 「把光标还给游戏」的窗口消息（Present 跑在渲染线程，不能直接操作光标）
constexpr UINT kMsgRestoreGameMouse = WM_APP + 0x101;

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

PresentFn             gOriginalPresent{};
Present1Fn            gOriginalPresent1{};
ResizeBuffersFn       gOriginalResizeBuffers{};
ResizeBuffers1Fn      gOriginalResizeBuffers1{};
ExecuteCommandListsFn gOriginalExecuteCommandLists{};

void* gPresentTarget{};
void* gPresent1Target{};
void* gResizeBuffersTarget{};
void* gResizeBuffers1Target{};
void* gExecuteCommandListsTarget{};

ID3D11Device*        gDevice{};
ID3D11DeviceContext* gDeviceContext{};
ID3D11On12Device*    gDevice11On12{};
ID3D12CommandQueue*  gGameQueue{};

HWND    gWindow{};
WNDPROC gOriginalWndProc{};

std::atomic_bool gInstalled{false};
std::atomic_bool gShuttingDown{false};
std::atomic_bool gRendering{false};
std::atomic_bool gImGuiReady{false};
std::atomic_bool gVisible{false};
bool             gGraphicsReady{};
bool             gVisibleLastFrame{};
std::mutex       gResourceMutex;

/// 我们自己吞掉的按下键（含鼠标键），需要连带吞掉对应的抬起
std::array<bool, input::kMaxVirtualKey> gHeldKeys{};
std::array<bool, input::kMaxVirtualKey> gConsumedKeyReleases{};
/// 游戏已经收到的按键 / 鼠标按下状态，用于菜单打开时补发抬起
std::array<bool, input::kMaxVirtualKey> gGameKeysDown{};
std::array<bool, 5>                     gGameMouseButtonsDown{};
bool                                    gConsumeEscapeRelease{};

/// 菜单开关热键。渲染线程（界面显示）与窗口线程（热键判定 / 改键）都会读，用锁保护。
std::mutex     gMenuKeysMutex;
input::KeyBind gMenuToggleKeys;

/// 改键捕获状态
std::atomic_bool                        gCapturing{false};
std::mutex                              gCaptureMutex;
std::string                             gCaptureTarget{};
Overlay::CaptureCallback                gCaptureCallback{};
std::vector<int>                        gCaptureCandidate{};
std::array<bool, input::kMaxVirtualKey> gCaptureDown{};
bool                                    gCaptureAnyReleased{};

/// 底部提示。同一时间只保留一条 —— 后来的直接覆盖前面的，不堆叠。
std::mutex  gToastMutex;
std::string gToastText;
ULONGLONG   gToastExpireAt{};

/// 本次按键是否由我们自己翻成字符塞给 ImGui 了。
/// 用来吃掉随后可能到来的 `WM_CHAR`，避免同一个字符输入两遍。
bool gCharSynthesized{};

auto& logger() { return Mangrove::getInstance().getSelf().getLogger(); }

// ---------------------------------------------------------------------------
// 窗口 / 输入
// ---------------------------------------------------------------------------

HWND findGameWindow() {
    struct Search {
        DWORD pid;
        HWND  result;
    } search{GetCurrentProcessId(), nullptr};

    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto& state = *reinterpret_cast<Search*>(parameter);
            DWORD pid{};
            GetWindowThreadProcessId(window, &pid);
            if (pid == state.pid && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
                state.result = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search)
    );
    return search.result;
}

/// 转发窗口消息给游戏，同时记账「游戏已经按下过哪些键 / 鼠标键」
LRESULT forwardToGame(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wParam < gGameKeysDown.size()) {
        gGameKeysDown[static_cast<size_t>(wParam)] = true;
    } else if ((message == WM_KEYUP || message == WM_SYSKEYUP) && wParam < gGameKeysDown.size()) {
        gGameKeysDown[static_cast<size_t>(wParam)] = false;
    }
    switch (message) {
    case WM_LBUTTONDOWN:
        gGameMouseButtonsDown[0] = true;
        break;
    case WM_LBUTTONUP:
        gGameMouseButtonsDown[0] = false;
        break;
    case WM_RBUTTONDOWN:
        gGameMouseButtonsDown[1] = true;
        break;
    case WM_RBUTTONUP:
        gGameMouseButtonsDown[1] = false;
        break;
    case WM_MBUTTONDOWN:
        gGameMouseButtonsDown[2] = true;
        break;
    case WM_MBUTTONUP:
        gGameMouseButtonsDown[2] = false;
        break;
    case WM_XBUTTONDOWN:
        gGameMouseButtonsDown[GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4] = true;
        break;
    case WM_XBUTTONUP:
        gGameMouseButtonsDown[GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4] = false;
        break;
    default:
        break;
    }

    if (gOriginalWndProc) return CallWindowProcW(gOriginalWndProc, window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

/// 菜单打开时，游戏已经收到过这些按下事件；先补发抬起，
/// 否则玩家会一直保持移动 / 攻击状态。
void releaseGameInput(HWND window) {
    InputHandoffScope handoff; // 补发的事件不能被我们自己的拦截钩子吃掉

    for (size_t key = 0; key < gGameKeysDown.size(); ++key) {
        if (!gGameKeysDown[key]) continue;

        auto const virtualKey = static_cast<UINT>(key);
        auto const scanCode   = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
        LPARAM     keyUp      = 1 | (static_cast<LPARAM>(scanCode) << 16) | (1LL << 30) | (1LL << 31);
        auto const systemKey  = virtualKey == VK_MENU || virtualKey == VK_LMENU || virtualKey == VK_RMENU;
        forwardToGame(window, systemKey ? WM_SYSKEYUP : WM_KEYUP, virtualKey, keyUp);
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(window, &cursor);
    auto const                    position = MAKELPARAM(cursor.x, cursor.y);
    constexpr std::array<UINT, 5> kUpMessages{WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP};
    for (size_t button = 0; button < gGameMouseButtonsDown.size(); ++button) {
        if (!gGameMouseButtonsDown[button]) continue;

        WPARAM buttonParam{};
        if (button == 3) buttonParam = MAKEWPARAM(0, XBUTTON1);
        if (button == 4) buttonParam = MAKEWPARAM(0, XBUTTON2);
        forwardToGame(window, kUpMessages[button], buttonParam, position);
    }
}

/// 把光标收回窗口中心再让游戏重新捕获鼠标，避免关掉菜单时视角跳一下
bool confineCursorToClientCenter(HWND window) {
    RECT clientRect{};
    if (!window || !GetClientRect(window, &clientRect)) return false;

    POINT topLeft{clientRect.left, clientRect.top};
    POINT bottomRight{clientRect.right, clientRect.bottom};
    if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight)) return false;

    RECT const screenRect{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    if (!ClipCursor(&screenRect)) return false;
    return SetCursorPos((screenRect.left + screenRect.right) / 2, (screenRect.top + screenRect.bottom) / 2) != FALSE;
}

bool isMenuInputMessage(UINT message) {
    switch (message) {
    case WM_INPUT:
    case WM_INPUT_DEVICE_CHANGE:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
        return true;
    default:
        return false;
    }
}

LRESULT consumeMenuInputMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // 前台原始输入必须回到 DefWindowProc 让 User32 做清理，但不能转给游戏
    if (message == WM_INPUT) return DefWindowProcW(window, message, wParam, lParam);
    return 1;
}

// ---------------------------------------------------------------------------
// 菜单开关热键
// ---------------------------------------------------------------------------

input::KeyBind menuToggleKeysSnapshot() {
    std::lock_guard lock(gMenuKeysMutex);
    return gMenuToggleKeys;
}

/// @returns 是否需要吞掉这条消息
bool handleMenuToggleDown(unsigned int virtualKey) {
    if (virtualKey >= gHeldKeys.size()) return false;

    bool const alreadyHeld = gHeldKeys[virtualKey];
    gHeldKeys[virtualKey]  = true;
    if (alreadyHeld) return false; // 系统的自动重复

    auto const keys = menuToggleKeysSnapshot();
    if (keys.empty() || keys.trigger() != static_cast<int>(virtualKey)) return false;
    if (!keys.allHeld(gHeldKeys)) return false;

    for (int key : keys.keys()) gConsumedKeyReleases[static_cast<size_t>(key)] = true;
    Overlay::getInstance().toggle();
    return true;
}

/// @returns 是否需要吞掉这条消息
bool handleMenuToggleUp(unsigned int virtualKey) {
    if (virtualKey >= gHeldKeys.size()) return false;

    bool const consumed              = gConsumedKeyReleases[virtualKey];
    gConsumedKeyReleases[virtualKey] = false;
    gHeldKeys[virtualKey]            = false;
    return consumed;
}

// ---------------------------------------------------------------------------
// 文本框输入
// ---------------------------------------------------------------------------

/// 把一次 `WM_KEYDOWN` 翻成字符交给 ImGui。
///
/// ImGui 的 Win32 后端只会从 `WM_CHAR` 取文本，而 `WM_CHAR` 是消息循环里
/// `TranslateMessage` 生成的 —— 游戏自己的消息循环不一定调它（它用原始输入读键盘）。
/// 一旦没调，数字输入框就会「能点进去、能按加减，但打不了字」。
/// 所以这里自己用 `ToUnicodeEx` 补一份字符，并把随后可能到来的 `WM_CHAR` 吃掉
/// （见 `gCharSynthesized`）。
void submitKeyAsCharacter(WPARAM virtualKey, LPARAM keyData) {
    if (virtualKey >= input::kMaxVirtualKey) return;

    BYTE keyboardState[256]{};
    if (!::GetKeyboardState(keyboardState)) return;

    // Alt / Ctrl 组合是菜单快捷键和编辑快捷键（复制粘贴），不是文本
    if ((keyboardState[VK_MENU] & 0x80) != 0) return;
    if ((keyboardState[VK_CONTROL] & 0x80) != 0) return;

    auto const scanCode = static_cast<UINT>(LOBYTE(HIWORD(keyData)));
    HKL        layout   = ::GetKeyboardLayout(0);

    wchar_t buffer[8]{};
    // ToUnicodeEx 会消费「死键」状态，先空跑一次，第二次拿到的才是稳定结果
    ::ToUnicodeEx(static_cast<UINT>(virtualKey), scanCode, keyboardState, buffer, 8, 0, layout);
    auto const count = ::ToUnicodeEx(static_cast<UINT>(virtualKey), scanCode, keyboardState, buffer, 8, 0, layout);
    if (count <= 0) return;

    auto& io = ImGui::GetIO();
    for (int index = 0; index < count; ++index) io.AddInputCharacterUTF16(buffer[index]);
    gCharSynthesized = true;
}

/// 一条窗口消息换算出来的按键状态
struct PressedKey {
    int  code{}; ///< VK 编码（鼠标键也在同一套编码里）；0 表示这条消息不是按键
    bool down{};
};

/// 把窗口消息归一成「哪个键、按下还是抬起」。
///
/// 键盘和鼠标在这里被抹平成同一种东西 —— 上层（菜单开关热键、改键捕获）因此只用一套逻辑，
/// 鼠标键只是编码不同的另外几个键。
[[nodiscard]] PressedKey decodePressedKey(UINT message, WPARAM wParam) {
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        return {static_cast<int>(wParam), true};
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return {static_cast<int>(wParam), false};

    // 窗口类若带 CS_DBLCLKS，第二次按下会是 DBLCLK 而不是 DOWN，两个都当「按下」
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        return {input::kMouseLeft, true};
    case WM_LBUTTONUP:
        return {input::kMouseLeft, false};
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        return {input::kMouseRight, true};
    case WM_RBUTTONUP:
        return {input::kMouseRight, false};
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        return {input::kMouseMiddle, true};
    case WM_MBUTTONUP:
        return {input::kMouseMiddle, false};
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        return {GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? input::kMouseX1 : input::kMouseX2, true};
    case WM_XBUTTONUP:
        return {GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? input::kMouseX1 : input::kMouseX2, false};
    default:
        return {};
    }
}

// ---------------------------------------------------------------------------
// 改键捕获
// ---------------------------------------------------------------------------

void clearCaptureState() {
    gCaptureTarget.clear();
    gCaptureCallback = nullptr;
    gCaptureCandidate.clear();
    gCaptureDown.fill(false);
    gCaptureAnyReleased = false;
}

void finishCapture(std::optional<input::KeyBind> result) {
    std::string              target;
    Overlay::CaptureCallback callback;
    {
        std::lock_guard lock(gCaptureMutex);
        if (!gCapturing.exchange(false, std::memory_order_acq_rel)) return;
        target   = std::move(gCaptureTarget);
        callback = std::move(gCaptureCallback);
        clearCaptureState();
    }
    if (callback) callback(target, std::move(result));
}

/// 捕获中：吃掉所有按键 / 鼠标消息，直到玩家按下又放开了一组键。
///
/// 确认时机是「候选键全部抬起」—— 这样单键和组合键是同一套流程：
/// 按下 G 再松开 => 绑定 G；按下 X、C 再依次松开 => 绑定 X + C；
/// 鼠标同理：按下右键再松开 => 绑定右键；按住 X 再点右键 => 绑定 X + 右键。
/// @returns 是否已消费该消息（不是按键消息则返回 false，交回正常流程）
bool handleCaptureMessage(UINT message, WPARAM wParam) {
    auto const [code, down] = decodePressedKey(message, wParam);
    if (code == 0) return false;
    if (code >= static_cast<int>(gCaptureDown.size())) return true;

    auto const index = static_cast<size_t>(code);

    if (!down) {
        gCaptureDown[index] = false;

        std::optional<input::KeyBind> confirmed;
        {
            std::lock_guard lock(gCaptureMutex);
            if (std::ranges::find(gCaptureCandidate, code) != gCaptureCandidate.end()) gCaptureAnyReleased = true;
            if (gCaptureAnyReleased && !gCaptureCandidate.empty()) {
                auto const allReleased = std::ranges::none_of(gCaptureCandidate, [](int key) {
                    return gCaptureDown[static_cast<size_t>(key)];
                });
                if (allReleased) confirmed = input::KeyBind::make(gCaptureCandidate);
            }
        }
        if (confirmed) finishCapture(std::move(confirmed));
        return true;
    }

    if (code == VK_ESCAPE) {
        finishCapture(std::nullopt);
        return true;
    }

    // 按下即加入候选，界面可以实时预览（刚按下还没松就会显示出来）
    if (!gCaptureDown[index]) {
        std::lock_guard lock(gCaptureMutex);
        if (std::ranges::find(gCaptureCandidate, code) == gCaptureCandidate.end()) {
            gCaptureCandidate.push_back(code);
        }
    }
    gCaptureDown[index] = true;
    return true;
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------

/// 这个按键消息是不是「窗口 / 系统级动作」而非「游戏输入」。
///
/// 菜单只该拦游戏输入。目前这一类的成员只有：
///   - **F11**：切全屏。菜单自己就可能因为分辨率 / 全屏状态显示异常，
///     而那时候按不了 F11 基本就是无路可走（只能杀进程）。
///
/// @note 这里的实现在**窗口消息层**放行，`ui::InputGuard` 里也放行了同一些键 ——
///       文字上看着重复，但两条路径是独立的（游戏既可能从窗口消息拿键盘，
///       也可能从自己的 HID 控制器拿），只补一边就会出现「看起来放行了却还是没反应」。
[[nodiscard]] bool isWindowManagementKey(UINT message, WPARAM wParam) {
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return wParam == VK_F11;
    default:
        return false;
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kMsgRestoreGameMouse) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
        confineCursorToClientCenter(window);
        return 0;
    }

    // 失焦时把状态清干净：不然回来之后组合键判定会错
    if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && wParam == FALSE)) {
        gHeldKeys.fill(false);
        gConsumedKeyReleases.fill(false);
        gCharSynthesized = false;
        ClipCursor(nullptr);
        Overlay::getInstance().cancelCapture();
    }

    // 0) 改键捕获：按键与鼠标消息全部由覆盖层处理（含 Esc 取消），不触发开关也不进游戏
    if (gCapturing.load(std::memory_order_acquire) && handleCaptureMessage(message, wParam)) return 1;

    auto const imguiReady = gImGuiReady.load(std::memory_order_acquire);
    // 文本框聚焦时键盘整个归 ImGui：不判定菜单开关热键（正在打的字里出现 X 不算开关），
    // 也不给游戏
    bool const editingText = imguiReady && ImGui::GetIO().WantTextInput;

    // 1) 菜单开关热键。菜单打开时游戏输入被 InputGuard 拦掉，
    //    KeyInputEvent / MouseInputEvent 都不会发布，所以只能在这一层判定。
    //    键盘和鼠标用同一套判定：鼠标键就是编码在 VK 空间里的另外几个键。
    //
    //    菜单开着时**不触发**（所有热键都禁用），但抬起事件仍然要记账 ——
    //    否则把开关热键按下的那一发吞掉了、抬起却没吞，`gHeldKeys` 会一直留着「按住」，
    //    下次再按就成了自动重复，热键再也不响应。
    if (imguiReady && !editingText) {
        auto const [code, down] = decodePressedKey(message, wParam);
        if (code != 0) {
            bool const menuOpen = Overlay::getInstance().isVisible();

            if (!down) {
                if (handleMenuToggleUp(code)) return 1;
            } else if (!menuOpen && handleMenuToggleDown(code)) {
                // 命中：toggle() 已经把菜单打开。把游戏已经按下的键 / 鼠标键补发抬起，
                // 否则玩家会一直往前走、一直挖。
                releaseGameInput(window);
                return 1;
            }
        }
    }

    // 2) 窗口管理键不归菜单拦。
    //
    // 判据是「这个键是给窗口用的，还是给游戏用的」：覆盖层挡住游戏输入是应该的，
    // 但挡住切全屏这类窗口动作就成了「界面显示异常时无路可走」——自己把自己锁在外面。
    // 改键捕获在更靠前的步骤，所以这些键照样可以被绑成热键。
    if (imguiReady && isWindowManagementKey(message, wParam)) {
        return forwardToGame(window, message, wParam, lParam);
    }

    // 3) 菜单打开：键鼠交给 ImGui，其余输入全部吞掉
    if (imguiReady && Overlay::getInstance().isVisible()) {
        ClipCursor(nullptr);

        // 我们自己已经补过字符了，跟着来的 WM_CHAR 要吃掉，不然会输入两遍
        if (message == WM_CHAR && gCharSynthesized) {
            gCharSynthesized = false;
            return 1;
        }

        ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);

        // 游戏的消息循环不一定调 TranslateMessage，那样 WM_CHAR 永远不来，
        // 文本框就只剩加减按钮能用了。这里自己把按键翻成字符补进去。
        if (message == WM_KEYDOWN) {
            gCharSynthesized = false;
            if (ImGui::GetIO().WantTextInput) submitKeyAsCharacter(wParam, lParam);
        }

        if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
            gConsumeEscapeRelease = true;
            Overlay::getInstance().setVisible(false);
            confineCursorToClientCenter(window);
            return 1;
        }
        if (isMenuInputMessage(message)) return consumeMenuInputMessage(window, message, wParam, lParam);
    } else if (gConsumeEscapeRelease && (message == WM_KEYUP || message == WM_SYSKEYUP) && wParam == VK_ESCAPE) {
        // 关菜单用的那一发 Esc，抬起事件也得一起吞掉
        gConsumeEscapeRelease = false;
        return 1;
    }

    // 4) 菜单刚关闭的收尾窗口内，继续吞输入
    if (overlayOwnsGameInput() && isMenuInputMessage(message)) {
        return consumeMenuInputMessage(window, message, wParam, lParam);
    }

    return forwardToGame(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// ImGui 初始化 / 渲染
// ---------------------------------------------------------------------------

void releaseGraphics() {
    if (gGraphicsReady) {
        ImGui_ImplDX11_Shutdown();
        gGraphicsReady = false;
    }
    if (gDeviceContext) {
        ID3D11RenderTargetView* empty{};
        gDeviceContext->OMSetRenderTargets(1, &empty, nullptr);
        gDeviceContext->ClearState();
        gDeviceContext->Flush();
    }
    if (gDevice11On12) gDevice11On12->Release();
    if (gDeviceContext) gDeviceContext->Release();
    if (gDevice) gDevice->Release();
    gDevice11On12  = nullptr;
    gDeviceContext = nullptr;
    gDevice        = nullptr;
}

/// 保证 ImGui 上下文与图形后端就绪。
/// @note 菜单隐藏时也会初始化 —— 否则热键打开菜单的第一帧渲染不出来。
bool initializeGraphics(IDXGISwapChain* swapChain) {
    if (gImGuiReady.load(std::memory_order_acquire) && gGraphicsReady) return true;
    if (gShuttingDown.load(std::memory_order_acquire)) return false;

    // 每次重建都从干净的图形状态开始，顺便清掉上次失败留下的 COM 对象
    releaseGraphics();

    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&gDevice)))) {
        gDevice->GetImmediateContext(&gDeviceContext);
    } else {
        // Bedrock 用 D3D12，需要一个 D3D11On12 设备才能用 ImGui 的 DX11 后端
        if (!gGameQueue) return false;

        ID3D12Device* device12{};
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device12)))) return false;

        auto const result = D3D11On12CreateDevice(
            device12,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            reinterpret_cast<IUnknown**>(&gGameQueue),
            1,
            0,
            &gDevice,
            &gDeviceContext,
            nullptr
        );
        device12->Release();
        if (FAILED(result) || !gDevice) {
            releaseGraphics();
            return false;
        }
        if (FAILED(gDevice->QueryInterface(__uuidof(ID3D11On12Device), reinterpret_cast<void**>(&gDevice11On12)))) {
            releaseGraphics();
            return false;
        }
    }

    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swapChain->GetDesc(&description))) {
        releaseGraphics();
        return false;
    }

    auto const window = description.OutputWindow ? description.OutputWindow : findGameWindow();
    if (!window || (gWindow && window != gWindow)) {
        releaseGraphics();
        return false;
    }

    if (!gImGuiReady.load(std::memory_order_acquire)) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        theme::loadFonts();
        if (!ImGui_ImplWin32_Init(window)) {
            ImGui::DestroyContext();
            releaseGraphics();
            return false;
        }

        gWindow = window;
        gOriginalWndProc =
            reinterpret_cast<WNDPROC>(SetWindowLongPtrW(gWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProc)));
        gImGuiReady.store(true, std::memory_order_release);
        logger().debug("ImGui context created on window {}", static_cast<void*>(gWindow));
    }

    if (!ImGui_ImplDX11_Init(gDevice, gDeviceContext)) {
        releaseGraphics();
        return false;
    }

    gGraphicsReady = true;
    return true;
}

void releaseImGuiMouseState() {
    auto& io = ImGui::GetIO();
    for (bool& down : io.MouseDown) down = false;
    io.MouseWheel  = 0.0f;
    io.MouseWheelH = 0.0f;
    io.MousePos    = ImVec2(-FLT_MAX, -FLT_MAX);
}

/// 当前是否有未过期的提示
bool toastAlive() {
    std::lock_guard lock(gToastMutex);
    auto const      now = GetTickCount64();
    if (gToastText.empty() || now >= gToastExpireAt) {
        gToastText.clear();
        gToastExpireAt = 0;
        return false;
    }
    return true;
}

void drawToasts() {
    std::string text;
    {
        std::lock_guard lock(gToastMutex);
        if (gToastText.empty()) return;
        text = gToastText;
    }

    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
                                      | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
                                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                      | ImGuiWindowFlags_AlwaysAutoResize;

    auto const viewport = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(viewport.x * 0.5f, viewport.y - 96.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.78f);
    if (ImGui::Begin("##mangroveToast", nullptr, kFlags)) ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

void drawFrame(ID3D11RenderTargetView* target) {
    gDeviceContext->OMSetRenderTargets(1, &target, nullptr);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (Overlay::getInstance().isVisible()) menu::render();
    drawToasts();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    ID3D11RenderTargetView* empty{};
    gDeviceContext->OMSetRenderTargets(1, &empty, nullptr);
}

/// D3D11 交换链：直接拿后台缓冲做渲染目标
void drawOnSwapChain(IDXGISwapChain* swapChain) {
    ID3D11Texture2D* backBuffer{};
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) return;

    ID3D11RenderTargetView* target{};
    auto const              result = gDevice->CreateRenderTargetView(backBuffer, nullptr, &target);
    backBuffer->Release();
    if (SUCCEEDED(result) && target) {
        drawFrame(target);
        target->Release();
    }
}

/// D3D12 交换链（Bedrock 的常见情况）：把后台缓冲包成 D3D11 资源再渲染
void drawOnWrappedBackBuffer(IDXGISwapChain* swapChain) {
    UINT             index{};
    IDXGISwapChain3* swapChain3{};
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swapChain3)))) {
        index = swapChain3->GetCurrentBackBufferIndex();
        swapChain3->Release();
    }

    ID3D12Resource* backBuffer{};
    if (FAILED(swapChain->GetBuffer(index, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&backBuffer)))) return;

    ID3D11Resource*      wrappedBuffer{};
    D3D11_RESOURCE_FLAGS resourceFlags{D3D11_BIND_RENDER_TARGET};
    auto const           wrapResult = gDevice11On12->CreateWrappedResource(
        backBuffer,
        &resourceFlags,
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_PRESENT,
        __uuidof(ID3D11Resource),
        reinterpret_cast<void**>(&wrappedBuffer)
    );
    backBuffer->Release();
    if (FAILED(wrapResult) || !wrappedBuffer) return;

    ID3D11RenderTargetView* target{};
    if (SUCCEEDED(gDevice->CreateRenderTargetView(wrappedBuffer, nullptr, &target)) && target) {
        gDevice11On12->AcquireWrappedResources(&wrappedBuffer, 1);
        drawFrame(target);
        gDevice11On12->ReleaseWrappedResources(&wrappedBuffer, 1);
        gDeviceContext->Flush();
        target->Release();
    }
    wrappedBuffer->Release();
}

void render(IDXGISwapChain* swapChain) {
    if (gShuttingDown.load(std::memory_order_acquire)) return;
    // Present 可能被多个线程调用，同一时刻只允许一帧
    if (gRendering.exchange(true, std::memory_order_acq_rel)) return;
    struct RenderingReset {
        ~RenderingReset() { gRendering.store(false, std::memory_order_release); }
    } renderingReset;

    std::lock_guard lock(gResourceMutex);
    if (!initializeGraphics(swapChain)) return;

    auto const visible = Overlay::getInstance().isVisible();

    // 刚关掉菜单：清掉 ImGui 累积的鼠标状态，否则下次打开会以为按键还按着
    if (gVisibleLastFrame && !visible) releaseImGuiMouseState();
    gVisibleLastFrame = visible;

    // 菜单关着又没有提示要画时，完全不进入 ImGui 帧
    if (!visible && !toastAlive()) return;

    ImGui::GetIO().MouseDrawCursor = visible;
    if (visible) ClipCursor(nullptr);

    if (gDevice11On12) {
        drawOnWrappedBackBuffer(swapChain);
    } else {
        drawOnSwapChain(swapChain);
    }
}

// ---------------------------------------------------------------------------
// 钩子安装 / 卸载
// ---------------------------------------------------------------------------

HRESULT __stdcall presentHook(IDXGISwapChain* swapChain, UINT interval, UINT flags);
HRESULT __stdcall
present1Hook(IDXGISwapChain1* swapChain, UINT interval, UINT flags, DXGI_PRESENT_PARAMETERS const* parameters);
HRESULT __stdcall
resizeBuffersHook(IDXGISwapChain* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags);
HRESULT __stdcall resizeBuffers1Hook(
    IDXGISwapChain3* swapChain,
    UINT             count,
    UINT             width,
    UINT             height,
    DXGI_FORMAT      format,
    UINT             flags,
    UINT const*      creationNodeMask,
    IUnknown* const* presentQueue
);
void __stdcall executeCommandListsHook(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);

bool installHook(void* target, void* detour, void** original) {
    return target && MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

void removeHook(void*& target) {
    if (!target) return;
    MH_DisableHook(target);
    MH_RemoveHook(target);
    target = nullptr;
}

/// 创建一个一次性设备 / 交换链，仅用于拿到 DXGI 各入口的地址
bool hookSwapChainEntryPoints(HWND window) {
    RECT clientRect{};
    if (!GetClientRect(window, &clientRect)) return false;
    // 窗口刚创建时客户区可能是 0x0，交换链会创建失败，给个兜底尺寸
    auto const width  = static_cast<UINT>(std::max<LONG>(clientRect.right - clientRect.left, 8));
    auto const height = static_cast<UINT>(std::max<LONG>(clientRect.bottom - clientRect.top, 8));

    D3D_FEATURE_LEVEL    featureLevel = D3D_FEATURE_LEVEL_11_0;
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount       = 1;
    description.BufferDesc.Width  = width;
    description.BufferDesc.Height = height;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow      = window;
    description.SampleDesc.Count  = 1;
    description.Windowed          = TRUE;
    description.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device*        dummyDevice{};
    ID3D11DeviceContext* dummyContext{};
    IDXGISwapChain*      dummySwapChain{};
    if (FAILED(D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            &featureLevel,
            1,
            D3D11_SDK_VERSION,
            &description,
            &dummySwapChain,
            &dummyDevice,
            nullptr,
            &dummyContext
        ))) {
        logger().warn("Failed to create a temporary swap chain; the overlay will be retried later");
        return false;
    }

    auto** vtable = *reinterpret_cast<void***>(dummySwapChain);
    bool   ok     = installHook(
                  vtable[kPresentVtableIndex],
                  reinterpret_cast<void*>(presentHook),
                  reinterpret_cast<void**>(&gOriginalPresent)
              )
           && installHook(
                  vtable[kResizeBuffersVtableIndex],
                  reinterpret_cast<void*>(resizeBuffersHook),
                  reinterpret_cast<void**>(&gOriginalResizeBuffers)
           );
    gPresentTarget       = vtable[kPresentVtableIndex];
    gResizeBuffersTarget = vtable[kResizeBuffersVtableIndex];

    IDXGISwapChain1* swapChain1{};
    if (SUCCEEDED(dummySwapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapChain1)))) {
        gPresent1Target = (*reinterpret_cast<void***>(swapChain1))[kPresent1VtableIndex];
        ok              = installHook(
                 gPresent1Target,
                 reinterpret_cast<void*>(present1Hook),
                 reinterpret_cast<void**>(&gOriginalPresent1)
             )
          && ok;
        swapChain1->Release();
    }

    IDXGISwapChain3* swapChain3{};
    if (SUCCEEDED(dummySwapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swapChain3)))) {
        gResizeBuffers1Target = (*reinterpret_cast<void***>(swapChain3))[kResizeBuffers1VtableIndex];
        ok                    = installHook(
                 gResizeBuffers1Target,
                 reinterpret_cast<void*>(resizeBuffers1Hook),
                 reinterpret_cast<void**>(&gOriginalResizeBuffers1)
             )
          && ok;
        swapChain3->Release();
    }

    dummySwapChain->Release();
    dummyContext->Release();
    dummyDevice->Release();
    return ok;
}

/// 钩住 D3D12 队列的 ExecuteCommandLists，以便提前拿到游戏的直接队列
bool hookCommandQueueEntryPoint() {
    ID3D12Device* dummyDevice12{};
    if (FAILED(D3D12CreateDevice(
            nullptr,
            D3D_FEATURE_LEVEL_11_0,
            __uuidof(ID3D12Device),
            reinterpret_cast<void**>(&dummyDevice12)
        ))) {
        return false;
    }

    bool                     ok = false;
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* dummyQueue{};
    if (SUCCEEDED(dummyDevice12->CreateCommandQueue(
            &queueDescription,
            __uuidof(ID3D12CommandQueue),
            reinterpret_cast<void**>(&dummyQueue)
        ))) {
        gExecuteCommandListsTarget = (*reinterpret_cast<void***>(dummyQueue))[kExecuteCommandListsVtableIndex];
        ok                         = installHook(
            gExecuteCommandListsTarget,
            reinterpret_cast<void*>(executeCommandListsHook),
            reinterpret_cast<void**>(&gOriginalExecuteCommandLists)
        );
        dummyQueue->Release();
    }
    dummyDevice12->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// 钩子实现
// ---------------------------------------------------------------------------

HRESULT __stdcall presentHook(IDXGISwapChain* swapChain, UINT interval, UINT flags) {
    render(swapChain);
    return gOriginalPresent(swapChain, interval, flags);
}

HRESULT __stdcall
present1Hook(IDXGISwapChain1* swapChain, UINT interval, UINT flags, DXGI_PRESENT_PARAMETERS const* parameters) {
    render(static_cast<IDXGISwapChain*>(swapChain));
    return gOriginalPresent1(swapChain, interval, flags, parameters);
}

HRESULT __stdcall
resizeBuffersHook(IDXGISwapChain* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    std::unique_lock lock(gResourceMutex);
    // ResizeBuffers 要求旧的后台缓冲引用全部释放，先拆掉图形后端，下一帧 Present 再重建
    releaseGraphics();
    lock.unlock();
    return gOriginalResizeBuffers(swapChain, count, width, height, format, flags);
}

HRESULT __stdcall resizeBuffers1Hook(
    IDXGISwapChain3* swapChain,
    UINT             count,
    UINT             width,
    UINT             height,
    DXGI_FORMAT      format,
    UINT             flags,
    UINT const*      creationNodeMask,
    IUnknown* const* presentQueue
) {
    std::unique_lock lock(gResourceMutex);
    releaseGraphics();
    lock.unlock();
    return gOriginalResizeBuffers1(swapChain, count, width, height, format, flags, creationNodeMask, presentQueue);
}

void __stdcall executeCommandListsHook(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    // 记住游戏的 D3D12 直接队列，D3D11On12CreateDevice 需要它
    if (!gGameQueue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        std::lock_guard lock(gResourceMutex);
        if (!gGameQueue) {
            gGameQueue = queue;
            gGameQueue->AddRef();
        }
    }
    gOriginalExecuteCommandLists(queue, count, lists);
}

} // namespace

// ---------------------------------------------------------------------------
// Overlay
// ---------------------------------------------------------------------------

Overlay& Overlay::getInstance() {
    static Overlay instance;
    return instance;
}

bool Overlay::install() {
    if (gInstalled.load(std::memory_order_acquire)) return true;

    auto const window = findGameWindow();
    if (!window) {
        logger().error("Game window not found; the overlay cannot be installed");
        return false;
    }

    auto const status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        logger().error("MinHook initialization failed (status {})", static_cast<int>(status));
        return false;
    }

    gShuttingDown.store(false, std::memory_order_release);

    // 菜单开关热键：优先用持久化的改键，没有记录就 X + C
    auto const defaultKeys = std::vector<int>{kDefaultMenuToggleKeys.begin(), kDefaultMenuToggleKeys.end()};
    auto       menuKeys    = input::KeyBind::make(core::Config::getInstance().getKeys(kMenuToggleBinding));
    if (!menuKeys) menuKeys = input::KeyBind::make(defaultKeys);
    {
        std::lock_guard lock(gMenuKeysMutex);
        gMenuToggleKeys = *menuKeys;
    }

    if (!hookSwapChainEntryPoints(window)) {
        logger().warn("Failed to install the DXGI hooks; the overlay will be retried later");
        uninstall();
        return false;
    }
    // 只在 D3D12 的机器上需要；失败不致命，D3D11 路径仍然能渲染
    if (!hookCommandQueueEntryPoint()) {
        logger().warn("Failed to hook the D3D12 command queue; D3D11On12 rendering may be unavailable");
    }

    auto const guard = installInputGuard();
    if (!guard.mouseHookInstalled) logger().warn("Failed to install the menu mouse-input guard");
    if (!guard.keyDownHookInstalled) logger().warn("Failed to install the menu key-down guard");
    if (!guard.keyUpHookInstalled) logger().warn("Failed to install the menu key-up guard");

    gInstalled.store(true, std::memory_order_release);
    return true;
}

bool Overlay::isInstalled() const { return gInstalled.load(std::memory_order_acquire); }

void Overlay::uninstall() {
    bool const wasInstalled = gInstalled.load(std::memory_order_acquire);

    gShuttingDown.store(true, std::memory_order_release);
    gVisible.store(false, std::memory_order_release);
    ClipCursor(nullptr);

    // 先取消捕获，别让回调碰正在拆的钩子
    cancelCapture();

    removeHook(gExecuteCommandListsTarget);
    removeHook(gResizeBuffers1Target);
    removeHook(gPresent1Target);
    removeHook(gResizeBuffersTarget);
    removeHook(gPresentTarget);

    if (gOriginalWndProc && gWindow && IsWindow(gWindow)) {
        SetWindowLongPtrW(gWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(gOriginalWndProc));
    }
    gOriginalWndProc = nullptr;

    uninstallInputGuard();

    {
        std::lock_guard lock(gResourceMutex);
        releaseGraphics();
        if (gImGuiReady.load(std::memory_order_acquire)) {
            ImGui_ImplWin32_Shutdown();
            theme::reset();
            ImGui::DestroyContext();
            gImGuiReady.store(false, std::memory_order_release);
        }
        if (gGameQueue) {
            gGameQueue->Release();
            gGameQueue = nullptr;
        }
    }

    {
        std::lock_guard lock(gToastMutex);
        gToastText.clear();
        gToastExpireAt = 0;
    }

    gWindow               = nullptr;
    gVisibleLastFrame     = false;
    gConsumeEscapeRelease = false;
    gCharSynthesized      = false;
    gHeldKeys.fill(false);
    gConsumedKeyReleases.fill(false);
    gGameKeysDown.fill(false);
    gGameMouseButtonsDown.fill(false);

    gInstalled.store(false, std::memory_order_release);
    // 界面没了，热键也该恢复（重新 enable 时不会卡在「被压制」状态）
    input::KeyManager::getInstance().setSuppressed(false);
    if (wasInstalled) logger().debug("Overlay uninstalled");
}

bool Overlay::isVisible() const { return gVisible.load(std::memory_order_acquire); }

void Overlay::setVisible(bool visible) {
    if (gVisible.exchange(visible, std::memory_order_acq_rel) == visible) return;

    // 菜单是模态的：开着的时候一个热键都不响应，包括开关热键自己
    // （那个在 windowProc 里也被拦下，见那里的注释）。
    // 关掉后 `KeyManager` 会清掉按住状态，所以「菜单开着时按住的键」不会在关掉后补触发。
    input::KeyManager::getInstance().setSuppressed(visible);

    // 关菜单时如果还在改键，直接放弃这次捕获
    if (!visible) cancelCapture();

    // 打开 / 关闭后的短暂窗口内继续独占输入，避免触发键本身漏进游戏
    setOverlayInputCapture(visible);

    if (!visible && gWindow) {
        // 让窗口线程把光标收回中心，再让游戏重新捕获鼠标
        PostMessageW(gWindow, kMsgRestoreGameMouse, 0, 0);
    }
    logger().debug("Mangrove menu {}", visible ? "opened" : "closed");
}

void Overlay::toggle() { setVisible(!isVisible()); }

input::KeyBind Overlay::menuToggleKeys() const { return menuToggleKeysSnapshot(); }

void Overlay::setMenuToggleKeys(input::KeyBind keys) {
    if (keys.empty()) return;

    auto const defaultKeys =
        input::KeyBind::make(std::vector<int>{kDefaultMenuToggleKeys.begin(), kDefaultMenuToggleKeys.end()});
    // 只存 diff：改回 X + C 就删掉记录
    bool const sameAsDefault = defaultKeys && keys.equals(*defaultKeys);

    auto const stored = keys.keys();
    {
        std::lock_guard lock(gMenuKeysMutex);
        gMenuToggleKeys = std::move(keys);
    }

    core::Config::getInstance().setKeys(kMenuToggleBinding, sameAsDefault ? std::vector<int>{} : stored);
    logger().debug("Menu hotkey updated");
}

void Overlay::beginCapture(std::string target, CaptureCallback onFinished) {
    if (target.empty() || !onFinished) return;

    {
        std::lock_guard lock(gCaptureMutex);
        gCaptureTarget   = std::move(target);
        gCaptureCallback = std::move(onFinished);
        gCaptureCandidate.clear();
        gCaptureDown.fill(false);
        gCaptureAnyReleased = false;
    }
    gCapturing.store(true, std::memory_order_release);
}

void Overlay::cancelCapture() { finishCapture(std::nullopt); }

bool Overlay::isCapturing(std::string_view target) const {
    if (!gCapturing.load(std::memory_order_acquire)) return false;

    std::lock_guard lock(gCaptureMutex);
    return gCaptureTarget == target;
}

std::vector<int> Overlay::capturePreview() const {
    if (!gCapturing.load(std::memory_order_acquire)) return {};

    std::lock_guard lock(gCaptureMutex);
    return gCaptureCandidate;
}

void Overlay::notify(std::string message) {
    if (message.empty()) return;

    // 同一时间只留一条：后来的直接覆盖前面的
    std::lock_guard lock(gToastMutex);
    gToastText     = std::move(message);
    gToastExpireAt = GetTickCount64() + kToastLifetimeMs;
}

} // namespace mangrove::ui

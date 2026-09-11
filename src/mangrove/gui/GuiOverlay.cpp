#include "mangrove/gui/GuiOverlay.h"

#include "mangrove/Mangrove.h"
#include "mangrove/data/DataBase.h"
#include "mangrove/gui/GuiMenu.h"
#include "mangrove/gui/GuiTheme.h"
#include "mangrove/gui/MenuInputGuard.h"
#include "mangrove/input/KeyInputManager.h"
#include "mangrove/input/Keys.h"


#include <Windows.h>

// d3d11on12.h / d3d12.h 与部分 SDK 版本的 d3d12.h 会重复定义这几个结构体，先改名绕开
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
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// 新版 ImGui 的头文件里不再声明这个函数，需要自己引入
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace mangrove::gui {
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
// 菜单热键 / 输入
// ---------------------------------------------------------------------------

/// 开关组合键的默认值：按住 X 再按 C（与 KeyInputManager 的组合键语义一致）
constexpr std::array<int, 2> kDefaultToggleKeys{VK_X, VK_C};
constexpr size_t             kMaxVirtualKey = 256;
/// 关闭菜单后继续吞掉输入的收尾时长，避免关闭动作的抬起事件漏进游戏
constexpr ULONGLONG kInputHandoffDurationMs = 180;
/// 让窗口线程把系统箭头光标恢复回来（Present 跑在渲染线程，不能直接 SetCursor）
constexpr UINT kMsgRestoreNativeCursor = WM_APP + 0x101;

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

std::atomic_bool   gInstalled{false};
std::atomic_bool   gShuttingDown{false};
std::atomic_bool   gRendering{false};
std::atomic_bool   gImGuiInitialized{false};
std::atomic_bool   gVisible{false};
std::atomic_ullong gBlockGameInputUntil{};
bool               gGraphicsInitialized{};
bool               gVisibleLastFrame{};
bool               gMouseHandoffActive{};
std::mutex         gResourceMutex;

/// 我们自己吞掉的按下键，需要连带吞掉对应的抬起
std::array<bool, kMaxVirtualKey> gHeldKeys{};
std::array<bool, kMaxVirtualKey> gConsumedKeyReleases{};
/// 游戏已经收到的按键 / 鼠标按下状态，用于菜单打开时补发抬起
std::array<bool, kMaxVirtualKey> gGameKeysDown{};
std::array<bool, 5>              gGameMouseButtonsDown{};
bool                             gConsumeEscapeRelease{};

/// 菜单开关组合键。渲染线程（界面显示）与窗口线程（热键判定 / 改键）都会访问，用锁保护。
std::mutex       gToggleKeysMutex;
std::vector<int> gToggleKeys{kDefaultToggleKeys.begin(), kDefaultToggleKeys.end()};

/// 改键捕获状态。`gRebindTarget` / `gRebindCandidate` 会被渲染线程读取，用锁保护；
/// 其余只在窗口线程访问。
std::atomic_bool                 gRebinding{false};
std::mutex                       gRebindMutex;
std::string                      gRebindTarget{};
std::vector<int>                 gRebindCandidate{};
std::array<bool, kMaxVirtualKey> gRebindDown{};
bool                             gRebindAnyReleased{};

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
    MenuInputHandoffScope handoff; // 补发的事件不能被自己的拦截钩子吃掉

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

/// 把光标限制在窗口中心，避免关闭菜单时游戏按绝对位置重新捕获鼠标导致视角跳一下。
bool confineCursorToClientCenter(HWND window) {
    RECT clientRect{};
    if (!window || !GetClientRect(window, &clientRect)) return false;
    POINT topLeft{clientRect.left, clientRect.top};
    POINT bottomRight{clientRect.right, clientRect.bottom};
    if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight)) return false;
    RECT screenRect{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    if (!ClipCursor(&screenRect)) return false;
    return SetCursorPos((screenRect.left + screenRect.right) / 2, (screenRect.top + screenRect.bottom) / 2) != FALSE;
}

/// 组合键的触发键：最后一个非修饰键；全是修饰键时取最后一个。
int resolveTriggerKey(std::vector<int> const& keys) {
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        if (!input::isModifierKey(*it)) return *it;
    }
    return keys.empty() ? 0 : keys.back();
}

std::vector<int> toggleKeysSnapshot() {
    std::lock_guard lock(gToggleKeysMutex);
    return gToggleKeys;
}

bool isValidKeyList(std::vector<int> const& keys) {
    return !keys.empty()
        && std::ranges::all_of(keys, [](int key) { return key >= 0 && key < static_cast<int>(kMaxVirtualKey); });
}

/// 从 DataBase 读取开关组合键；没有记录时保留代码里的默认值。
void loadToggleKeys() {
    auto const stored = Mangrove::getInstance().getDataBase().getIntList(GuiOverlay::kToggleBinding);
    if (!stored || stored->empty()) return;
    if (!isValidKeyList(*stored)) {
        logger().warn("Ignoring an invalid persisted menu hotkey");
        return;
    }

    std::lock_guard lock(gToggleKeysMutex);
    gToggleKeys = *stored;
}

void persistToggleKeys(std::vector<int> const& keys) {
    if (!Mangrove::getInstance().getDataBase().setIntList(GuiOverlay::kToggleBinding, keys)) {
        logger().warn("Failed to persist the menu hotkey");
    }
}

/// 组合键按下：命中时返回 true，调用方需要吞掉这条消息。
bool handleToggleHotkeyDown(unsigned int virtualKey) {
    if (virtualKey >= gHeldKeys.size()) return false;
    bool const alreadyHeld = gHeldKeys[virtualKey];
    gHeldKeys[virtualKey]  = true;
    if (alreadyHeld) return false; // 自动重复

    auto const keys = toggleKeysSnapshot();
    if (keys.empty()) return false;
    if (static_cast<int>(virtualKey) != resolveTriggerKey(keys)) return false;

    for (int key : keys) {
        if (key < 0 || key >= static_cast<int>(kMaxVirtualKey)) return false;
        if (!gHeldKeys[static_cast<size_t>(key)]) return false;
    }

    for (int key : keys) gConsumedKeyReleases[static_cast<size_t>(key)] = true;
    GuiOverlay::getInstance().toggle();
    return true;
}

/// 组合键抬起：只有被我们吞掉过按下的键才需要吞掉抬起。
bool handleToggleHotkeyUp(unsigned int virtualKey) {
    if (virtualKey >= gHeldKeys.size()) return false;
    bool const consumed              = gConsumedKeyReleases[virtualKey];
    gConsumedKeyReleases[virtualKey] = false;
    gHeldKeys[virtualKey]            = false;
    return consumed;
}

std::string rebindTarget() {
    std::lock_guard lock(gRebindMutex);
    return gRebindTarget;
}

/// 捕获确认：把候选键写回对应绑定。`kToggleBinding` 走覆盖层自己的热键，
/// 其余绑定统一交给 KeyInputManager（它会连带持久化）。
void commitRebind(std::vector<int> const& keys) {
    auto const target = rebindTarget();
    if (target.empty()) return;

    if (target == GuiOverlay::kToggleBinding) {
        GuiOverlay::getInstance().setToggleKeys(keys);
    } else {
        input::KeyInputManager::getInstance().rebind(target, keys);
    }
    GuiOverlay::getInstance().cancelRebind();
}

/// 改键捕获：吃掉所有键盘消息，直到玩家按下并松开了一组键。
///
/// 确认时机是「候选键全部抬起」——这样单击和组合键是同一套流程：
/// 按下 G 再松手 => 绑定 G；按下 X、C 再依次松手 => 绑定 X + C。
/// @returns 是否已消费该消息（非键盘消息返回 false，交回正常流程）
bool handleRebindKeyMessage(UINT message, WPARAM wParam) {
    auto const virtualKey = static_cast<unsigned int>(wParam);
    if (virtualKey >= gRebindDown.size()) return true;

    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (virtualKey == VK_ESCAPE) {
            GuiOverlay::getInstance().cancelRebind();
            return true;
        }
        // 按下即加入候选，界面会实时显示（按下 F 还没松就先显示 F）
        if (!gRebindDown[virtualKey]) {
            std::lock_guard lock(gRebindMutex);
            if (std::ranges::find(gRebindCandidate, static_cast<int>(virtualKey)) == gRebindCandidate.end()) {
                gRebindCandidate.push_back(static_cast<int>(virtualKey));
            }
        }
        gRebindDown[virtualKey] = true;
        return true;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        gRebindDown[virtualKey] = false;

        // 候选键全部抬起才算确认；锁内只做判定，出锁再改绑定（避免重入）
        std::vector<int> confirmed;
        {
            std::lock_guard lock(gRebindMutex);
            if (std::ranges::find(gRebindCandidate, static_cast<int>(virtualKey)) != gRebindCandidate.end()) {
                gRebindAnyReleased = true;
            }
            if (gRebindAnyReleased && !gRebindCandidate.empty()) {
                auto const allReleased = std::ranges::none_of(gRebindCandidate, [](int key) {
                    return gRebindDown[static_cast<size_t>(key)];
                });
                if (allReleased) confirmed = gRebindCandidate;
            }
        }
        if (!confirmed.empty()) commitRebind(confirmed);
        return true;
    }
    default:
        return false;
    }
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
    // 前台原始输入必须回到 DefWindowProc 做 User32 的清理，但不能转给游戏
    if (message == WM_INPUT) return DefWindowProcW(window, message, wParam, lParam);
    return 1;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kMsgRestoreNativeCursor) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
        return 0;
    }
    if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && wParam == FALSE)) {
        gHeldKeys.fill(false);
        gConsumedKeyReleases.fill(false);
        gMouseHandoffActive = false;
        ClipCursor(nullptr);
        GuiOverlay::getInstance().cancelRebind();
    }

    // 0) 改键捕获：键盘消息全部由覆盖层处理（含 Esc 取消），不会触发开关或进入游戏
    if (gRebinding.load(std::memory_order_acquire) && handleRebindKeyMessage(message, wParam)) return 1;

    auto const imguiReady = gImGuiInitialized.load(std::memory_order_acquire);

    // 1) 菜单开关热键。菜单打开时游戏输入会被拦截，所以必须在这一层判定。
    if (imguiReady && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)) {
        if (handleToggleHotkeyDown(static_cast<unsigned int>(wParam))) {
            auto const visible = GuiOverlay::getInstance().isVisible();
            if (visible) {
                releaseGameInput(window);
            } else {
                confineCursorToClientCenter(window);
            }
            return 1;
        }
    }
    if (imguiReady && (message == WM_KEYUP || message == WM_SYSKEYUP)) {
        if (handleToggleHotkeyUp(static_cast<unsigned int>(wParam))) return 1;
    }

    // 2) 菜单打开：把输入交给 ImGui（鼠标 + 键盘），其余全部吞掉
    if (imguiReady && GuiOverlay::getInstance().isVisible()) {
        gMouseHandoffActive = false;
        ClipCursor(nullptr);
        ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
        if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
            gConsumeEscapeRelease = true;
            GuiOverlay::getInstance().setVisible(false);
            confineCursorToClientCenter(window);
            return 1;
        }
        if (isMenuInputMessage(message)) return consumeMenuInputMessage(window, message, wParam, lParam);
    } else if (gConsumeEscapeRelease && (message == WM_KEYUP || message == WM_SYSKEYUP) && wParam == VK_ESCAPE) {
        // 关掉菜单用的 Esc，抬起事件也要一起吞掉
        gConsumeEscapeRelease = false;
        return 1;
    }

    if (GuiOverlay::getInstance().shouldBlockGameInput() && isMenuInputMessage(message)) {
        return consumeMenuInputMessage(window, message, wParam, lParam);
    }
    return forwardToGame(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// ImGui 初始化 / 渲染
// ---------------------------------------------------------------------------

void loadFonts() {
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();

    // ImGui 内置字体没有中文，存在系统字体时换成微软雅黑，并按 2 倍字号构建图集
    constexpr char const* kChineseFont = "C:\\Windows\\Fonts\\msyh.ttc";
    if (std::filesystem::exists(kChineseFont)) {
        ImFontConfig config{};
        config.OversampleH = 2;
        config.OversampleV = 2;
        if (io.Fonts->AddFontFromFileTTF(kChineseFont, 32.0f, &config, io.Fonts->GetGlyphRangesChineseFull())) return;
    }
    io.Fonts->AddFontDefault();
}

void releaseGraphics() {
    if (gGraphicsInitialized) {
        ImGui_ImplDX11_Shutdown();
        gGraphicsInitialized = false;
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

/// 确保 ImGui 上下文与图形后端就绪（菜单隐藏时也会初始化，热键才可用）。
bool initializeGraphics(IDXGISwapChain* swapChain) {
    if (gImGuiInitialized.load(std::memory_order_acquire) && gGraphicsInitialized) return true;
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

    if (!gImGuiInitialized.load(std::memory_order_acquire)) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        loadFonts();
        if (!ImGui_ImplWin32_Init(window)) {
            ImGui::DestroyContext();
            releaseGraphics();
            return false;
        }
        gWindow = window;
        gOriginalWndProc =
            reinterpret_cast<WNDPROC>(SetWindowLongPtrW(gWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProc)));
        gImGuiInitialized.store(true, std::memory_order_release);
        logger().debug("ImGui context created on window {}", static_cast<void*>(gWindow));
    }

    if (!ImGui_ImplDX11_Init(gDevice, gDeviceContext)) {
        releaseGraphics();
        return false;
    }
    gGraphicsInitialized = true;
    return true;
}

void drawFrame(ID3D11RenderTargetView* target) {
    gDeviceContext->OMSetRenderTargets(1, &target, nullptr);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    renderMenu();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    ID3D11RenderTargetView* empty{};
    gDeviceContext->OMSetRenderTargets(1, &empty, nullptr);
}

void releaseImGuiMouseState() {
    auto& io = ImGui::GetIO();
    for (bool& down : io.MouseDown) down = false;
    io.MouseWheel  = 0.0f;
    io.MouseWheelH = 0.0f;
    io.MousePos    = ImVec2(-FLT_MAX, -FLT_MAX);
}

void render(IDXGISwapChain* swapChain) {
    if (gShuttingDown.load(std::memory_order_acquire)) return;
    // Present 可能被多个线程调用，同一时刻只允许一帧
    if (gRendering.exchange(true, std::memory_order_acq_rel)) return;
    struct Reset {
        ~Reset() { gRendering.store(false, std::memory_order_release); }
    } reset;

    std::lock_guard lock(gResourceMutex);

    if (!initializeGraphics(swapChain)) return;

    auto const visible = GuiOverlay::getInstance().isVisible();
    auto&      io      = ImGui::GetIO();
    io.MouseDrawCursor = visible;

    if (gVisibleLastFrame && !visible) {
        // 刚关闭：清掉 ImGui 累积的鼠标状态，并把系统光标交还给游戏
        releaseImGuiMouseState();
        if (gWindow) PostMessageW(gWindow, kMsgRestoreNativeCursor, 0, 0);
    }
    gVisibleLastFrame = visible;

    if (!visible) {
        // 菜单隐藏时保持输入接管收尾，然后完全不进入 ImGui 帧
        if (gMouseHandoffActive && gWindow && GuiOverlay::getInstance().shouldBlockGameInput()) {
            confineCursorToClientCenter(gWindow);
        } else {
            gMouseHandoffActive = false;
        }
        return;
    }

    ClipCursor(nullptr);

    if (gDevice11On12) {
        UINT             index{};
        IDXGISwapChain3* swapChain3{};
        if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swapChain3)))) {
            index = swapChain3->GetCurrentBackBufferIndex();
            swapChain3->Release();
        }

        ID3D12Resource* backBuffer{};
        if (FAILED(swapChain->GetBuffer(index, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&backBuffer))))
            return;

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
    } else {
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
}

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

// ---------------------------------------------------------------------------
// 钩子管理
// ---------------------------------------------------------------------------

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
    // 窗口刚创建时客户区可能是 0x0，交换链会创建失败，这里给个兜底尺寸
    RECT clientRect{};
    if (!GetClientRect(window, &clientRect)) return false;
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
        logger().warn("Failed to create a temporary swap chain; the ImGui overlay will be retried later");
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
        )))
        return false;

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

} // namespace

// ---------------------------------------------------------------------------
// GuiOverlay
// ---------------------------------------------------------------------------

GuiOverlay& GuiOverlay::getInstance() {
    static GuiOverlay instance;
    return instance;
}

bool GuiOverlay::install() {
    if (gInstalled.load(std::memory_order_acquire)) return true;

    auto const window = findGameWindow();
    if (!window) {
        logger().error("Game window not found; the ImGui overlay cannot be installed");
        return false;
    }

    auto const status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        logger().error("MinHook initialization failed (status {})", static_cast<int>(status));
        return false;
    }

    gShuttingDown.store(false, std::memory_order_release);
    loadToggleKeys();

    if (!hookSwapChainEntryPoints(window)) {
        logger().warn("Failed to install the DXGI hooks; the ImGui overlay will be retried later");
        uninstall();
        return false;
    }
    // 只在 D3D12 的机器上需要；失败不致命，D3D11 路径仍可渲染
    if (!hookCommandQueueEntryPoint()) {
        logger().warn("Failed to hook the D3D12 command queue; D3D11On12 rendering may be unavailable");
    }

    auto const guard = installMenuInputGuard();
    if (!guard.mouseInputHookInstalled) logger().warn("Failed to install the menu mouse-input guard");
    if (!guard.keyDownInputHookInstalled) logger().warn("Failed to install the menu key-down guard");
    if (!guard.keyUpInputHookInstalled) logger().warn("Failed to install the menu key-up guard");

    gInstalled.store(true, std::memory_order_release);
    return true;
}

bool GuiOverlay::isInstalled() const { return gInstalled.load(std::memory_order_acquire); }

void GuiOverlay::uninstall() {
    // 安装中途失败也会走到这里，此时不该打出"已关闭"的日志
    bool const wasInstalled = gInstalled.load(std::memory_order_acquire);

    gShuttingDown.store(true, std::memory_order_release);
    gVisible.store(false, std::memory_order_release);
    gMouseHandoffActive = false;
    ClipCursor(nullptr);

    removeHook(gExecuteCommandListsTarget);
    removeHook(gResizeBuffers1Target);
    removeHook(gPresent1Target);
    removeHook(gResizeBuffersTarget);
    removeHook(gPresentTarget);

    if (gOriginalWndProc && gWindow && IsWindow(gWindow)) {
        SetWindowLongPtrW(gWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(gOriginalWndProc));
    }
    gOriginalWndProc = nullptr;

    uninstallMenuInputGuard();

    {
        std::lock_guard lock(gResourceMutex);
        releaseGraphics();
        if (gImGuiInitialized.load(std::memory_order_acquire)) {
            ImGui_ImplWin32_Shutdown();
            resetTheme();
            ImGui::DestroyContext();
            gImGuiInitialized.store(false, std::memory_order_release);
        }
        if (gGameQueue) {
            gGameQueue->Release();
            gGameQueue = nullptr;
        }
    }

    gWindow               = nullptr;
    gVisibleLastFrame     = false;
    gConsumeEscapeRelease = false;
    gHeldKeys.fill(false);
    gConsumedKeyReleases.fill(false);
    gGameKeysDown.fill(false);
    gGameMouseButtonsDown.fill(false);
    GuiOverlay::getInstance().cancelRebind();

    gInstalled.store(false, std::memory_order_release);
    if (wasInstalled) logger().debug("ImGui overlay uninstalled");
}

bool GuiOverlay::isVisible() const { return gVisible.load(std::memory_order_acquire); }

void GuiOverlay::setVisible(bool visible) {
    if (gVisible.exchange(visible, std::memory_order_acq_rel) == visible) return;

    // 关掉菜单时如果还在改键，直接放弃这次捕获
    if (!visible) cancelRebind();

    // 打开 / 关闭后的短暂窗口内继续独占输入，避免触发键本身漏进游戏
    gBlockGameInputUntil.store(GetTickCount64() + kInputHandoffDurationMs, std::memory_order_release);
    if (visible) {
        gMouseHandoffActive = false;
    } else if (gWindow) {
        // 关闭后先把光标收回窗口中心，再让游戏重新捕获鼠标
        gMouseHandoffActive = confineCursorToClientCenter(gWindow);
    }
    logger().debug("Mangrove GUI {}", visible ? "opened" : "closed");
}

void GuiOverlay::toggle() { setVisible(!isVisible()); }

std::vector<int> GuiOverlay::getToggleKeys() const { return toggleKeysSnapshot(); }

void GuiOverlay::setToggleKeys(std::vector<int> keys) {
    cancelRebind();
    if (!isValidKeyList(keys)) {
        logger().warn("Ignoring an invalid menu hotkey");
        return;
    }

    {
        std::lock_guard lock(gToggleKeysMutex);
        gToggleKeys = std::move(keys);
    }
    persistToggleKeys(toggleKeysSnapshot());
    logger().debug("Menu hotkey updated");
}

void GuiOverlay::beginRebind(std::string bindingName) {
    if (bindingName.empty()) return;

    {
        std::lock_guard lock(gRebindMutex);
        gRebindTarget = std::move(bindingName);
        gRebindCandidate.clear();
    }
    gRebindDown.fill(false);
    gRebindAnyReleased = false;
    gRebinding.store(true, std::memory_order_release);
}

void GuiOverlay::cancelRebind() {
    if (!gRebinding.exchange(false, std::memory_order_acq_rel)) return;
    {
        std::lock_guard lock(gRebindMutex);
        gRebindTarget.clear();
        gRebindCandidate.clear();
    }
    gRebindDown.fill(false);
    gRebindAnyReleased = false;
}

bool GuiOverlay::isRebinding() const { return gRebinding.load(std::memory_order_acquire); }

bool GuiOverlay::isRebinding(std::string_view bindingName) const {
    if (!gRebinding.load(std::memory_order_acquire)) return false;

    std::lock_guard lock(gRebindMutex);
    return gRebindTarget == bindingName;
}

std::vector<int> GuiOverlay::getRebindPreview() const {
    std::lock_guard lock(gRebindMutex);
    return gRebindCandidate;
}

bool GuiOverlay::shouldBlockGameInput() const {
    return isVisible() || GetTickCount64() <= gBlockGameInputUntil.load(std::memory_order_acquire);
}

} // namespace mangrove::gui

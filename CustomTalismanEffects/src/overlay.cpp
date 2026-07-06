// In-game config overlay: Dear ImGui drawn in a SEPARATE, INDEPENDENT transparent
// top-most window with its own D3D11 + DirectComposition device, on its own thread.
//
// Why a separate window (not a swapchain hook): we no longer touch the game's DXGI
// swapchain at all. That makes the overlay compatible with anything that wraps or
// renders on the game's swapchain -- other overlay mods (MapForGoblins 2.0.4+),
// Special K, NVIDIA Smooth Motion / frame-gen, ReShade. The old Present/
// ExecuteCommandLists DX12 hook shared GPU + command-queue state with the game and
// with those tools, and that shared state getting corrupted crashed the game.
//
// Architecture:
//   - setup() queues the input hooks and spawns a detached overlay thread that
//     creates the window + D3D11 + DComp + ImGui DX11 backend and runs the render
//     loop. setup() returns fast; the thread owns all overlay state.
//   - Window: WS_POPUP + WS_EX_NOREDIRECTIONBITMAP|TOPMOST|NOACTIVATE|TOOLWINDOW;
//     transparency via DirectComposition (premultiplied alpha). Shown only while
//     the menu is open, hidden when closed (a hidden window touches no input).
//   - Each iteration the window is moved to exactly cover the game's client area.
//   - Open/close (Insert / L3+R3) + Esc/B are polled with GetAsyncKeyState. We do
//     NOT steal the game's focus.
//   - Mouse comes to our own window (WM_MOUSE -> ImGui_ImplWin32_WndProcHandler);
//     the SetCursorPos/ClipCursor hooks free the OS cursor while the menu is open so
//     it can reach our window. Keyboard (incl. typed text for the search box) is
//     captured from the game's GetRawInputData and fed to ImGui; the same hook
//     neutralizes the game's keyboard/mouse so menu input never leaks into gameplay.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <Xinput.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "overlay.hpp"
#include "hooks.hpp"
#include "state.hpp"
#include "log.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "shell32.lib")

// From imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace cte {
namespace {

// Mod page opened by the small "Nexus" button in the overlay. Leave empty ("")
// to hide the button entirely.
constexpr const char* kModPageUrl = "https://www.nexusmods.com/eldenring/mods/10327";

const wchar_t* kOverlayClass = L"CTE_OverlayWindow";

using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using SetCursorPos_t     = BOOL(WINAPI*)(int, int);
using ClipCursor_t       = BOOL(WINAPI*)(const RECT*);
using XInputGetState_t   = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

GetRawInputData_t oGetRawInputData = nullptr;
SetCursorPos_t    oSetCursorPos = nullptr;
ClipCursor_t      oClipCursor = nullptr;
XInputGetState_t  oXInputGetState = nullptr; // trampoline: OUR poll reads the real pad
XInputGetState_t  pXInputGetState = nullptr; // raw proc, in case the hook never applied

// Open/close inputs, snapshotted from g_state at setup().
unsigned int   g_open_vk = 0x2D;         // VK_INSERT
unsigned short g_open_pad_mask = 0x00C0; // L3+R3

// ── window + D3D11 + DirectComposition state (overlay thread only) ──
HWND g_hwnd = nullptr;      // our overlay window
HWND g_game_hwnd = nullptr; // tracked game main window (for cover)

ID3D11Device*           g_d3d_device = nullptr;
ID3D11DeviceContext*    g_d3d_ctx = nullptr;
IDXGISwapChain1*        g_swapchain = nullptr; // composition swapchain (BGRA, premult)
ID3D11RenderTargetView* g_rtv = nullptr;
IDCompositionDevice*    g_dcomp_device = nullptr;
IDCompositionTarget*    g_dcomp_target = nullptr;
IDCompositionVisual*    g_dcomp_visual = nullptr;
UINT g_back_w = 0, g_back_h = 0;

// Proton/Wine fallback: DirectComposition (CreateSwapChainForComposition) returns
// E_NOTIMPL under Wine, so there we present via a WS_EX_LAYERED window fed by
// UpdateLayeredWindow. Windows keeps the DComp path. g_use_layered selects it.
bool g_use_layered = false;
ID3D11Texture2D*        g_ltex = nullptr;
ID3D11RenderTargetView* g_lrtv = nullptr;
ID3D11Texture2D*        g_lstaging = nullptr;
HDC     g_lmemdc = nullptr;
HBITMAP g_ldib = nullptr;
void*   g_ldibbits = nullptr;

std::atomic<bool> g_running{false};   // overlay thread alive
std::atomic<bool> g_menu_open{false};
bool g_context_inited = false;        // ImGui context + win32 backend
bool g_d3d_inited = false;            // D3D11 + DComp + dx11 backend

// ── gamepad snapshot (real state read via the trampoline) ──
XINPUT_GAMEPAD g_pad{};
bool  g_pad_ok = false;

// ── keyboard capture (game message thread writes; render thread drains) ──
// The overlay window never holds focus, so it gets no WM_KEYDOWN/WM_CHAR. We hook
// the game's GetRawInputData instead: key events + synthesized characters (so the
// search box works) are queued here and drained into ImGui on the render thread.
struct KeyEv { ImGuiKey key; bool down; };
std::mutex           g_key_mtx;
std::vector<KeyEv>   g_key_events;   // guarded by g_key_mtx
std::vector<wchar_t> g_char_events;  // guarded by g_key_mtx

inline bool kd(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Is the app in the foreground our own process (game or our overlay)? Gates the
// open/close polling so Insert/Esc don't act while alt-tabbed to another app.
bool foreground_is_ours() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ── ER-flavored dark/gold theme ──
void apply_er_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 2.0f;
    s.FrameRounding = 2.0f;
    s.WindowBorderSize = 1.0f;
    s.WindowPadding = ImVec2(12, 12);
    s.ItemSpacing = ImVec2(8, 6);
    ImVec4* c = s.Colors;
    const ImVec4 gold(0.80f, 0.68f, 0.40f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.05f, 0.04f, 0.96f);
    c[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.10f, 0.05f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.22f, 0.17f, 0.08f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.30f, 0.24f, 0.12f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.36f, 0.18f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.55f, 0.44f, 0.22f, 1.0f);
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold;
    c[ImGuiCol_Button] = ImVec4(0.25f, 0.20f, 0.10f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.40f, 0.32f, 0.16f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.52f, 0.42f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.12f, 0.07f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.20f, 0.11f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.88f, 0.78f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.50f, 0.40f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.50f, 0.42f, 0.25f, 0.6f);
    c[ImGuiCol_NavHighlight] = gold;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.24f, 0.13f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.22f, 0.17f, 0.08f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.45f, 0.36f, 0.18f, 1.0f);
    c[ImGuiCol_TabActive] = ImVec4(0.35f, 0.28f, 0.14f, 1.0f);
    c[ImGuiCol_TabUnfocused] = ImVec4(0.12f, 0.10f, 0.05f, 1.0f);
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.25f, 0.20f, 0.10f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.50f, 0.42f, 0.25f, 0.6f);
    c[ImGuiCol_SeparatorHovered] = ImVec4(0.65f, 0.53f, 0.30f, 0.8f);
    c[ImGuiCol_SeparatorActive] = gold;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.30f, 0.24f, 0.12f, 0.6f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.45f, 0.36f, 0.18f, 0.8f);
    c[ImGuiCol_ResizeGripActive] = gold;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.24f, 0.12f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.45f, 0.36f, 0.18f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.55f, 0.44f, 0.22f, 1.0f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.55f, 0.44f, 0.22f, 0.5f);
}

// ── input mapping / hooks ──
ImGuiKey vk_to_imgui(USHORT vk) {
    if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: return ImGuiKey_LeftCtrl;
    case VK_RCONTROL: return ImGuiKey_RightCtrl;
    case VK_SHIFT: case VK_LSHIFT: return ImGuiKey_LeftShift;
    case VK_RSHIFT: return ImGuiKey_RightShift;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_TAB: return ImGuiKey_Tab;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_ESCAPE: return ImGuiKey_Escape;
    default: return ImGuiKey_None;
    }
}

// Hook the game's raw-input read. While the menu is open we (a) capture the
// keyboard for ImGui (key events + layout-aware characters for the search box),
// and (b) neutralize the game's keyboard+mouse payload so menu input never leaks
// into gameplay. Runs on the game's message thread.
UINT WINAPI hkGetRawInputData(HRAWINPUT hri, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    UINT res = oGetRawInputData(hri, cmd, data, size, hdr);
    if (!g_menu_open.load() || cmd != RID_INPUT || data == nullptr ||
        res == static_cast<UINT>(-1))
        return res;

    auto* ri = reinterpret_cast<RAWINPUT*>(data);
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        // Menu mouse comes via our window's WM_MOUSE; kill the game's copy so the
        // camera doesn't move while the menu is open.
        ri->data.mouse.lLastX = 0;
        ri->data.mouse.lLastY = 0;
        ri->data.mouse.usButtonFlags = 0;
        ri->data.mouse.usButtonData = 0;
        ri->data.mouse.ulRawButtons = 0;
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& kb = ri->data.keyboard;
        const bool down = (kb.Flags & RI_KEY_BREAK) == 0;
        ImGuiKey k = vk_to_imgui(kb.VKey);
        {
            std::lock_guard<std::mutex> lk(g_key_mtx);
            if (k != ImGuiKey_None) g_key_events.push_back({k, down});
            if (down) {
                BYTE ks[256];
                if (GetKeyboardState(ks)) {
                    wchar_t buf[4]{};
                    const int n = ToUnicode(kb.VKey, kb.MakeCode, ks, buf, 4, 0);
                    for (int i = 0; i < n; ++i)
                        if (buf[i] >= 0x20 && buf[i] != 0x7F) // printable only
                            g_char_events.push_back(buf[i]);
                }
            }
        }
        ri->data.keyboard.VKey = 0;
        ri->data.keyboard.MakeCode = 0;
        ri->data.keyboard.Flags = RI_KEY_BREAK; // report a no-op key-up
        ri->data.keyboard.Message = WM_NULL;
    }
    return res;
}

// The game's FPS camera recenters (SetCursorPos) and confines (ClipCursor) the OS
// cursor every frame. While the menu is open we no-op those so the cursor moves
// freely and reaches our overlay window.
BOOL WINAPI hkSetCursorPos(int x, int y) {
    if (g_menu_open.load()) return TRUE;
    return oSetCursorPos(x, y);
}
BOOL WINAPI hkClipCursor(const RECT* r) {
    if (g_menu_open.load()) return oClipCursor(nullptr);
    return oClipCursor(r);
}

// While the menu is open, feed the GAME a neutral (connected, nothing pressed) pad
// so its buttons don't leak into gameplay. Our own poll_gamepad reads the real pad
// through the trampoline.
DWORD WINAPI hkXInputGetState(DWORD idx, XINPUT_STATE* st) {
    DWORD r = oXInputGetState(idx, st);
    if (r == ERROR_SUCCESS && st && g_menu_open.load()) {
        const DWORD pkt = st->dwPacketNumber;
        ZeroMemory(&st->Gamepad, sizeof(st->Gamepad));
        st->dwPacketNumber = pkt;
    }
    return r;
}

void poll_gamepad() {
    g_pad_ok = false;
    XInputGetState_t xget = oXInputGetState ? oXInputGetState : pXInputGetState;
    if (!xget) return;
    for (DWORD idx = 0; idx < XUSER_MAX_COUNT; ++idx) {
        XINPUT_STATE state{};
        if (xget(idx, &state) == ERROR_SUCCESS) {
            g_pad = state.Gamepad;
            g_pad_ok = true;
            break;
        }
    }
}

// Drain captured keyboard events/characters into ImGui (render thread).
void feed_keyboard() {
    ImGuiIO& io = ImGui::GetIO();
    std::lock_guard<std::mutex> lk(g_key_mtx);
    for (const auto& e : g_key_events) io.AddKeyEvent(e.key, e.down);
    g_key_events.clear();
    for (wchar_t ch : g_char_events) io.AddInputCharacterUTF16(static_cast<ImWchar16>(ch));
    g_char_events.clear();
}

// Feed the polled gamepad to ImGui nav (mouse + keyboard come in elsewhere).
void feed_gamepad() {
    if (!g_pad_ok) return;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const WORD nbt = g_pad.wButtons & ~g_open_pad_mask; // reserve the toggle combo
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    (nbt & XINPUT_GAMEPAD_DPAD_UP) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  (nbt & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  (nbt & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (nbt & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  (nbt & XINPUT_GAMEPAD_A) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (nbt & XINPUT_GAMEPAD_B) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp,    (nbt & XINPUT_GAMEPAD_Y) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft,  (nbt & XINPUT_GAMEPAD_X) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadL1,        (nbt & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadR1,        (nbt & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadStart,     (nbt & XINPUT_GAMEPAD_START) != 0);
    const float lx = g_pad.sThumbLX / 32767.0f;
    const float ly = g_pad.sThumbLY / 32767.0f;
    constexpr float DZ = 0.35f;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft,  lx < -DZ, lx < -DZ ? -lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx >  DZ, lx >  DZ ?  lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp,    ly >  DZ, ly >  DZ ?  ly : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown,  ly < -DZ, ly < -DZ ? -ly : 0.0f);
}

// ── window ──
LRESULT CALLBACK overlay_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 0;
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // never steal the game's focus on a click
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// Pick the foreground window if it belongs to our process (skip tool windows, i.e.
// our own overlay). Cached so we keep covering the game after focus moves.
HWND find_game_window() {
    HWND fg = GetForegroundWindow();
    if (fg && fg != g_hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            const LONG ex = GetWindowLongW(fg, GWL_EXSTYLE);
            if (!(ex & WS_EX_TOOLWINDOW)) g_game_hwnd = fg;
        }
    }
    return g_game_hwnd;
}

// Move/resize our overlay to exactly cover the game's client area.
void cover_game_window(int& out_w, int& out_h) {
    out_w = out_h = 0;
    HWND game = find_game_window();
    if (!game || !IsWindow(game)) return;
    RECT cr{};
    if (!GetClientRect(game, &cr)) return;
    POINT tl{cr.left, cr.top};
    ClientToScreen(game, &tl);
    const int w = cr.right - cr.left, h = cr.bottom - cr.top;
    if (w <= 0 || h <= 0) return;
    SetWindowPos(g_hwnd, HWND_TOPMOST, tl.x, tl.y, w, h, SWP_NOACTIVATE);
    out_w = w;
    out_h = h;
}

// ── Proton/Wine layered-window fallback (DComp is E_NOTIMPL under Wine) ──
void release_layered_targets() {
    if (g_lrtv) { g_lrtv->Release(); g_lrtv = nullptr; }
    if (g_ltex) { g_ltex->Release(); g_ltex = nullptr; }
    if (g_lstaging) { g_lstaging->Release(); g_lstaging = nullptr; }
    if (g_lmemdc) { DeleteDC(g_lmemdc); g_lmemdc = nullptr; }
    if (g_ldib) { DeleteObject(g_ldib); g_ldib = nullptr; }
    g_ldibbits = nullptr;
}

bool create_layered_targets(UINT w, UINT h) {
    release_layered_targets();
    if (!g_d3d_device || w == 0 || h == 0) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_d3d_device->CreateTexture2D(&td, nullptr, &g_ltex)) || !g_ltex) return false;
    if (FAILED(g_d3d_device->CreateRenderTargetView(g_ltex, nullptr, &g_lrtv)) || !g_lrtv) return false;
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_d3d_device->CreateTexture2D(&td, nullptr, &g_lstaging)) || !g_lstaging) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h); // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    g_ldib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_ldibbits, nullptr, 0);
    g_lmemdc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!g_ldib || !g_lmemdc || !g_ldibbits) return false;
    SelectObject(g_lmemdc, g_ldib);
    g_back_w = w; g_back_h = h;
    return true;
}

void recreate_window_layered() {
    if (g_hwnd) DestroyWindow(g_hwnd);
    const DWORD ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    g_hwnd = CreateWindowExW(ex, kOverlayClass, L"Custom Talisman Effects overlay", WS_POPUP,
                             0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// ── D3D11 + DirectComposition + ImGui dx11 backend ──
// POD-only locals: SEH-guarded by seh_init_d3d (a torn GPU state can AV).
bool init_d3d() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // BGRA required for DComp
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 want, 2, D3D11_SDK_VERSION,
                                 &g_d3d_device, &got, &g_d3d_ctx))) {
        flog("[overlay] D3D11CreateDevice failed");
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(g_d3d_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || !dxgiDevice)
        return false;
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter) {
        dxgiDevice->Release();
        return false;
    }
    IDXGIFactory2* factory = nullptr;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) || !factory) {
        adapter->Release();
        dxgiDevice->Release();
        return false;
    }

    RECT cr{};
    GetClientRect(g_hwnd, &cr);
    UINT w = static_cast<UINT>(cr.right - cr.left), h = static_cast<UINT>(cr.bottom - cr.top);
    if (w == 0) w = 1;
    if (h == 0) h = 1;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = w;
    scd.Height = h;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scd.Scaling = DXGI_SCALING_STRETCH;
    HRESULT hr = factory->CreateSwapChainForComposition(g_d3d_device, &scd, nullptr, &g_swapchain);
    factory->Release();
    adapter->Release();
    if (SUCCEEDED(hr) && g_swapchain) {
        // ── Windows path: DirectComposition (device -> target(hwnd) -> visual). ──
        g_back_w = w;
        g_back_h = h;
        if (FAILED(DCompositionCreateDevice(dxgiDevice, IID_PPV_ARGS(&g_dcomp_device))) || !g_dcomp_device) {
            dxgiDevice->Release();
            flog("[overlay] DCompositionCreateDevice failed");
            return false;
        }
        dxgiDevice->Release();
        if (FAILED(g_dcomp_device->CreateTargetForHwnd(g_hwnd, TRUE, &g_dcomp_target)) || !g_dcomp_target)
            return false;
        if (FAILED(g_dcomp_device->CreateVisual(&g_dcomp_visual)) || !g_dcomp_visual)
            return false;
        g_dcomp_visual->SetContent(g_swapchain);
        g_dcomp_target->SetRoot(g_dcomp_visual);
        g_dcomp_device->Commit();
        ID3D11Texture2D* back = nullptr;
        if (FAILED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back)
            return false;
        hr = g_d3d_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
        if (FAILED(hr) || !g_rtv)
            return false;
    } else {
        // ── Proton/Wine path: composition swapchains are E_NOTIMPL -> layered. ──
        dxgiDevice->Release();
        flog("[overlay] composition swapchain unavailable (0x%08X); using layered fallback",
             static_cast<unsigned>(hr));
        g_use_layered = true;
        recreate_window_layered();
        if (!g_hwnd) { flog("[overlay] layered window creation failed"); return false; }
        if (!create_layered_targets(w, h)) { flog("[overlay] layered targets failed"); return false; }
    }

    if (!g_context_inited) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; // don't drop an imgui.ini next to the game
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        // Draw our own software cursor; never touch the OS cursor image.
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        const char* base_fonts[] = {"C:\\Windows\\Fonts\\malgun.ttf",
                                    "C:\\Windows\\Fonts\\meiryo.ttc",
                                    "C:\\Windows\\Fonts\\msgothic.ttc",
                                    "C:\\Windows\\Fonts\\msyh.ttc",
                                    "C:\\Windows\\Fonts\\msjh.ttc",
                                    "C:\\Windows\\Fonts\\segoeui.ttf",
                                    "C:\\Windows\\Fonts\\arial.ttf",
                                    "C:\\Windows\\Fonts\\tahoma.ttf"};
        ImFont* base = nullptr;
        for (const char* fp : base_fonts) {
            if (GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES) {
                const ImWchar* ranges = nullptr;
                if (strstr(fp, "malgun.ttf") != nullptr) {
                    ranges = io.Fonts->GetGlyphRangesKorean();
                } else if (strstr(fp, "meiryo.ttc") != nullptr || strstr(fp, "msgothic.ttc") != nullptr) {
                    ranges = io.Fonts->GetGlyphRangesJapanese();
                } else if (strstr(fp, "msyh.ttc") != nullptr || strstr(fp, "msjh.ttc") != nullptr) {
                    ranges = io.Fonts->GetGlyphRangesChineseFull();
                }
                base = io.Fonts->AddFontFromFileTTF(fp, 18.0f, nullptr, ranges);
                if (base) break;
            }
        }
        if (!base) base = io.Fonts->AddFontDefault();
        apply_er_style();
        ImGui_ImplWin32_Init(g_hwnd);
        g_context_inited = true;
    }

    if (!ImGui_ImplDX11_Init(g_d3d_device, g_d3d_ctx)) {
        flog("[overlay] ImGui_ImplDX11_Init failed");
        return false;
    }
    g_d3d_inited = true;
    flog("[overlay] separate-window overlay ready (%ux%u, %s)", w, h,
         g_use_layered ? "layered" : "DComp");
    return true;
}

bool seh_init_d3d() {
    __try { return init_d3d(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void resize_swapchain(UINT w, UINT h) {
    if (g_use_layered) { create_layered_targets(w, h); return; }
    if (!g_swapchain || w == 0 || h == 0) return;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (FAILED(g_swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
        return;
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_d3d_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
    g_back_w = w;
    g_back_h = h;
}

void seh_resize(UINT w, UINT h) {
    __try { resize_swapchain(w, h); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// One rendered frame (SEH-wrapped). Clears to transparent; draws ImGui only when
// the caller passes draw==true; presents. POD-only locals.
void render_frame(bool draw) {
    __try {
        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // fully transparent
        if (g_use_layered) {
            if (!g_lrtv || !g_lstaging || !g_ltex || !g_ldib || !g_d3d_ctx) return;
            g_d3d_ctx->OMSetRenderTargets(1, &g_lrtv, nullptr);
            g_d3d_ctx->ClearRenderTargetView(g_lrtv, clear);
            if (draw) ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_d3d_ctx->CopyResource(g_lstaging, g_ltex);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(g_d3d_ctx->Map(g_lstaging, 0, D3D11_MAP_READ, 0, &m))) {
                const size_t rowbytes = static_cast<size_t>(g_back_w) * 4;
                for (UINT y = 0; y < g_back_h; ++y)
                    memcpy(static_cast<uint8_t*>(g_ldibbits) + static_cast<size_t>(y) * rowbytes,
                           static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                           rowbytes);
                g_d3d_ctx->Unmap(g_lstaging, 0);
                SIZE sz{static_cast<LONG>(g_back_w), static_cast<LONG>(g_back_h)};
                POINT src0{0, 0};
                BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                HDC screen = GetDC(nullptr);
                UpdateLayeredWindow(g_hwnd, screen, nullptr, &sz, g_lmemdc, &src0, 0, &bf, ULW_ALPHA);
                ReleaseDC(nullptr, screen);
            }
            return;
        }
        if (!g_rtv || !g_d3d_ctx || !g_swapchain) return;
        g_d3d_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_d3d_ctx->ClearRenderTargetView(g_rtv, clear);
        if (draw) ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapchain->Present(1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A bad frame must never take the whole game down.
    }
}

// ── the talisman panel (backend-agnostic; unchanged from the DX12 version) ──
std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void draw_talisman_window() {
    ImGui::SetNextWindowSize(ImVec2(440, 580), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Custom Talisman Effects", nullptr)) {
        ImGui::End();
        return;
    }

    std::lock_guard<std::mutex> lk(g_state_mutex);

    bool stacking = g_state.allow_stacking;
    if (ImGui::Checkbox("Allow stacking (ignore talisman families)", &stacking)) {
        g_state.allow_stacking = stacking;
        if (!stacking) collapse_groups_locked();
    }
    if (kModPageUrl[0] != '\0') {
        // Small corner link to the mod page (opens the default browser).
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 52.0f);
        if (ImGui::SmallButton("Nexus"))
            ShellExecuteA(nullptr, "open", kModPageUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }

    static char filter[64] = "";
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputTextWithHint("##search", "search talismans...", filter, sizeof(filter));
    ImGui::SameLine();
    if (ImGui::Button("Clear")) filter[0] = '\0';

    int on = 0;
    for (const auto& t : g_state.talismans) if (t.enabled) ++on;
    ImGui::Text("%d enabled / %d", on, static_cast<int>(g_state.talismans.size()));
    ImGui::SameLine();
    if (ImGui::Button("Save to .ini")) g_state.save_requested = true;
    ImGui::SameLine();
    if (ImGui::Button("Disable all"))
        for (auto& t : g_state.talismans) t.enabled = false;

    // Talismans currently worn on the character are drawn in this color; when
    // stacking is off they're also greyed out / non-toggleable (already active).
    const ImVec4 kEquippedCol(0.45f, 0.78f, 0.98f, 1.0f);

    ImGui::Separator();

    // Hotkey hint footer text (labels come straight from the .ini). Built up
    // front so its WRAPPED height at the current window width can be reserved.
    // The gamepad combo can be disabled entirely (toggle_gamepad_combo = none in
    // the .ini, g_open_pad_mask == 0) -- drop it from the hint when that's the case.
    std::string footer = g_state.open_key_label;
    if (g_open_pad_mask) footer += " or " + g_state.open_pad_label;
    footer += ": open/close  |  Esc or B (pad): close & save";
    if (g_state.has_mod_added) footer += "  |  LB/RB: switch tab";
    const float wrap_w = ImGui::GetContentRegionAvail().x;
    const float footer_h =
        ImGui::CalcTextSize(footer.c_str(), nullptr, false, wrap_w).y + 4.0f;

    const float controls_h = ImGui::GetFrameHeightWithSpacing() + footer_h + 4.0f;
    const float detail_h = g_state.show_descriptions
        ? ImGui::GetTextLineHeightWithSpacing() * 4.0f + 8.0f
        : 0.0f;
    const std::string needle = to_lower(filter);
    const Talisman* detail = nullptr; // row hovered by mouse OR focused by gamepad nav
    bool detail_equipped = false;

    auto render_rows = [&](bool base_tab) {
        int shown = 0;
        for (size_t i = 0; i < g_state.talismans.size(); ++i) {
            Talisman& t = g_state.talismans[i];
            if (t.is_base != base_tab) continue;
            if (!needle.empty() && to_lower(t.name).find(needle) == std::string::npos) continue;
            ++shown;

            bool equipped = false;
            for (int sp : t.sp_ids)
                if (g_state.external_active.count(sp)) { equipped = true; break; }
            const bool lock_it = equipped;

            ImGui::PushID(static_cast<int>(i));
            if (equipped) ImGui::PushStyleColor(ImGuiCol_Text, kEquippedCol);
            if (lock_it)  ImGui::BeginDisabled();

            bool v = lock_it ? true : t.enabled; // a locked/equipped row reads as active
            std::string label = t.name;
            if (equipped) {
                label += " (Already equipped)";
            }
            if (ImGui::Checkbox(label.c_str(), &v)) {
                t.enabled = v;
                if (v) apply_exclusivity_locked(i);
            }

            if (lock_it)  ImGui::EndDisabled();
            if (equipped) ImGui::PopStyleColor();

            if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) {
                detail = &t;
                detail_equipped = equipped;
            }
            ImGui::PopID();
        }
        if (shown == 0)
            ImGui::TextDisabled(base_tab
                ? "No talismans match your search."
                : "No mod-added talismans detected in this regulation.");
    };

    if (g_state.has_mod_added) {
        static bool prev_lb = false, prev_rb = false;
        const bool lb = g_pad_ok && (g_pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        const bool rb = g_pad_ok && (g_pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        int select_tab = -1; // -1 = no request; 0 = Base, 1 = Mod-Added
        if (lb && !prev_lb) select_tab = 0;
        if (rb && !prev_rb) select_tab = 1;
        prev_lb = lb;
        prev_rb = rb;

        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Base Game", nullptr,
                    select_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginChild("##list_base", ImVec2(0, -(controls_h + detail_h)),
                                  ImGuiChildFlags_NavFlattened);
                render_rows(true);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Mod-Added", nullptr,
                    select_tab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginChild("##list_mod", ImVec2(0, -(controls_h + detail_h)),
                                  ImGuiChildFlags_NavFlattened);
                render_rows(false);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    } else {
        ImGui::BeginChild("##list_base", ImVec2(0, -(controls_h + detail_h)),
                          ImGuiChildFlags_NavFlattened);
        render_rows(true);
        ImGui::EndChild();
    }

    if (g_state.show_descriptions) {
        ImGui::Separator();
        ImGui::BeginChild("##detail", ImVec2(0, detail_h - 8.0f));
        if (detail) {
            ImGui::TextColored(detail_equipped ? kEquippedCol : ImVec4(0.80f, 0.68f, 0.40f, 1.0f),
                               "%s%s", detail->name.c_str(), detail_equipped ? "  (equipped)" : "");
            ImGui::TextWrapped("%s", detail->effect.empty()
                ? "(no description yet)"
                : detail->effect.c_str());
        } else {
            ImGui::TextDisabled("Hover or select a talisman to see its effect.");
        }
        ImGui::EndChild();
    }

    ImGui::Separator();
    static const char* kSortLabels[] = { "Talisman ID", "Name (A-Z)", "In-Game Order" };
    int sort_mode = g_state.sort_mode;
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("Sort by", &sort_mode, kSortLabels, IM_ARRAYSIZE(kSortLabels))) {
        g_state.sort_mode = sort_mode;
        sort_talismans_locked();
    }
    ImGui::SameLine();
    bool show_desc = g_state.show_descriptions;
    if (ImGui::Checkbox("Show descriptions", &show_desc))
        g_state.show_descriptions = show_desc;

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", footer.c_str());
    ImGui::PopStyleColor();

    ImGui::End();
}

// ── open/close handling (polled on the overlay thread) ──
void update_menu_toggle() {
    const bool ours = foreground_is_ours();

    static bool prev_open_in = false;
    const bool key = ours && kd(static_cast<int>(g_open_vk));
    const bool combo = ours && g_pad_ok && g_open_pad_mask &&
                       (g_pad.wButtons & g_open_pad_mask) == g_open_pad_mask;
    const bool open_in = key || combo;
    if (open_in && !prev_open_in)
        g_menu_open.store(!g_menu_open.load());
    prev_open_in = open_in;

    static bool prev_esc = false, prev_padb = false;
    const bool esc = ours && kd(VK_ESCAPE);
    const bool padb = ours && g_pad_ok && (g_pad.wButtons & XINPUT_GAMEPAD_B) != 0;
    if (g_menu_open.load() && ((esc && !prev_esc) || (padb && !prev_padb)))
        g_menu_open.store(false);
    prev_esc = esc;
    prev_padb = padb;

    static bool prev_open = false;
    const bool open_now = g_menu_open.load();
    if (open_now && !prev_open) {
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else if (!open_now && prev_open) {
        ShowWindow(g_hwnd, SW_HIDE);
        std::lock_guard<std::mutex> l(g_state_mutex);
        g_state.save_requested = true; // auto-save selections on close
    }
    prev_open = open_now;
}

// ── the overlay thread ──
void overlay_thread() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlay_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kOverlayClass;
    wc.hCursor = nullptr;
    RegisterClassExW(&wc);

    const DWORD ex = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST |
                     WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    g_hwnd = CreateWindowExW(ex, kOverlayClass, L"Custom Talisman Effects overlay", WS_POPUP,
                             0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        flog("[overlay] CreateWindowExW failed; overlay disabled");
        return;
    }
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); // needed for DComp target creation

    if (!seh_init_d3d()) {
        flog("[overlay] D3D11/DComp init failed; overlay disabled");
        if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
        return;
    }

    ShowWindow(g_hwnd, SW_HIDE); // menu starts closed -> window hidden

    while (g_running.load()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        int gw = 0, gh = 0;
        cover_game_window(gw, gh);
        if (gw > 0 && gh > 0 &&
            (static_cast<UINT>(gw) != g_back_w || static_cast<UINT>(gh) != g_back_h))
            seh_resize(static_cast<UINT>(gw), static_cast<UINT>(gh));

        poll_gamepad();
        update_menu_toggle();

        if (g_menu_open.load()) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            feed_keyboard();
            feed_gamepad();
            ImGui::NewFrame();
            ImGui::GetIO().MouseDrawCursor = true; // our window has no OS cursor
            draw_talisman_window();
            ImGui::Render();
            render_frame(true);
        } else {
            render_frame(false);
            Sleep(16); // idle pacing while closed
        }
    }
}

// ── best-effort teardown (the process usually just exits) ──
void teardown() {
    if (g_d3d_inited) {
        __try { ImGui_ImplDX11_Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_context_inited) {
        __try { ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_dcomp_visual) { g_dcomp_visual->Release(); g_dcomp_visual = nullptr; }
    if (g_dcomp_target) { g_dcomp_target->Release(); g_dcomp_target = nullptr; }
    if (g_dcomp_device) { g_dcomp_device->Release(); g_dcomp_device = nullptr; }
    if (g_swapchain) { g_swapchain->Release(); g_swapchain = nullptr; }
    release_layered_targets();
    if (g_d3d_ctx) { g_d3d_ctx->Release(); g_d3d_ctx = nullptr; }
    if (g_d3d_device) { g_d3d_device->Release(); g_d3d_device = nullptr; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
}

} // namespace

void overlay::setup() {
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_open_vk = g_state.open_vk;
        g_open_pad_mask = g_state.open_pad_mask;
    }

    // Queue the input hooks. None of these touch the game's swapchain, so the
    // overlay is safe alongside frame-gen / Special K / other overlay mods. They
    // are committed by cte::hooks::apply() (called from the worker after setup()).
    if (HMODULE u32 = GetModuleHandleA("user32.dll")) {
        auto queue = [u32](const char* name, void* detour, void** tramp) {
            if (void* p = reinterpret_cast<void*>(GetProcAddress(u32, name)))
                hooks::create(p, detour, tramp);
        };
        queue("GetRawInputData", reinterpret_cast<void*>(&hkGetRawInputData),
              reinterpret_cast<void**>(&oGetRawInputData));
        queue("SetCursorPos", reinterpret_cast<void*>(&hkSetCursorPos),
              reinterpret_cast<void**>(&oSetCursorPos));
        queue("ClipCursor", reinterpret_cast<void*>(&hkClipCursor),
              reinterpret_cast<void**>(&oClipCursor));
    }

    {
        const char* xdlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        void* xfn = nullptr;
        for (const char* d : xdlls) {
            HMODULE h = GetModuleHandleA(d);
            if (h) { xfn = reinterpret_cast<void*>(GetProcAddress(h, "XInputGetState")); if (xfn) break; }
        }
        if (!xfn)
            if (HMODULE h = LoadLibraryA("xinput1_4.dll"))
                xfn = reinterpret_cast<void*>(GetProcAddress(h, "XInputGetState"));
        if (xfn) {
            pXInputGetState = reinterpret_cast<XInputGetState_t>(xfn);
            hooks::create(xfn, reinterpret_cast<void*>(&hkXInputGetState),
                          reinterpret_cast<void**>(&oXInputGetState));
        } else {
            flog("[overlay] [WARN] XInputGetState not found; gamepad open combo unavailable");
        }
    }

    // Spawn the dedicated overlay thread (window + D3D11 + DComp + ImGui + loop).
    if (g_running.exchange(true)) return; // already running
    std::thread([] {
        overlay_thread();
        teardown();
        g_running.store(false);
    }).detach();
    flog("[overlay] separate-window overlay thread spawned; input hooks queued");
}

} // namespace cte

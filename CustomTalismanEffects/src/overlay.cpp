// In-game config overlay: Dear ImGui on the game's DX12 swapchain.
//
// Standard UniversalHookX / EROverlay pattern for D3D12 (same as the user's
// ERR-MapForGoblins-DLL, trimmed to just what this mod needs):
//   - IDXGISwapChain::Present (vtable idx 8)   -> drive rendering + lazy init +
//     read the open/close hotkeys
//   - ID3D12CommandQueue::ExecuteCommandLists  -> capture the game's graphics
//     queue (not reachable from a Present-only hook)
//   - IDXGISwapChain::ResizeBuffers (idx 13)   -> tear down our RTVs so a resize
//     succeeds; recreated on the next Present
//   - user32!GetRawInputData                   -> route kbd/mouse to ImGui and
//     freeze the game's input while the menu is open (ER uses raw input)
//   - xinput!XInputGetState                    -> read the pad for the L3+R3 combo
//     and ImGui nav; zero the game's read while the menu is open
// vtable pointers are grabbed once from a throwaway device+swapchain, so there is
// no game-specific RVA/AOB to maintain.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <Xinput.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "overlay.hpp"
#include "hooks.hpp"
#include "state.hpp"
#include "log.hpp"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")

namespace cte {
namespace {

// Mod page opened by the small "Nexus" button in the overlay. Leave empty ("")
// to hide the button entirely.
constexpr const char* kModPageUrl = "https://www.nexusmods.com/eldenring/mods/10327";

using Present_t = HRESULT(WINAPI*)(IDXGISwapChain3*, UINT, UINT);
using ResizeBuffers_t = HRESULT(WINAPI*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ExecuteCommandLists_t = void(WINAPI*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

Present_t             oPresent = nullptr;
ResizeBuffers_t       oResizeBuffers = nullptr;
ExecuteCommandLists_t oExecuteCommandLists = nullptr;
GetRawInputData_t     oGetRawInputData = nullptr;
XInputGetState_t      oXInputGetState = nullptr;

// Open/close inputs, snapshotted from g_state at setup().
unsigned int   g_open_vk = 0x2D;       // VK_INSERT
unsigned short g_open_pad_mask = 0x00C0; // L3+R3

// ---- gamepad snapshot (real state read via the trampoline) ----
XINPUT_GAMEPAD g_pad{};
bool  g_pad_ok = false;
DWORD g_pad_index = 0;
std::atomic<WORD> g_pad_swallow[XUSER_MAX_COUNT]{}; // buttons held at close, suppressed until released

// ---- raw-input capture (message thread writes; render thread drains) ----
std::atomic<int>      g_raw_dx{0};
std::atomic<int>      g_raw_dy{0};
std::atomic<int>      g_raw_wheel{0};
std::atomic<uint32_t> g_raw_btn{0}; // bit0=L bit1=R bit2=M
float g_mouse_x = 0.0f, g_mouse_y = 0.0f;
bool  g_need_center = true;
bool  g_os_cursor = false;

struct KeyEv { ImGuiKey key; bool down; };
std::mutex           g_key_mtx;
std::vector<KeyEv>   g_key_events;
// Text input for ImGui widgets (the search box). The WndProc swallows WM_CHAR
// while the menu is open, so characters are synthesized from the raw keyboard
// events (ToUnicode) and fed to ImGui as InputCharacter events.
std::vector<wchar_t> g_char_events; // guarded by g_key_mtx

// ---- DX12 state ----
struct FrameContext {
    ID3D12CommandAllocator*     allocator = nullptr;
    ID3D12Resource*             render_target = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle{};
};

ID3D12Device*              g_device = nullptr;
ID3D12DescriptorHeap*      g_srv_heap = nullptr; // shader-visible, imgui font SRV
ID3D12DescriptorHeap*      g_rtv_heap = nullptr;
ID3D12GraphicsCommandList* g_command_list = nullptr;
ID3D12CommandQueue*        g_command_queue = nullptr; // captured from ExecuteCommandLists
FrameContext*              g_frames = nullptr;
UINT                       g_buffer_count = 0;

ID3D12Fence* g_fence = nullptr;
UINT64       g_fence_val = 0;
HANDLE       g_fence_event = nullptr;

HWND    g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;

std::atomic<bool> g_menu_open{false};
bool g_context_inited = false;
bool g_dx12_inited = false;

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
    c[ImGuiCol_NavHighlight] = gold; //ImVec4(0.80f, 0.68f, 0.40f, 0.80f); // Golden
    // Tabs (Base/Mod-Added bar), separators, scrollbars, resize grips and text
    // selection all default to ImGui blue -- bring them into the gold theme.
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

// ── gamepad helpers ──
constexpr SHORT PAD_ACTIVITY_DEADZONE = 12000;

bool pad_has_activity(const XINPUT_GAMEPAD& pad) {
    return pad.wButtons != 0 ||
           pad.sThumbLX > PAD_ACTIVITY_DEADZONE || pad.sThumbLX < -PAD_ACTIVITY_DEADZONE ||
           pad.sThumbLY > PAD_ACTIVITY_DEADZONE || pad.sThumbLY < -PAD_ACTIVITY_DEADZONE ||
           pad.sThumbRX > PAD_ACTIVITY_DEADZONE || pad.sThumbRX < -PAD_ACTIVITY_DEADZONE ||
           pad.sThumbRY > PAD_ACTIVITY_DEADZONE || pad.sThumbRY < -PAD_ACTIVITY_DEADZONE ||
           pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
           pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
}
bool pad_has_mask(const XINPUT_GAMEPAD& pad, uint16_t mask) {
    return mask != 0 && (pad.wButtons & mask) == mask;
}
void set_active_gamepad(DWORD idx, const XINPUT_GAMEPAD& pad) {
    g_pad = pad;
    g_pad_ok = true;
    g_pad_index = idx;
}

// Poll every XInput slot via the trampoline (real state, not the zeroed state the
// game reads) so the open combo works even when the game only reads slot 0.
void refresh_gamepad_state() {
    if (!oXInputGetState) return;
    bool have_first = false, have_active = false, have_combo = false;
    XINPUT_GAMEPAD first{}, active{}, combo{};
    DWORD fi = 0, ai = 0, ci = 0;
    for (DWORD idx = 0; idx < XUSER_MAX_COUNT; ++idx) {
        XINPUT_STATE s{};
        if (oXInputGetState(idx, &s) != ERROR_SUCCESS) continue;
        const XINPUT_GAMEPAD& pad = s.Gamepad;
        if (!have_first) { first = pad; fi = idx; have_first = true; }
        if (!have_active && pad_has_activity(pad)) { active = pad; ai = idx; have_active = true; }
        if (pad_has_mask(pad, g_open_pad_mask)) { combo = pad; ci = idx; have_combo = true; break; }
    }
    if (have_combo)       set_active_gamepad(ci, combo);
    else if (have_active) set_active_gamepad(ai, active);
    else if (have_first)  set_active_gamepad(fi, first);
    else                  g_pad_ok = false;
}

void capture_swallow_buttons() {
    if (!g_pad_ok) return;
    if (g_pad_index < XUSER_MAX_COUNT)
        g_pad_swallow[g_pad_index].store(g_pad.wButtons);
}

// ── input hooks ──
LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_menu_open.load()) {
        switch (msg) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_INPUT:
            return 0;
        case WM_SETCURSOR:
            return TRUE;
        default: break;
        }
    }
    return CallWindowProcW(g_orig_wndproc, hWnd, msg, wParam, lParam);
}

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

bool game_focused() {
    return g_hwnd != nullptr && GetForegroundWindow() == g_hwnd;
}

UINT WINAPI hkGetRawInputData(HRAWINPUT hri, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    UINT res = oGetRawInputData(hri, cmd, data, size, hdr);
    if (!g_menu_open.load() || cmd != RID_INPUT || data == nullptr || !game_focused())
        return res;

    auto* ri = reinterpret_cast<RAWINPUT*>(data);
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE& m = ri->data.mouse;
        if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            g_raw_dx.fetch_add(m.lLastX, std::memory_order_relaxed);
            g_raw_dy.fetch_add(m.lLastY, std::memory_order_relaxed);
        }
        const USHORT bf = m.usButtonFlags;
        if (bf & RI_MOUSE_LEFT_BUTTON_DOWN)   g_raw_btn.fetch_or(1u);
        if (bf & RI_MOUSE_LEFT_BUTTON_UP)     g_raw_btn.fetch_and(~1u);
        if (bf & RI_MOUSE_RIGHT_BUTTON_DOWN)  g_raw_btn.fetch_or(2u);
        if (bf & RI_MOUSE_RIGHT_BUTTON_UP)    g_raw_btn.fetch_and(~2u);
        if (bf & RI_MOUSE_MIDDLE_BUTTON_DOWN) g_raw_btn.fetch_or(4u);
        if (bf & RI_MOUSE_MIDDLE_BUTTON_UP)   g_raw_btn.fetch_and(~4u);
        if (bf & RI_MOUSE_WHEEL)
            g_raw_wheel.fetch_add(static_cast<short>(m.usButtonData), std::memory_order_relaxed);
        ri->data.mouse.lLastX = 0;
        ri->data.mouse.lLastY = 0;
        ri->data.mouse.usButtonFlags = 0;
        ri->data.mouse.usButtonData = 0;
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& kb = ri->data.keyboard;
        const bool down = (kb.Flags & RI_KEY_BREAK) == 0;
        ImGuiKey k = vk_to_imgui(kb.VKey);
        if (k != ImGuiKey_None || down) {
            std::lock_guard<std::mutex> lk(g_key_mtx);
            if (k != ImGuiKey_None) g_key_events.push_back({k, down});
            // Synthesize the character (layout-aware, shift-aware) so text
            // widgets receive typed input despite WM_CHAR being swallowed.
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
        ri->data.keyboard.Flags = 0;
        ri->data.keyboard.Message = WM_NULL;
    }
    return res;
}

DWORD WINAPI hkXInputGetState(DWORD idx, XINPUT_STATE* st) {
    DWORD r = oXInputGetState(idx, st);
    if (r == ERROR_SUCCESS && st) {
        if (!g_pad_ok || idx == g_pad_index || pad_has_activity(st->Gamepad))
            set_active_gamepad(idx, st->Gamepad);
    } else if (idx == g_pad_index) {
        g_pad_ok = false;
    }
    if (st) {
        if (g_menu_open.load()) {
            ZeroMemory(&st->Gamepad, sizeof(st->Gamepad));
        } else if (idx < XUSER_MAX_COUNT && g_pad_swallow[idx].load()) {
            WORD sw = g_pad_swallow[idx].load() & st->Gamepad.wButtons;
            g_pad_swallow[idx].store(sw);
            st->Gamepad.wButtons &= ~sw;
        }
    }
    return r;
}

// Drain raw-input atomics + the pad snapshot into ImGui (render thread).
void feed_input() {
    ImGuiIO& io = ImGui::GetIO();
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    g_os_cursor = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0;
    if (g_os_cursor) {
        POINT p{};
        if (GetCursorPos(&p) && g_hwnd) {
            ScreenToClient(g_hwnd, &p);
            g_mouse_x = static_cast<float>(p.x);
            g_mouse_y = static_cast<float>(p.y);
        }
        g_raw_dx.exchange(0, std::memory_order_relaxed);
        g_raw_dy.exchange(0, std::memory_order_relaxed);
        g_need_center = true;
    } else {
        if (g_need_center) {
            g_mouse_x = io.DisplaySize.x * 0.5f;
            g_mouse_y = io.DisplaySize.y * 0.5f;
            g_need_center = false;
        }
        g_mouse_x += static_cast<float>(g_raw_dx.exchange(0, std::memory_order_relaxed));
        g_mouse_y += static_cast<float>(g_raw_dy.exchange(0, std::memory_order_relaxed));
        if (g_mouse_x < 0.0f) g_mouse_x = 0.0f;
        if (g_mouse_y < 0.0f) g_mouse_y = 0.0f;
        if (io.DisplaySize.x > 0 && g_mouse_x > io.DisplaySize.x) g_mouse_x = io.DisplaySize.x;
        if (io.DisplaySize.y > 0 && g_mouse_y > io.DisplaySize.y) g_mouse_y = io.DisplaySize.y;
    }
    io.AddMousePosEvent(g_mouse_x, g_mouse_y);
    const uint32_t b = g_raw_btn.load();
    io.AddMouseButtonEvent(0, (b & 1u) != 0);
    io.AddMouseButtonEvent(1, (b & 2u) != 0);
    io.AddMouseButtonEvent(2, (b & 4u) != 0);
    const int w = g_raw_wheel.exchange(0, std::memory_order_relaxed);
    if (w != 0)
        io.AddMouseWheelEvent(0.0f, static_cast<float>(w) / static_cast<float>(WHEEL_DELTA));
    {
        std::lock_guard<std::mutex> lk(g_key_mtx);
        for (const auto& e : g_key_events) io.AddKeyEvent(e.key, e.down);
        g_key_events.clear();
        for (wchar_t c : g_char_events) io.AddInputCharacterUTF16(static_cast<ImWchar16>(c));
        g_char_events.clear();
    }
    if (g_pad_ok) {
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
}

// ── DX12 lifecycle ──
void teardown_dx12() {
    if (!g_dx12_inited) return;
    ImGui_ImplDX12_Shutdown();
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_command_list) { g_command_list->Release(); g_command_list = nullptr; }
    if (g_frames) {
        for (UINT i = 0; i < g_buffer_count; ++i) {
            if (g_frames[i].render_target) g_frames[i].render_target->Release();
            if (g_frames[i].allocator) g_frames[i].allocator->Release();
        }
        delete[] g_frames;
        g_frames = nullptr;
    }
    if (g_rtv_heap) { g_rtv_heap->Release(); g_rtv_heap = nullptr; }
    if (g_srv_heap) { g_srv_heap->Release(); g_srv_heap = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_dx12_inited = false;
}

bool init_dx12(IDXGISwapChain3* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc))) return false;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device)))) return false;
    g_hwnd = desc.OutputWindow;
    g_buffer_count = desc.BufferCount;

    D3D12_DESCRIPTOR_HEAP_DESC srv{};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv.NumDescriptors = 1; // imgui font only
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&g_srv_heap)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = g_buffer_count;
    rtv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_rtv_heap)))) return false;

    const UINT rtv_size =
        g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g_frames = new FrameContext[g_buffer_count];
    for (UINT i = 0; i < g_buffer_count; ++i) {
        g_frames[i] = FrameContext{};
        g_frames[i].rtv_handle = h;
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g_frames[i].allocator))))
            return false;
        ID3D12Resource* back = nullptr;
        if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&back))) && back) {
            g_device->CreateRenderTargetView(back, nullptr, h);
            g_frames[i].render_target = back;
        }
        h.ptr += rtv_size;
    }

    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_frames[0].allocator, nullptr,
                                           IID_PPV_ARGS(&g_command_list))))
        return false;
    g_command_list->Close();

    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (!g_fence_event) g_fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_fence_val = 0;

    if (!g_context_inited) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; // don't drop an imgui.ini next to the game
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
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
        g_orig_wndproc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));
        g_context_inited = true;
    }

    ImGui_ImplDX12_Init(g_device, g_buffer_count, desc.BufferDesc.Format, g_srv_heap,
                        g_srv_heap->GetCPUDescriptorHandleForHeapStart(),
                        g_srv_heap->GetGPUDescriptorHandleForHeapStart());
    g_dx12_inited = true;
    flog("[overlay] DX12 backend ready (%u buffers, %ux%u)", g_buffer_count,
         desc.BufferDesc.Width, desc.BufferDesc.Height);
    return true;
}

static bool seh_init_dx12(IDXGISwapChain3* sc) {
    __try { return init_dx12(sc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void wait_gpu() {
    if (!g_fence || !g_command_queue) return;
    const UINT64 v = ++g_fence_val;
    if (FAILED(g_command_queue->Signal(g_fence, v))) return;
    if (g_fence->GetCompletedValue() < v && g_fence_event) {
        g_fence->SetEventOnCompletion(v, g_fence_event);
        WaitForSingleObject(g_fence_event, 1000);
    }
}

void submit_frame(IDXGISwapChain3* sc) {
    __try {
        const UINT idx = sc->GetCurrentBackBufferIndex();
        if (idx >= g_buffer_count) return;
        FrameContext& f = g_frames[idx];
        if (!f.allocator || !f.render_target) return;
        f.allocator->Reset();

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = f.render_target;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

        g_command_list->Reset(f.allocator, nullptr);
        g_command_list->ResourceBarrier(1, &b);
        g_command_list->OMSetRenderTargets(1, &f.rtv_handle, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {g_srv_heap};
        g_command_list->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_command_list);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_command_list->ResourceBarrier(1, &b);
        g_command_list->Close();
        ID3D12CommandList* lists[] = {g_command_list};
        g_command_queue->ExecuteCommandLists(1, lists);
        wait_gpu();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A bad frame must never take the whole game down.
    }
}

// ── the talisman panel ──
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
    // ImGui::TextColored(kEquippedCol, "Blue = already equipped on your character"); // removed the text line. "equipped" is now shown on the item name.

    ImGui::Separator();

    // Hotkey hint footer text (labels come straight from the .ini). Built up
    // front so its WRAPPED height at the current window width can be reserved
    // -- on narrow windows it flows to 2+ lines instead of clipping.
    std::string footer = g_state.open_key_label + " or " + g_state.open_pad_label +
                         ": open/close  |  Esc or B (pad): close & save";
    if (g_state.has_mod_added) footer += "  |  LB/RB: switch tab";
    const float wrap_w = ImGui::GetContentRegionAvail().x;
    const float footer_h =
        ImGui::CalcTextSize(footer.c_str(), nullptr, false, wrap_w).y + 4.0f;

    // Reserve room at the bottom for the sort/description controls + the
    // hotkey hint footer, plus a fixed-size (~4 lines) effect detail pane when
    // descriptions are enabled. All constant heights per frame, so they never
    // shift the list or each other around as the hovered talisman's text
    // changes.
    const float controls_h = ImGui::GetFrameHeightWithSpacing() + footer_h + 4.0f;
    const float detail_h = g_state.show_descriptions
        ? ImGui::GetTextLineHeightWithSpacing() * 4.0f + 8.0f
        : 0.0f;
    const std::string needle = to_lower(filter);
    const Talisman* detail = nullptr; // row hovered by mouse OR focused by gamepad nav
    bool detail_equipped = false;

    // Render the search-filtered rows for one tab: base_tab==true shows base-game
    // talismans, false shows mod-added ones (ids not in the baked table). Assumes
    // g_state_mutex is held (it is). `i` is the index into g_state.talismans, kept
    // as the ImGui id so it stays unique/stable across both tabs.
    auto render_rows = [&](bool base_tab) {
        int shown = 0;
        for (size_t i = 0; i < g_state.talismans.size(); ++i) {
            Talisman& t = g_state.talismans[i];
            if (t.is_base != base_tab) continue;
            if (!needle.empty() && to_lower(t.name).find(needle) == std::string::npos) continue;
            ++shown;

            // Equipped = one of this talisman's effects is active on the player from
            // a source other than us (the real talisman is worn). Lock it -- even
            // when stacking is on -- since re-applying our copy would be redundant.
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

            // Hover (mouse) OR gamepad focus -> drive the shared detail pane below.
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

    // Only split into Base/Mod-Added tabs when an overhaul actually added
    // talismans; on the vanilla game there's a single plain list (no tabs).
    if (g_state.has_mod_added) {
        // Controller tab switching: ImGui's nav doesn't drive tab bars, so LB/RB
        // presses (edge-triggered off the live pad snapshot) select explicitly.
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

    // Detail pane: effect of whichever row the mouse/controller is on. Fixed-height
    // child (matching detail_h above) so long descriptions scroll instead of
    // pushing the controls below it around.
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

    // Bottom controls: sort order + description-pane toggle.
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

    // Hotkey hint footer (string built above; wraps on narrow windows).
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", footer.c_str());
    ImGui::PopStyleColor();

    ImGui::End();
}

void render(IDXGISwapChain3* sc) {
    const bool focused = game_focused();
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (focused) {
        feed_input();
    } else {
        g_raw_btn.store(0);
        g_raw_dx.exchange(0, std::memory_order_relaxed);
        g_raw_dy.exchange(0, std::memory_order_relaxed);
        g_need_center = true;
    }
    ImGui::NewFrame();
    ImGui::GetIO().MouseDrawCursor = focused && !g_os_cursor;
    draw_talisman_window();
    ImGui::Render();
    submit_frame(sc);
}

// ── swapchain hooks ──
HRESULT WINAPI hkPresent(IDXGISwapChain3* sc, UINT sync, UINT flags) {
    const bool focused = game_focused();
    if (focused) refresh_gamepad_state();

    static bool prev_open_in = false;
    const bool key = focused && (GetAsyncKeyState(static_cast<int>(g_open_vk)) & 0x8000) != 0;
    const bool combo = focused && g_pad_ok && g_open_pad_mask &&
                       (g_pad.wButtons & g_open_pad_mask) == g_open_pad_mask;
    const bool open_in = key || combo;
    if (open_in && !prev_open_in)
        g_menu_open.store(!g_menu_open.load());
    prev_open_in = open_in;

    // Close-only shortcuts: ESC (keyboard) or B (gamepad).
    static bool prev_esc = false, prev_padb = false;
    const bool esc = focused && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool padb = focused && g_pad_ok && (g_pad.wButtons & XINPUT_GAMEPAD_B) != 0;
    if (g_menu_open.load() && ((esc && !prev_esc) || (padb && !prev_padb)))
        g_menu_open.store(false);
    prev_esc = esc;
    prev_padb = padb;

    // Auto-save to the .ini on close.
    static bool prev_open = false;
    const bool open_now = g_menu_open.load();
    if (open_now && !prev_open) {
        g_need_center = true;
    } else if (!open_now && prev_open) {
        capture_swallow_buttons();
        std::lock_guard<std::mutex> l(g_state_mutex);
        g_state.save_requested = true;
    }
    prev_open = open_now;

    if (!g_dx12_inited) {
        if (!seh_init_dx12(sc))
            return oPresent(sc, sync, flags);
    }
    if (g_menu_open.load() && g_command_queue)
        render(sc);

    return oPresent(sc, sync, flags);
}

static void seh_resize_teardown() {
    __try {
        wait_gpu();
        teardown_dx12();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dx12_inited = false;
    }
}

HRESULT WINAPI hkResizeBuffers(IDXGISwapChain3* sc, UINT bc, UINT w, UINT h,
                               DXGI_FORMAT fmt, UINT flags) {
    seh_resize_teardown();
    return oResizeBuffers(sc, bc, w, h, fmt, flags);
}

void WINAPI hkExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l) {
    if (!g_command_queue && q) {
        const D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            g_command_queue = q;
    }
    oExecuteCommandLists(q, n, l);
}

// Throwaway device + swapchain just to read the vtable function pointers.
bool capture_vtables(void*& present, void*& resize, void*& execcl) {
    HMODULE hd3d12 = GetModuleHandleA("d3d12.dll");
    if (!hd3d12) hd3d12 = LoadLibraryA("d3d12.dll");
    HMODULE hdxgi = GetModuleHandleA("dxgi.dll");
    if (!hdxgi) hdxgi = LoadLibraryA("dxgi.dll");
    if (!hd3d12 || !hdxgi) return false;

    auto pD3D12CreateDevice =
        reinterpret_cast<decltype(&D3D12CreateDevice)>(GetProcAddress(hd3d12, "D3D12CreateDevice"));
    auto pCreateDXGIFactory1 =
        reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(hdxgi, "CreateDXGIFactory1"));
    if (!pD3D12CreateDevice || !pCreateDXGIFactory1) return false;

    ID3D12Device* dev = nullptr;
    if (FAILED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) || !dev)
        return false;

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || !queue) {
        dev->Release();
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CTE_OverlayDummy";
    RegisterClassExW(&wc);
    HWND dummy = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                 nullptr, nullptr, wc.hInstance, nullptr);

    IDXGIFactory2* factory = nullptr;
    bool ok = false;
    if (dummy && SUCCEEDED(pCreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.BufferCount = 2;
        scd.Width = 100;
        scd.Height = 100;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.SampleDesc.Count = 1;
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue, dummy, &scd, nullptr, nullptr, &sc1)) && sc1) {
            void** sc_vtbl = *reinterpret_cast<void***>(sc1);
            void** cq_vtbl = *reinterpret_cast<void***>(queue);
            present = sc_vtbl[8];   // IDXGISwapChain::Present
            resize = sc_vtbl[13];   // IDXGISwapChain::ResizeBuffers
            execcl = cq_vtbl[10];   // ID3D12CommandQueue::ExecuteCommandLists
            ok = present && resize && execcl;
            sc1->Release();
        }
        factory->Release();
    }
    if (dummy) DestroyWindow(dummy);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    queue->Release();
    dev->Release();
    return ok;
}

} // namespace

void overlay::setup() {
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_open_vk = g_state.open_vk;
        g_open_pad_mask = g_state.open_pad_mask;
    }

    void* present = nullptr; void* resize = nullptr; void* execcl = nullptr;
    if (!capture_vtables(present, resize, execcl)) {
        flog("[overlay] failed to capture DX12 vtables; overlay disabled");
        return;
    }
    flog("[overlay] vtables captured: Present=%p Resize=%p ExecCL=%p", present, resize, execcl);

    hooks::create(present, reinterpret_cast<void*>(&hkPresent),
                  reinterpret_cast<void**>(&oPresent));
    hooks::create(resize, reinterpret_cast<void*>(&hkResizeBuffers),
                  reinterpret_cast<void**>(&oResizeBuffers));
    hooks::create(execcl, reinterpret_cast<void*>(&hkExecuteCommandLists),
                  reinterpret_cast<void**>(&oExecuteCommandLists));

    if (HMODULE u32 = GetModuleHandleA("user32.dll")) {
        if (void* grid = reinterpret_cast<void*>(GetProcAddress(u32, "GetRawInputData"))) {
            hooks::create(grid, reinterpret_cast<void*>(&hkGetRawInputData),
                          reinterpret_cast<void**>(&oGetRawInputData));
            flog("[overlay] GetRawInputData hook queued (menu input routing)");
        } else {
            flog("[overlay] [WARN] GetRawInputData not found; menu input limited");
        }
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
            hooks::create(xfn, reinterpret_cast<void*>(&hkXInputGetState),
                          reinterpret_cast<void**>(&oXInputGetState));
            flog("[overlay] XInputGetState hook queued (gamepad combo + nav)");
        } else {
            flog("[overlay] [WARN] XInputGetState not found; gamepad open combo unavailable");
        }
    }
    flog("[overlay] handlers queued");
}

} // namespace cte

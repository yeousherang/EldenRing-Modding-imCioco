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
//   - setup() resolves + detours XInputGetState (pad neutralization, see the
//     input section) and spawns a detached overlay thread that creates the
//     window + D3D11 + DComp + ImGui DX11 backend and runs the render loop.
//     setup() returns fast; the thread owns all overlay state.
//   - Window: WS_POPUP + WS_EX_NOREDIRECTIONBITMAP|TOPMOST|TOOLWINDOW|NOACTIVATE;
//     transparency via DirectComposition (premultiplied alpha). Kept SHOWN for
//     the mod's lifetime and NEVER activated: hiding it uncovers the game (a DWM
//     present-mode transition), and taking focus deactivates the game -- which
//     makes frame-gen mods (erdGameTools / Smooth Motion) tear down and re-init
//     for several seconds, the classic ER "alt-tab freeze". Both open and close
//     must change NOTHING the game's presentation or activation state can see.
//     Closed = one fully-transparent frame + WS_EX_TRANSPARENT (click-through).
//   - Each iteration the window is moved to exactly cover the game's client area.
//   - Open/close (default Insert / L3+R3) + Esc/B are polled with GetAsyncKeyState.
//   - While the menu is open, keyboard + mouse are captured focus-free by
//     re-targeting the process's raw-input registration at our window (which
//     also starves the game's raw-input reader, so menu input never leaks into
//     gameplay); the mouse is a virtual cursor integrated from raw deltas; the
//     game's registration is restored on close. Gamepad: XInput + DirectInput8
//     detours hand the game neutral pad input while the menu is open.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <Xinput.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
#pragma comment(lib, "dxguid.lib") // IID_IDirectInput8W/A, GUID_SysKeyboard

// From imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace cte {
namespace {

// Mod page opened by the small "Nexus" button in the overlay. Leave empty ("")
// to hide the button entirely.
constexpr const char* kModPageUrl = "https://www.nexusmods.com/eldenring/mods/10327";

const wchar_t* kOverlayClass = L"CTE_OverlayWindow";

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t pXInputGetState = nullptr; // real pad reader (MinHook trampoline,
                                            // or the direct proc if hooking failed)

// Open/close inputs, snapshotted from g_state at setup().
unsigned int   g_open_vk = 0x2D;         // VK_INSERT
unsigned short g_open_pad_mask = 0x00C0; // L3+R3
bool g_open_pad_is_hold = false;         // true if the combo needs a hold, not a tap
constexpr long long kPadHoldMs = 1000;   // hardcoded hold duration for HOLD_ combos
// Escape hatch ([overlay] focus_input = 1): restore the old focus-taking input
// mode for setups where the focus-free capture misbehaves. Costs the alt-tab
// freeze on close when frame-gen mods are active. Snapshotted at setup().
bool g_focus_input = false;

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

// Keyboard + mouse now arrive via our window's raw-input capture (re-targeted
// while the menu is open); raw keys are forwarded to the ImGui Win32 backend and
// the mouse is a virtual cursor, so no separate capture buffers are needed.

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

// Build the ImGui font atlas: a Latin base font for the UI chrome, with EVERY
// available CJK font merged on top so talisman names in any language render
// correctly -- rather than picking a single "first found" CJK font and using
// it for all CJK text regardless of script (the earlier approach: Malgun
// Gothic, a Korean-only font present on nearly every Windows install, got
// found first and was used for Simplified Chinese too, which it doesn't
// cover -> tofu/mojibake for CN players).
//
// ImGui's glyph lookup is last-added-wins for codepoints that appear in more
// than one merged range (CJK Unified Ideographs are shared by the SC/TC/JP
// ranges, each with its own font-specific stroke shapes). We use the
// player's Windows UI language to decide which font is merged LAST -- i.e.
// whose glyph shapes win any overlapping codepoints -- while still merging
// every other script first as a fallback, so text in a non-primary language
// (a renamed talisman from a translation mod, etc.) still renders instead of
// being skipped.
void load_fonts(ImGuiIO& io) {
    // Cyrillic (Russian, Bulgarian, Ukrainian, Serbian, etc.) glyphs live in the
    // same font FILES as Latin, but AddFontFromFileTTF's default range is Basic
    // Latin + Latin-1 only (0x0020-0x00FF) -- Cyrillic (0x0400+) is never pulled
    // into the atlas unless explicitly requested, even though segoeui/arial/tahoma
    // all contain those glyphs. GetGlyphRangesCyrillic() is a superset of the
    // default Latin range, so this is a straight upgrade with no separate font.
    const char* latin_fonts[] = {"C:\\Windows\\Fonts\\segoeui.ttf",
                                 "C:\\Windows\\Fonts\\arial.ttf",
                                 "C:\\Windows\\Fonts\\tahoma.ttf"};
    ImFont* base = nullptr;
    for (const char* fp : latin_fonts) {
        if (GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES) {
            base = io.Fonts->AddFontFromFileTTF(fp, 18.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
            if (base) break;
        }
    }
    if (!base) base = io.Fonts->AddFontDefault();

    struct CjkFont { const char* lang; const char* path; const ImWchar* ranges; };
    const CjkFont cjk_fonts[] = {
        {"ja", "C:\\Windows\\Fonts\\meiryo.ttc",   io.Fonts->GetGlyphRangesJapanese()},
        {"ja", "C:\\Windows\\Fonts\\msgothic.ttc", io.Fonts->GetGlyphRangesJapanese()},
        {"ko", "C:\\Windows\\Fonts\\malgun.ttf",   io.Fonts->GetGlyphRangesKorean()},
        {"zh", "C:\\Windows\\Fonts\\msyh.ttc",     io.Fonts->GetGlyphRangesChineseFull()},
        {"zh", "C:\\Windows\\Fonts\\msjh.ttc",     io.Fonts->GetGlyphRangesChineseFull()},
    };

    // Player's Windows UI language, so overlapping CJK codepoints render with
    // the glyph shapes that language's readers actually expect (the game's
    // own text language usually follows it).
    const char* preferred = nullptr;
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_JAPANESE: preferred = "ja"; break;
        case LANG_KOREAN:   preferred = "ko"; break;
        case LANG_CHINESE:  preferred = "zh"; break;
        default: break;
    }

    ImFontConfig merge_cfg;
    merge_cfg.MergeMode = true;
    for (const auto& f : cjk_fonts) // fallback scripts first...
        if ((!preferred || std::strcmp(f.lang, preferred) != 0) &&
            GetFileAttributesA(f.path) != INVALID_FILE_ATTRIBUTES)
            io.Fonts->AddFontFromFileTTF(f.path, 18.0f, &merge_cfg, f.ranges);
    for (const auto& f : cjk_fonts) // ...preferred script last, so it wins ties
        if (preferred && std::strcmp(f.lang, preferred) == 0 &&
            GetFileAttributesA(f.path) != INVALID_FILE_ATTRIBUTES)
            io.Fonts->AddFontFromFileTTF(f.path, 18.0f, &merge_cfg, f.ranges);
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

// ── input (focus-free) ──
// The game must NEVER lose foreground activation: frame-generation mods
// (erdGameTools / NVIDIA Smooth Motion) tear their pipeline down and re-init
// it whenever the game window is deactivated OR reactivated -- a multi-second
// frozen-frame stall (a plain alt-tab reproduces it with the mod unloaded).
// The overlay window is therefore permanently WS_EX_NOACTIVATE and we never
// call SetForegroundWindow in either direction. While the menu is open, input
// is captured WITHOUT focus:
//   - Keyboard + mouse: the process's raw-input registration is re-targeted at
//     our window (RegisterRawInputDevices is per-process, per-usage, so one
//     call both routes WM_INPUT to us and SILENCES the game's raw-input reader
//     -- ER reads kb/mouse via raw input, so menu input can't leak into
//     gameplay). The game's exact registration is restored on close.
//   - Mouse cursor: a VIRTUAL cursor integrated from raw deltas. The game,
//     still focused, keeps hiding/clipping/warping the OS cursor; raw deltas
//     don't care. ImGui draws it as a software cursor.
//   - Gamepad: the still-focused game would act on pad presses, so the pad
//     APIs are MinHook-detoured to hand the GAME neutral input while the menu
//     is open: XInputGetState + XInputGetStateEx(#100), and -- the one ER
//     actually uses -- IDirectInputDevice8::GetDeviceState/GetDeviceData via
//     the shared dinput8 vtable. Our own polling reads the real state via the
//     XInput trampoline.
// [overlay] focus_input = 1 restores the previous focus-taking mode (see
// g_focus_input above) as an escape hatch.

// Read the real pad (through the trampoline) for ImGui nav + the open combo.
void poll_gamepad() {
    g_pad_ok = false;
    if (!pXInputGetState) return;
    for (DWORD idx = 0; idx < XUSER_MAX_COUNT; ++idx) {
        XINPUT_STATE state{};
        if (pXInputGetState(idx, &state) == ERROR_SUCCESS) {
            g_pad = state.Gamepad;
            g_pad_ok = true;
            break;
        }
    }
}

// ONLY used by the focus_input = 1 escape hatch. Robustly move foreground
// focus to `hwnd`; same-process foreground switches still need the
// AttachThreadInput dance or SetForegroundWindow is silently refused by the
// foreground lock. The default mode never changes activation at all.
void force_foreground(HWND hwnd) {
    if (!hwnd) return;
    const HWND fg = GetForegroundWindow();
    const DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD our_tid = GetCurrentThreadId();
    if (fg_tid && fg_tid != our_tid) AttachThreadInput(fg_tid, our_tid, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    SetActiveWindow(hwnd);
    if (fg_tid && fg_tid != our_tid) AttachThreadInput(fg_tid, our_tid, FALSE);
}

// Feed the polled gamepad to ImGui nav (mouse + keyboard come in elsewhere).
void feed_gamepad() {
    if (!g_pad_ok) return;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    // Only strip the toggle-combo buttons (e.g. A+UP) from nav while that FULL
    // combo is actually held down together -- that's the one instant we must
    // stop it from also acting as a nav press. Stripping the bits unconditionally
    // (the old behavior) permanently disabled plain A (confirm) and plain DPad-Up
    // (nav) for as long as the menu stayed open, since both happen to be part of
    // the default open combo.
    const bool combo_held = g_open_pad_mask &&
        (g_pad.wButtons & g_open_pad_mask) == g_open_pad_mask;
    const WORD nbt = combo_held ? (g_pad.wButtons & ~g_open_pad_mask) : g_pad.wButtons;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    (nbt & XINPUT_GAMEPAD_DPAD_UP) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  (nbt & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  (nbt & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (nbt & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  (nbt & XINPUT_GAMEPAD_A) != 0); // A: confirm/open
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (nbt & XINPUT_GAMEPAD_B) != 0);
    // Y is intentionally NOT mapped to GamepadFaceUp: ImGui treats FaceUp as a
    // second generic "activate" input alongside A, so leaving it mapped made Y
    // toggle checkboxes too. Only A should confirm.
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

// ── gamepad neutralization: XInput + DirectInput8 detours ──
// The game reads a NEUTRAL pad while the menu is open. XInput alone is NOT
// enough: with XInputGetState detoured ER still saw every pad press -- ER polls
// controllers through DirectInput8. So we detour BOTH: XInputGetState and
// XInputGetStateEx (ordinal 100) in every loaded xinput module, plus
// IDirectInputDevice8::GetDeviceState/GetDeviceData via their shared vtable.
// Do not "simplify" the dinput hooks away -- they are the ones that matter.

// Up to 3 xinput modules x {named export, ordinal 100}.
XInputGetState_t g_xi_real[6] = {}; // MinHook trampolines, filled by hook_xinput()

template <int I>
DWORD WINAPI xi_detour(DWORD idx, XINPUT_STATE* state) {
    const DWORD r = g_xi_real[I](idx, state);
    if (r == ERROR_SUCCESS && state && !g_focus_input &&
        g_menu_open.load(std::memory_order_relaxed))
        // Zero buttons/sticks/triggers; the packet number is left intact so
        // the game's "controller connected" logic never blinks.
        std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    return r;
}

// ── DirectInput8: neutralize GetDeviceState / GetDeviceData ──
// Every device instance dinput8.dll hands out shares its vtable, so hooking
// through our own throwaway keyboard device intercepts the game's controller
// devices too. The A and W interfaces can carry distinct vtables -- both are
// resolved and hooked (deduped by address).
using DIGetDeviceState_t = HRESULT(WINAPI*)(IDirectInputDevice8W*, DWORD, LPVOID);
using DIGetDeviceData_t  = HRESULT(WINAPI*)(IDirectInputDevice8W*, DWORD,
                                            LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
DIGetDeviceState_t g_di_state_real[2] = {};
DIGetDeviceData_t  g_di_data_real[2]  = {};
std::atomic<bool>  g_dinput_hooked{false};

bool pad_blocking() {
    return !g_focus_input && g_menu_open.load(std::memory_order_relaxed);
}

// Neutral joystick axes are NOT zero: the game configures each axis range
// (DIPROP_RANGE) and neutral is its midpoint -- memset(0) would slam a
// 0..65535-range stick into the top-left corner. Query once per device.
// DIJOYSTATE(2) starts with 8 contiguous LONG axes: lX..lRz, rglSlider[0..1].
struct JoyNeutral { LONG axis[8]; };
std::mutex g_di_mutex;
std::unordered_map<void*, JoyNeutral> g_joy_neutral;
std::unordered_map<void*, std::vector<uint8_t>> g_joy_frozen; // custom formats

JoyNeutral joy_neutral_for(IDirectInputDevice8W* dev) {
    {
        std::lock_guard<std::mutex> lk(g_di_mutex);
        auto it = g_joy_neutral.find(dev);
        if (it != g_joy_neutral.end()) return it->second;
    }
    JoyNeutral n{};
    for (int a = 0; a < 8; ++a) {
        DIPROPRANGE pr{};
        pr.diph.dwSize = sizeof(DIPROPRANGE);
        pr.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        pr.diph.dwObj = static_cast<DWORD>(a) * sizeof(LONG); // DIJOFS_X..
        pr.diph.dwHow = DIPH_BYOFFSET;
        // GetProperty (vtable slot 5) is unhooked -- no recursion.
        n.axis[a] = SUCCEEDED(dev->GetProperty(DIPROP_RANGE, &pr.diph))
                        ? (pr.lMin + pr.lMax) / 2 : 0;
    }
    std::lock_guard<std::mutex> lk(g_di_mutex);
    g_joy_neutral.emplace(dev, n);
    return n;
}

void di_neutralize_joy(IDirectInputDevice8W* dev, DWORD cb, void* data) {
    const JoyNeutral n = joy_neutral_for(dev);
    if (cb == sizeof(DIJOYSTATE)) {
        auto* js = static_cast<DIJOYSTATE*>(data);
        std::memcpy(&js->lX, n.axis, sizeof(n.axis));
        for (auto& pov : js->rgdwPOV) pov = 0xFFFFFFFFu; // centered
        std::memset(js->rgbButtons, 0, sizeof(js->rgbButtons));
    } else { // DIJOYSTATE2
        auto* js = static_cast<DIJOYSTATE2*>(data);
        std::memcpy(&js->lX, n.axis, sizeof(n.axis));
        for (auto& pov : js->rgdwPOV) pov = 0xFFFFFFFFu;
        std::memset(js->rgbButtons, 0, sizeof(js->rgbButtons));
        // The trailing velocity/acceleration/force blocks are deltas; zero is
        // genuinely neutral for them.
        std::memset(&js->lVX, 0,
                    static_cast<size_t>(reinterpret_cast<char*>(js + 1) -
                                        reinterpret_cast<char*>(&js->lVX)));
    }
}

template <int I>
HRESULT WINAPI di_state_detour(IDirectInputDevice8W* dev, DWORD cb, LPVOID data) {
    const HRESULT hr = g_di_state_real[I](dev, cb, data);
    if (hr != DI_OK || !data || !pad_blocking()) return hr;
    if (cb == sizeof(DIJOYSTATE) || cb == sizeof(DIJOYSTATE2)) {
        di_neutralize_joy(dev, cb, data);
    } else if (cb == 256 /* keyboard */ || cb == sizeof(DIMOUSESTATE) ||
               cb == sizeof(DIMOUSESTATE2)) {
        std::memset(data, 0, cb); // keys/buttons released, zero deltas
    } else {
        // Custom data format (layout unknown): hold the first blocked read
        // frozen -- stale-but-plausible beats zeroing unknown axes. The
        // snapshots are cleared on each menu open.
        std::lock_guard<std::mutex> lk(g_di_mutex);
        auto& snap = g_joy_frozen[dev];
        if (snap.size() != cb)
            snap.assign(static_cast<uint8_t*>(data),
                        static_cast<uint8_t*>(data) + cb);
        else
            std::memcpy(data, snap.data(), cb);
    }
    return hr;
}

template <int I>
HRESULT WINAPI di_data_detour(IDirectInputDevice8W* dev, DWORD cb_obj,
                              LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdw_inout,
                              DWORD flags) {
    const HRESULT hr = g_di_data_real[I](dev, cb_obj, rgdod, pdw_inout, flags);
    if (SUCCEEDED(hr) && pdw_inout && pad_blocking())
        *pdw_inout = 0; // "no buffered events" -- safe for every device type
    return hr;
}

// Install the dinput8 vtable hooks. Called from setup(); retried on the first
// menu open in case dinput8.dll loads after us. Idempotent.
void hook_dinput8() {
    if (g_dinput_hooked.load()) return;
    HMODULE dim = GetModuleHandleA("dinput8.dll");
    if (!dim) return; // not loaded (yet); caller retries
    using DI8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const auto create =
        reinterpret_cast<DI8Create_t>(GetProcAddress(dim, "DirectInput8Create"));
    if (!create) return;

    void* state_fns[2] = {};
    void* data_fns[2] = {};
    int found = 0;
    const IID* iids[2] = {&IID_IDirectInput8W, &IID_IDirectInput8A};
    for (const IID* iid : iids) {
        void* dip = nullptr;
        if (FAILED(create(g_hinst, DIRECTINPUT_VERSION, *iid, &dip, nullptr)) || !dip)
            continue;
        // A and W share the method layout of the slots we touch; type as W.
        auto* di = static_cast<IDirectInput8W*>(dip);
        IDirectInputDevice8W* dev = nullptr;
        if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) && dev) {
            void** vtbl = *reinterpret_cast<void***>(dev);
            void* st = vtbl[9];  // GetDeviceState
            void* da = vtbl[10]; // GetDeviceData
            bool dup = false;
            for (int i = 0; i < found; ++i)
                if (state_fns[i] == st) dup = true;
            if (!dup && found < 2) {
                state_fns[found] = st;
                data_fns[found] = da;
                ++found;
            }
            dev->Release();
        }
        di->Release(); // vtable code stays resident in dinput8.dll
    }
    if (!found) {
        flog("[overlay] [WARN] dinput8 vtable resolve failed; pad may leak while menu open");
        return;
    }

    const DIGetDeviceState_t sdet[2] = {&di_state_detour<0>, &di_state_detour<1>};
    const DIGetDeviceData_t  ddet[2] = {&di_data_detour<0>, &di_data_detour<1>};
    bool queued = false;
    for (int i = 0; i < found; ++i) {
        if (hooks::create(state_fns[i], reinterpret_cast<void*>(sdet[i]),
                          reinterpret_cast<void**>(&g_di_state_real[i])))
            queued = true;
        if (hooks::create(data_fns[i], reinterpret_cast<void*>(ddet[i]),
                          reinterpret_cast<void**>(&g_di_data_real[i])))
            queued = true;
    }
    if (queued && hooks::apply()) {
        g_dinput_hooked.store(true);
        flog("[overlay] dinput8 GetDeviceState/GetDeviceData detoured (%d vtable%s)",
             found, found > 1 ? "s" : "");
    } else {
        flog("[overlay] [WARN] dinput8 hook failed; pad may leak while menu open");
    }
}

// Detour XInputGetState + XInputGetStateEx(#100) in every currently-loaded
// xinput module. Idempotent; called on EVERY menu open, never at setup().
// Hooking at first menu open guarantees (a) any lazily loaded xinput modules
// exist by then and (b) our detour is patched in LAST, i.e. outermost --
// anything that hooked the same export earlier (Steam Input's emulation layer
// etc.) runs INSIDE us, so our zeroing is what the game finally receives.
void* g_xi_seen[6] = {};
int   g_xi_slot = 0;
bool  g_mh_ok = false; // hooks::init() result, set in setup()

void hook_xinput() {
    if (!g_mh_ok || g_xi_slot >= 6) return;
    const char* xdlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    using Detour_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    const Detour_t detours[6] = {&xi_detour<0>, &xi_detour<1>, &xi_detour<2>,
                                 &xi_detour<3>, &xi_detour<4>, &xi_detour<5>};
    bool queued = false;
    for (const char* d : xdlls) {
        HMODULE h = GetModuleHandleA(d);
        if (!h) continue;
        // Named export + ordinal 100 (XInputGetStateEx: same signature, adds
        // the Guide button) -- a game reading Ex bypasses the named export.
        const char* names[2] = {"XInputGetState",
                                reinterpret_cast<const char*>(MAKEINTRESOURCEA(100))};
        for (int e = 0; e < 2; ++e) {
            void* fn = reinterpret_cast<void*>(GetProcAddress(h, names[e]));
            if (!fn) continue;
            bool dup = false; // 9_1_0 can forward to 1_4's export
            for (int i = 0; i < g_xi_slot; ++i)
                if (g_xi_seen[i] == fn) dup = true;
            if (dup || g_xi_slot >= 6) continue;
            if (hooks::create(fn, reinterpret_cast<void*>(detours[g_xi_slot]),
                              reinterpret_cast<void**>(&g_xi_real[g_xi_slot]))) {
                g_xi_seen[g_xi_slot] = fn;
                // If our own polling pointed at this raw proc, upgrade it to
                // the trampoline -- otherwise poll_gamepad would read through
                // our own detour and see a zeroed pad while the menu is open.
                if (!pXInputGetState ||
                    pXInputGetState == reinterpret_cast<XInputGetState_t>(fn))
                    pXInputGetState = g_xi_real[g_xi_slot];
                ++g_xi_slot;
                queued = true;
                flog("[overlay] XInputGetState%s detoured in %s",
                     e ? "Ex(#100)" : "", d);
            }
        }
    }
    if (queued && !hooks::apply())
        flog("[overlay] [WARN] MinHook apply failed; pad may leak to the game while menu open");
}

// ── raw-input capture (keyboard/mouse without focus) ──
// Windows keeps ONE raw-input registration per usage per process; registering
// kb + mouse with our hwnd (RIDEV_INPUTSINK -- we are never the foreground
// thread) atomically routes WM_INPUT to us and starves whoever registered
// before (the game, or DirectInput on its behalf). The prior registration is
// snapshotted and restored exactly on close.
std::vector<RAWINPUTDEVICE> g_rid_saved; // the game's kb/mouse entries
bool g_raw_captured = false;

constexpr USHORT kHidPage = 0x01, kHidMouse = 0x02, kHidKeyboard = 0x06;

bool rid_is_kbm(const RAWINPUTDEVICE& r) {
    return r.usUsagePage == kHidPage &&
           (r.usUsage == kHidMouse || r.usUsage == kHidKeyboard);
}

// The process's current kb/mouse raw-input registrations.
std::vector<RAWINPUTDEVICE> rid_process_kbm() {
    std::vector<RAWINPUTDEVICE> out;
    UINT n = 0;
    GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
    if (!n) return out;
    std::vector<RAWINPUTDEVICE> all(n);
    if (GetRegisteredRawInputDevices(all.data(), &n, sizeof(RAWINPUTDEVICE)) ==
        static_cast<UINT>(-1))
        return out;
    all.resize(n);
    for (const auto& r : all)
        if (rid_is_kbm(r)) out.push_back(r);
    return out;
}

bool rid_register_ours() {
    RAWINPUTDEVICE rid[2] = {
        {kHidPage, kHidMouse,    RIDEV_INPUTSINK, g_hwnd},
        {kHidPage, kHidKeyboard, RIDEV_INPUTSINK, g_hwnd},
    };
    return RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
}

void raw_input_capture(bool on) {
    if (on == g_raw_captured) return;
    if (on) {
        g_rid_saved = rid_process_kbm();
        if (!rid_register_ours()) {
            flog("[overlay] [WARN] raw-input capture failed (err %lu) -- "
                 "keyboard/mouse degraded while menu open", GetLastError());
            return;
        }
        g_raw_captured = true;
        return;
    }
    // Restore the snapshot; a usage that had no prior registration gets
    // RIDEV_REMOVE so we don't keep swallowing input after close.
    RAWINPUTDEVICE restore[2] = {
        {kHidPage, kHidMouse,    RIDEV_REMOVE, nullptr},
        {kHidPage, kHidKeyboard, RIDEV_REMOVE, nullptr},
    };
    for (const auto& r : g_rid_saved)
        restore[r.usUsage == kHidMouse ? 0 : 1] = r;
    if (!RegisterRawInputDevices(restore, 2, sizeof(RAWINPUTDEVICE))) {
        // Per-entry fallback: one stale hwnd (game recreated its window) must
        // not block the other usage; downgrade stale entries to REMOVE -- the
        // game re-registers on demand when it next touches raw input.
        bool ok = true;
        for (auto& r : restore) {
            if (RegisterRawInputDevices(&r, 1, sizeof(RAWINPUTDEVICE))) continue;
            RAWINPUTDEVICE rm{r.usUsagePage, r.usUsage, RIDEV_REMOVE, nullptr};
            if (!RegisterRawInputDevices(&rm, 1, sizeof(RAWINPUTDEVICE))) ok = false;
        }
        if (!ok) {
            flog("[overlay] [ERROR] raw-input restore failed (err %lu) -- "
                 "game input may be dead; retrying", GetLastError());
            return; // stays "captured": the render loop retries every tick
        }
        flog("[overlay] [WARN] raw-input restore downgraded to unregister");
    }
    g_raw_captured = false;
}

// The game (or DirectInput) can re-register raw input while our menu is open
// (device hotplug, window recreation). Once per frame: if any kb/mouse usage
// no longer targets us, fold the newcomer into the restore snapshot and
// re-route to our window.
void raw_input_reassert() {
    if (!g_raw_captured) return;
    bool ours = true;
    for (const auto& r : rid_process_kbm()) {
        if (r.hwndTarget == g_hwnd) continue;
        ours = false;
        bool merged = false;
        for (auto& s : g_rid_saved)
            if (s.usUsage == r.usUsage) { s = r; merged = true; }
        if (!merged) g_rid_saved.push_back(r);
    }
    if (!ours && !rid_register_ours())
        flog("[overlay] [WARN] raw-input re-assert failed (err %lu)", GetLastError());
}

// ── WM_INPUT -> ImGui ──
// Virtual mouse cursor in overlay-client coordinates, integrated from raw
// deltas (immune to whatever the still-focused game does to the OS cursor).
float g_vmx = 0.0f, g_vmy = 0.0f;

void raw_mouse(const RAWMOUSE& m, ImGuiIO& io) {
    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
        // Absolute devices (pen tablets, RDP): 0..65535 across the screen.
        const bool vd = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        POINT p{static_cast<LONG>((vd ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0) +
                    m.lLastX * GetSystemMetrics(vd ? SM_CXVIRTUALSCREEN : SM_CXSCREEN) / 65535),
                static_cast<LONG>((vd ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0) +
                    m.lLastY * GetSystemMetrics(vd ? SM_CYVIRTUALSCREEN : SM_CYSCREEN) / 65535)};
        ScreenToClient(g_hwnd, &p);
        g_vmx = static_cast<float>(p.x);
        g_vmy = static_cast<float>(p.y);
    } else {
        g_vmx += static_cast<float>(m.lLastX);
        g_vmy += static_cast<float>(m.lLastY);
    }
    g_vmx = std::clamp(g_vmx, 0.0f, g_back_w > 1 ? g_back_w - 1.0f : 0.0f);
    g_vmy = std::clamp(g_vmy, 0.0f, g_back_h > 1 ? g_back_h - 1.0f : 0.0f);
    io.AddMousePosEvent(g_vmx, g_vmy);

    const USHORT f = m.usButtonFlags;
    if (f & RI_MOUSE_LEFT_BUTTON_DOWN)   io.AddMouseButtonEvent(0, true);
    if (f & RI_MOUSE_LEFT_BUTTON_UP)     io.AddMouseButtonEvent(0, false);
    if (f & RI_MOUSE_RIGHT_BUTTON_DOWN)  io.AddMouseButtonEvent(1, true);
    if (f & RI_MOUSE_RIGHT_BUTTON_UP)    io.AddMouseButtonEvent(1, false);
    if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) io.AddMouseButtonEvent(2, true);
    if (f & RI_MOUSE_MIDDLE_BUTTON_UP)   io.AddMouseButtonEvent(2, false);
    if (f & RI_MOUSE_BUTTON_4_DOWN)      io.AddMouseButtonEvent(3, true);
    if (f & RI_MOUSE_BUTTON_4_UP)        io.AddMouseButtonEvent(3, false);
    if (f & RI_MOUSE_BUTTON_5_DOWN)      io.AddMouseButtonEvent(4, true);
    if (f & RI_MOUSE_BUTTON_5_UP)        io.AddMouseButtonEvent(4, false);
    if (f & RI_MOUSE_WHEEL)
        io.AddMouseWheelEvent(0.0f,
            static_cast<float>(static_cast<SHORT>(m.usButtonData)) / WHEEL_DELTA);
    if (f & RI_MOUSE_HWHEEL)
        io.AddMouseWheelEvent(
            -static_cast<float>(static_cast<SHORT>(m.usButtonData)) / WHEEL_DELTA, 0.0f);
}

// Forward a raw key as a synthetic WM_KEY* to the ImGui Win32 backend (reuses
// its full VK -> ImGuiKey mapping) plus WM_CHAR text via ToUnicode. The
// backend's own modifier reads (GetKeyState) are stale on this never-focused
// thread; update_modifiers() below feeds the truth every frame. No IME without
// focus -- acceptable for the ASCII-lowercased search box.
void raw_keyboard(const RAWKEYBOARD& k) {
    if (k.VKey == 0 || k.VKey >= 255) return; // fake keys / overrun marker
    const bool down = (k.Flags & RI_KEY_BREAK) == 0;
    const UINT scan = k.MakeCode & 0xFFu;
    LPARAM lp = 1 | (static_cast<LPARAM>(scan) << 16);
    if (k.Flags & RI_KEY_E0) lp |= static_cast<LPARAM>(1) << 24;
    if (!down) lp |= (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
    ImGui_ImplWin32_WndProcHandler(g_hwnd, down ? WM_KEYDOWN : WM_KEYUP, k.VKey, lp);

    if (!down || kd(VK_CONTROL) || kd(VK_MENU)) return; // no text from chords
    BYTE ks[256] = {};
    if (kd(VK_SHIFT)) ks[VK_SHIFT] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 0x01;
    WCHAR buf[4] = {};
    const int n = ToUnicode(k.VKey, scan, ks, buf, 4, 0);
    for (int i = 0; i < n; ++i)
        if (buf[i] >= 32)
            ImGui_ImplWin32_WndProcHandler(g_hwnd, WM_CHAR, buf[i], 0);
}

void on_raw_input(HRAWINPUT h) {
    // Only feed ImGui while it will consume events next frame; a failed
    // restore with the menu closed must not grow the io queue unboundedly.
    if (!g_context_inited || !g_raw_captured || !g_menu_open.load()) return;
    RAWINPUT ri{};
    UINT sz = sizeof(ri);
    if (GetRawInputData(h, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) ==
        static_cast<UINT>(-1))
        return;
    if (ri.header.dwType == RIM_TYPEMOUSE)
        raw_mouse(ri.data.mouse, ImGui::GetIO());
    else if (ri.header.dwType == RIM_TYPEKEYBOARD)
        raw_keyboard(ri.data.keyboard);
}

// Feed modifier state from the hardware. The win32 backend derives modifiers
// from GetKeyState, which only updates for threads that receive real keyboard
// messages -- ours never does.
void update_modifiers() {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl,  kd(VK_CONTROL));
    io.AddKeyEvent(ImGuiMod_Shift, kd(VK_SHIFT));
    io.AddKeyEvent(ImGuiMod_Alt,   kd(VK_MENU));
    io.AddKeyEvent(ImGuiMod_Super, kd(VK_LWIN) || kd(VK_RWIN));
}

// ── window ──
LRESULT CALLBACK overlay_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INPUT:
        on_raw_input(reinterpret_cast<HRAWINPUT>(lParam));
        return DefWindowProcW(hWnd, msg, wParam, lParam); // required cleanup
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // NEVER take focus (see the input section)
    case WM_SETCURSOR:
        // Hide the OS cursor over our window ONLY while the menu is open (we draw
        // a software cursor then). While closed we must never dictate the cursor
        // -- SetCursor(nullptr) sets the global cursor to none, which on the
        // DComp path (window still shown-transparent over the game) would leave
        // the game's cursor hidden. Fall through so the game keeps its cursor.
        if (g_menu_open.load() && LOWORD(lParam) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    case WM_DESTROY:
        return 0;
    }
    // Legacy mouse messages still arrive over our topmost window while it is
    // interactive; raw input is the single mouse source (double-feeding makes
    // the cursor jump between the OS and virtual positions), so swallow them.
    // The focus_input fallback keeps the old native path through the backend.
    if (!g_focus_input && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)
        return 0;
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 0;
    return DefWindowProcW(hWnd, msg, wParam, lParam);
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

// Toggle the overlay window between hit-testable (menu open: it swallows the
// legacy mouse messages so they can't reach the game) and click-through (menu
// closed: everything falls through). Only WS_EX_TRANSPARENT toggles --
// WS_EX_NOACTIVATE is permanent, the overlay never takes focus. The window
// stays SHOWN either way; keeping the game covered holds its DWM presentation
// steady so closing never triggers a present-mode transition.
void set_click_through(bool on) {
    if (!g_hwnd) return;
    LONG ex = GetWindowLongW(g_hwnd, GWL_EXSTYLE);
    if (on) ex |=  WS_EX_TRANSPARENT;
    else    ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongW(g_hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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
    // erdGameTools hooks IDXGISwapChain::Present via the SHARED dxgi vtable
    // (dummy-device vtable extraction) -- a vtable detour fires for EVERY
    // swapchain in the process, not just the game's. If we present a DXGI
    // swapchain, its detour runs its DX12 renderer against our D3D11
    // composition swapchain and access-violates inside erdGameTools.dll
    // (confirmed by Windows crash logs). When it's loaded, never create a DXGI
    // swapchain at all: present through the layered-window path (GDI
    // UpdateLayeredWindow), which no DXGI hook can see.
    const bool avoid_dxgi_present = GetModuleHandleA("erdGameTools.dll") != nullptr;
    HRESULT hr = E_FAIL;
    if (!avoid_dxgi_present)
        hr = factory->CreateSwapChainForComposition(g_d3d_device, &scd, nullptr, &g_swapchain);
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
        // ── Layered path: Proton/Wine (composition swapchains are E_NOTIMPL)
        //    or another mod's global DXGI Present hook must be avoided. ──
        dxgiDevice->Release();
        if (avoid_dxgi_present)
            flog("[overlay] erdGameTools detected -- using hook-safe layered presentation");
        else
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
        load_fonts(io);
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

    // Shared gold accent (matches apply_er_style()'s `gold`).
    const ImVec4 kGold(0.80f, 0.68f, 0.40f, 1.0f);

    // ── Header: emphasized title + mod-page link, set off by a separator ──
    ImGui::SetWindowFontScale(1.15f);
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::TextUnformatted("Custom Talisman Effects");
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    if (kModPageUrl[0] != '\0') {
        // Corner link to the mod page (opens the default browser).
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 52.0f);
        if (ImGui::SmallButton("Nexus"))
            ShellExecuteA(nullptr, "open", kModPageUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::Separator();
    ImGui::Spacing();

    // ── Import prompt: a new character with no saved preset, shown only when
    //    OTHER characters already have presets to import from (set by the worker).
    if (g_state.import_prompt_active && !g_state.import_candidates.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.12f, 0.05f, 0.85f));
        ImGui::BeginChild("##import_banner", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Border);
        ImGui::TextColored(kGold, "New character - no saved preset");
        ImGui::TextWrapped("Start fresh (all talismans off), or import another "
                           "character's setup:");
        static int import_sel = 0;
        if (import_sel >= static_cast<int>(g_state.import_candidates.size())) import_sel = 0;
        auto cand_label = [&](int i) {
            const auto& c = g_state.import_candidates[static_cast<size_t>(i)];
            return c.second.empty() ? c.first : c.second; // display name, else key
        };
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##import_from", cand_label(import_sel).c_str())) {
            for (int i = 0; i < static_cast<int>(g_state.import_candidates.size()); ++i) {
                const bool is_sel = (i == import_sel);
                if (ImGui::Selectable(cand_label(i).c_str(), is_sel)) import_sel = i;
                if (is_sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        // The worker performs the copy + save (all disk I/O off the render thread);
        // clear the prompt now for immediate feedback.
        if (ImGui::Button("Import selected")) {
            g_state.import_from_key = g_state.import_candidates[static_cast<size_t>(import_sel)].first;
            g_state.import_requested = true;
            g_state.import_prompt_active = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Start fresh")) {
            g_state.import_from_key.clear();
            g_state.import_requested = true;
            g_state.import_prompt_active = false;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // ── Settings: the global toggles, grouped and set off from the list below ──
    bool stacking = g_state.allow_stacking;
    if (ImGui::Checkbox("Allow stacking (ignore talisman families)", &stacking)) {
        g_state.allow_stacking = stacking;
        if (!stacking) collapse_groups_locked();
    }
    bool progression = g_state.progression_mode;
    if (ImGui::Checkbox("Progression mode (owned talismans only)", &progression)) {
        g_state.progression_mode = progression;
        g_state.save_requested = true; // persist the toggle like the others
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    static char filter[64] = "";
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputTextWithHint("##search", "Search talismans...", filter, sizeof(filter));
    ImGui::SameLine();
    if (ImGui::Button("Clear")) filter[0] = '\0';

    int on = 0;
    for (const auto& t : g_state.talismans) if (t.enabled) ++on;
    // Status indicator: enabled count in gold, total muted.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGold, "Enabled: %d", on);
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextDisabled("/ %d", static_cast<int>(g_state.talismans.size()));
    ImGui::SameLine();
    // Save = primary action (gold-tinted). Disable all = secondary (default,
    // only brightening on hover via the theme).
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.42f, 0.34f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.45f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.66f, 0.54f, 0.27f, 1.0f));
    if (ImGui::Button("Save changes")) g_state.save_requested = true;
    ImGui::PopStyleColor(3);
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
            // Progression mode: hide talismans the player doesn't currently own
            // (fails open until the first good inventory read).
            if (g_state.progression_mode && g_state.possessed_valid &&
                !g_state.possessed_accessories.count(t.accessory_id)) continue;
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
        if (shown == 0) {
            const bool progression_empty =
                g_state.progression_mode && g_state.possessed_valid && needle.empty();
            ImGui::TextDisabled(
                progression_empty ? "You haven't found any talismans yet."
                : base_tab        ? "No talismans match your search."
                                  : "No mod-added talismans detected in this regulation.");
        }
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

    static bool prev_key_in = false;
    const bool key_in = ours && kd(static_cast<int>(g_open_vk));
    bool toggled = key_in && !prev_key_in;
    prev_key_in = key_in;

    const bool combo_in = ours && g_pad_ok && g_open_pad_mask &&
                          (g_pad.wButtons & g_open_pad_mask) == g_open_pad_mask;
    if (g_open_pad_is_hold) {
        static bool combo_active = false, combo_fired = false;
        static std::chrono::steady_clock::time_point combo_since;
        if (combo_in) {
            if (!combo_active) {
                combo_active = true;
                combo_fired = false;
                combo_since = std::chrono::steady_clock::now();
            } else if (!combo_fired) {
                const auto held_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - combo_since).count();
                if (held_ms >= kPadHoldMs) { toggled = true; combo_fired = true; }
            }
        } else {
            combo_active = false;
            combo_fired = false;
        }
    } else {
        static bool prev_combo_in = false;
        if (combo_in && !prev_combo_in) toggled = true;
        prev_combo_in = combo_in;
    }

    if (toggled)
        g_menu_open.store(!g_menu_open.load());

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
        // Cover the game first (so we open at the right place/size), make the
        // window hit-testable, and redirect raw input to us. The game KEEPS
        // foreground focus and stays shown-covered: neither its activation nor
        // its presentation state changes, so frame-gen pipelines don't re-init.
        // DComp path: the window is HIDDEN while the menu is closed (see the
        // close branch) so it can't intercept the game's cursor/clicks -- reveal
        // it here. It still holds the transparent frame committed on close, so
        // showing it is flash-free. Layered path stays always-shown.
        if (!g_use_layered) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        int gw = 0, gh = 0;
        cover_game_window(gw, gh);
        set_click_through(false);
        if (g_focus_input) {
            force_foreground(g_hwnd); // legacy escape-hatch mode ([overlay] focus_input)
        } else {
            hook_xinput();  // install/refresh pad detours LAST-hooked = outermost
            hook_dinput8(); // no-op if already hooked; retry if dinput8 loaded late
            { // fresh freeze-frames for custom-format devices this session
                std::lock_guard<std::mutex> lk(g_di_mutex);
                g_joy_frozen.clear();
            }
            raw_input_capture(true);
            // Start the virtual cursor centered.
            g_vmx = 0.5f * static_cast<float>(gw > 0 ? gw : static_cast<int>(g_back_w));
            g_vmy = 0.5f * static_cast<float>(gh > 0 ? gh : static_cast<int>(g_back_h));
            if (g_context_inited) ImGui::GetIO().AddMousePosEvent(g_vmx, g_vmy);
        }
    } else if (!open_now && prev_open) {
        // Blank to one fully-transparent frame and go click-through; the
        // window stays SHOWN (hiding uncovers the game -> DWM present-mode
        // transition) and the game never lost focus, so gameplay resumes
        // instantly -- no frozen-frame stall.
        render_frame(false);
        set_click_through(true);
        // DComp path: WS_EX_TRANSPARENT click-through is unreliable for this
        // (non-layered) topmost window -- it keeps receiving WM_SETCURSOR/clicks
        // over the game, stealing the cursor. Hide it outright while closed; the
        // game has no frame-gen on this path (erdGameTools forces the layered
        // path), so hiding can't trigger the activation/present freeze. The
        // layered path stays shown: its alpha-0 frame is already input-transparent.
        if (!g_use_layered) ShowWindow(g_hwnd, SW_HIDE);
        if (g_focus_input && g_game_hwnd) force_foreground(g_game_hwnd);
        raw_input_capture(false); // give the game its raw input back
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

    // WS_EX_NOACTIVATE: the overlay must never take focus -- activation changes
    // on the game window make frame-gen mods re-init for seconds (alt-tab
    // freeze); input is captured focus-free (see the input section).
    // WS_EX_NOREDIRECTIONBITMAP is required for the DComp swapchain path.
    const DWORD ex = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                     WS_EX_NOACTIVATE;
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

    // Menu starts closed. Closed-state window handling differs by path:
    //  - Layered (erdGameTools/Wine): stay SHOWN with a committed alpha-0 frame.
    //    That frame is per-pixel input-transparent, so the window holds the
    //    game's DWM presentation steady without intercepting cursor/clicks. This
    //    is also what avoids the erdGameTools activation/present freeze.
    //  - DComp: HIDDEN while closed. WS_EX_TRANSPARENT click-through is
    //    unreliable for this non-layered topmost window, so leaving it shown
    //    would steal WM_SETCURSOR from the game (cursor vanishes) and cover the
    //    desktop on exit. No erdGameTools on this path, so hiding is freeze-safe.
    // Commit one transparent frame first (the layered path needs its content set
    // via UpdateLayeredWindow before it is shown).
    set_click_through(true);
    render_frame(false); // commit one fully-transparent frame
    if (g_use_layered) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    else               ShowWindow(g_hwnd, SW_HIDE);

    while (g_running.load()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Track the game window every tick (not only while covering) so the
        // shutdown check below works even if the menu was never opened.
        find_game_window();

        // Game shutdown detection: when the game window dies but the process
        // lingers (me3 keeps it alive during teardown), our always-shown
        // topmost window would keep sitting over the desktop and leave it
        // unresponsive until the process finally exits. Give the game a short
        // grace period to recreate its window (display-mode changes do that),
        // then tear the overlay down for good.
        static ULONGLONG dead_since = 0;
        if (g_game_hwnd && !IsWindow(g_game_hwnd)) {
            g_game_hwnd = nullptr; // stale; find_game_window may re-adopt
            dead_since = GetTickCount64();
            // The game window is gone: there is nothing left to keep steady, so
            // drop our always-shown topmost window IMMEDIATELY rather than waiting
            // out the grace -- otherwise it covers the desktop and locks it until
            // the process finally exits (me3 lingers). Safe on both paths: no
            // game window means no DWM/frame-gen state to protect.
            ShowWindow(g_hwnd, SW_HIDE);
            flog("[overlay] game window died -- overlay hidden, grace before teardown");
        }
        if (dead_since) {
            // Only cancel teardown if a REAL game window came back (a display-mode
            // change recreates it). A transient or hidden process window during
            // me3 shutdown must not keep resetting the grace timer forever, or the
            // thread never exits.
            HWND back = find_game_window();
            RECT cr{};
            const bool real = back && IsWindow(back) && IsWindowVisible(back) &&
                              GetClientRect(back, &cr) &&
                              (cr.right - cr.left) > 0 && (cr.bottom - cr.top) > 0;
            if (real) {
                dead_since = 0; // window came back (display-mode change)
                flog("[overlay] game window re-adopted -- overlay restored");
                // Re-show if the menu is open, or on the layered path (which is
                // always-shown even when closed). Closed DComp stays hidden.
                if (g_menu_open.load() || g_use_layered)
                    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
            } else if (GetTickCount64() - dead_since > 3000) {
                flog("[overlay] game window gone -- removing overlay window");
                g_menu_open.store(false);
                break; // -> teardown() destroys our window; thread exits
            }
        }

        poll_gamepad();
        update_menu_toggle();

        if (g_menu_open.load()) {
            // Only do GPU work (resize + render + Present) while the menu is
            // open. Keep our overlay sized to the game's client area.
            int gw = 0, gh = 0;
            cover_game_window(gw, gh);
            if (gw > 0 && gh > 0 &&
                (static_cast<UINT>(gw) != g_back_w || static_cast<UINT>(gh) != g_back_h))
                seh_resize(static_cast<UINT>(gw), static_cast<UINT>(gh));

            raw_input_reassert(); // keep kb/mouse routed to us (game may re-register)

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            feed_gamepad();
            update_modifiers(); // hardware truth; backend's GetKeyState is stale here
            ImGui::NewFrame();
            ImGui::GetIO().MouseDrawCursor = true; // our window has no OS cursor
            draw_talisman_window();
            ImGui::Render();
            render_frame(true);
        } else {
            // Menu closed -> our window stays SHOWN but holds its last committed
            // fully-transparent frame (see update_menu_toggle), so it's invisible
            // and click-through while keeping the game covered/composited.
            // Deliberately do NO GPU work here: no Present, no ResizeBuffers. A
            // thread suspended mid-Present holds a driver/DXGI lock, and when
            // another mod installs its own hooks via MinHook (which suspends
            // EVERY thread to patch code) while needing that same lock, the
            // game deadlocks/freezes on startup. Idling here keeps this thread
            // safe to suspend and cuts pointless GPU churn. The first open frame
            // re-covers + resizes before drawing, so nothing is stale.
            if (g_raw_captured) raw_input_capture(false); // retry a failed restore
            Sleep(32);
        }
    }
}

// ── best-effort teardown (the process usually just exits) ──
void teardown() {
    raw_input_capture(false); // never leave the game's raw input starved
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

void overlay::sync_open_keys() {
    std::lock_guard<std::mutex> lk(g_state_mutex);
    g_open_vk = g_state.open_vk;
    g_open_pad_mask = g_state.open_pad_mask;
    g_open_pad_is_hold = g_state.open_pad_is_hold;
    g_focus_input = g_state.focus_input;
}

void overlay::setup() {
    sync_open_keys();

    // Gamepad. We must both READ the pad (menu nav + the open/close combo) and
    // BLOCK it from reaching the still-focused game while the menu is open.
    // Reading: resolve XInputGetState directly here (no hook needed to read).
    // Blocking: XInput detours are installed by hook_xinput() at MENU OPEN,
    // deliberately not here -- hooking early proved ineffective (see the
    // comment on hook_xinput); dinput8 (the path ER actually polls) is hooked
    // now and retried on open if the module loads late.
    {
        const char* xdlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        for (const char* d : xdlls) {
            HMODULE h = GetModuleHandleA(d);
            if (!h) continue;
            pXInputGetState =
                reinterpret_cast<XInputGetState_t>(GetProcAddress(h, "XInputGetState"));
            if (pXInputGetState) break;
        }
        if (!pXInputGetState)
            if (HMODULE h = LoadLibraryA("xinput1_4.dll"))
                pXInputGetState =
                    reinterpret_cast<XInputGetState_t>(GetProcAddress(h, "XInputGetState"));
        if (!pXInputGetState)
            flog("[overlay] [WARN] XInputGetState not found; gamepad unavailable");

        g_mh_ok = hooks::init();
        if (!g_mh_ok)
            flog("[overlay] [WARN] MinHook init failed; pad will leak to the game while menu open");
        hook_dinput8();
    }

    // Spawn the dedicated overlay thread (window + D3D11 + DComp + ImGui + loop).
    if (g_running.exchange(true)) return; // already running
    std::thread([] {
        overlay_thread();
        teardown();
        g_running.store(false);
    }).detach();
    flog("[overlay] separate-window overlay thread spawned (focus-free input)");
}

} // namespace cte

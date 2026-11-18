// PowerMenu - shows a Lock / Sleep / Power Off / Restart menu when the power button is pressed.
// Lock works with one click (or L); the others must be held for three seconds before they run,
// with a pointer or by holding their key (S, O, R).
//
// The power button reaches Windows as a HID collection carrying the Generic Desktop
// "System Power Down" usage (0x01/0x81): the ACPI button array (ACPI0011, usage 0x01/0x0D)
// and the System Control collection (0x01/0x80) the button converter makes from it, plus
// power keys on keyboards. Raw Input is no use for this: Windows keeps System Control
// collections out of the Raw Input device list, so WM_INPUT never arrives. They are opened
// shared, though, and HIDClass gives every open handle its own copy of each report, so a
// worker thread reads them directly. Windows' own power-button action is set to
// "Do nothing" (--take-button) so the two don't fight.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <wtsapi32.h>
#include <setupapi.h>
#include <dbt.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <powrprof.h>
#include <objidl.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdarg.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "userenv.lib")

using namespace Gdiplus;

namespace {

constexpr ULONGLONG kHoldMs = 3000;
constexpr UINT kIdleCloseMs = 60000;
constexpr ULONGLONG kWakeGraceMs = 4000;   // a press this soon after the screen comes on woke the PC
constexpr ULONGLONG kEchoMs = 400;         // a press this soon after a release is the same click seen elsewhere
constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_SHOW = WM_APP + 2;
constexpr UINT WM_APP_POWER = WM_APP + 3;  // lParam: new std::wstring with the device path
constexpr UINT_PTR TIMER_HOLD = 1, TIMER_RUN = 2, TIMER_IDLE = 3;
constexpr UINT ID_TRAY_SHOW = 100, ID_TRAY_LOG = 101, ID_TRAY_EXIT = 102;

const wchar_t kListenerClass[] = L"PowerMenu.Listener";
const wchar_t kBackdropClass[] = L"PowerMenu.Backdrop";
const wchar_t kPanelClass[] = L"PowerMenu.Panel";
const wchar_t kMutexName[] = L"Local\\PowerMenu.SingleInstance";
const wchar_t kSavedKey[] = L"Software\\PowerMenu\\SavedButtonAction";

// SUB_BUTTONS / PBUTTONACTION
const GUID kSubButtons = {0x4f971e89, 0xeebd, 0x4455, {0xa8, 0xde, 0x9e, 0x59, 0x04, 0x0e, 0x73, 0x47}};
const GUID kPowerButtonAction = {0x7648efa3, 0xdd9c, 0x4e3e, {0xb5, 0x66, 0x50, 0xf9, 0x29, 0x38, 0x62, 0x80}};
// GUID_CONSOLE_DISPLAY_STATE / GUID_DEVINTERFACE_HID
const GUID kConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
const GUID kHidInterface = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

enum Action { ActLock, ActSleep, ActPowerOff, ActRestart, ActCount };

struct ActionInfo {
    const wchar_t* label;
    const wchar_t* busy;
    const wchar_t* verb;
    wchar_t key;   // shortcut letter; virtual-key codes, so it is the same key on Thai and other layouts
    wchar_t glyph;
    Color color;
    bool instant;  // runs on a click instead of a hold
};

const ActionInfo kActions[ActCount] = {
    {L"Lock", L"Locking...", L"lock", L'L', 0xE72E, Color(255, 72, 199, 142), true},
    {L"Sleep", L"Going to sleep...", L"sleep", L'S', 0xE708, Color(255, 79, 142, 247), false},
    {L"Power Off", L"Powering off...", L"power off", L'O', 0xE7E8, Color(255, 232, 72, 58), false},
    {L"Restart", L"Restarting...", L"restart", L'R', 0xE72C, Color(255, 242, 169, 59), false},
};

HINSTANCE g_inst;
bool g_dryRun;
HWND g_listener, g_backdrop, g_panel;
UINT g_taskbarCreated;
bool g_locked;
std::wstring g_logPath;
SRWLOCK g_logLock = SRWLOCK_INIT;

HANDLE g_hidThread, g_hidStop, g_hidRescan;
// Written by the service control handler and read by the HID thread in service mode.
std::atomic<bool> g_displayOn{true};
std::atomic<ULONGLONG> g_displayOnTick{0};

bool g_service;  // running as the PowerMenu service (session 0, no UI)
SERVICE_STATUS_HANDLE g_svcHandle;
HANDLE g_svcStop;
const wchar_t kServiceName[] = L"PowerMenu";
const wchar_t kDryRunTitle[] = L"PowerMenu (dry run)";

// Menu state.
float g_scale = 1.0f;
int g_hover = -1;
int g_focus = 0;
bool g_showFocus;
int g_holdIdx = -1;
ULONGLONG g_holdStart;
UINT32 g_holdPointer;        // pointer id when held with mouse/touch/pen
WPARAM g_holdKey;            // virtual key when held from the keyboard
POINTER_INPUT_TYPE g_holdPointerType = PT_POINTER;
int g_doneIdx = -1;
int g_pressIdx = -1;         // an instant option pressed by a pointer, runs on release
UINT32 g_pressPointer;
bool g_cancelHot, g_cancelPressed;

// ---------------------------------------------------------------------------
// Logging

void Log(const wchar_t* fmt, ...) {
    if (g_logPath.empty()) return;
    struct Guard {
        Guard() { AcquireSRWLockExclusive(&g_logLock); }
        ~Guard() { ReleaseSRWLockExclusive(&g_logLock); }
    } guard;  // the HID thread logs too
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath.c_str(), L"a, ccs=UTF-8") || !f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"%04d-%02d-%02d %02d:%02d:%02d  ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fputwc(L'\n', f);
    fclose(f);
}

void InitLog() {
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring d = std::wstring(dir) + L"\\PowerMenu";
    CreateDirectoryW(d.c_str(), nullptr);
    g_logPath = d + L"\\PowerMenu.log";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 512 * 1024)
        DeleteFileW(g_logPath.c_str());
}

// ---------------------------------------------------------------------------
// Power actions

bool EnableShutdownPrivilege() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
    AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    bool ok = GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

bool FastStartupEnabled() {
    DWORD v = 0, cb = sizeof(v);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
                        L"HiberbootEnabled", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS && v == 1;
}

void RunAction(int act) {
    Log(L"action: %s%s", kActions[act].label, g_dryRun ? L" (dry run)" : L"");
    if (g_dryRun) {
        wchar_t msg[128];
        swprintf_s(msg, L"Dry run: \"%s\" would run now.", kActions[act].label);
        MessageBoxW(nullptr, msg, L"PowerMenu", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        return;
    }
    EnableShutdownPrivilege();
    const DWORD reason = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
    BOOL ok = FALSE;
    switch (act) {
    case ActLock:
        ok = LockWorkStation();
        break;
    case ActSleep:
        ok = SetSuspendState(FALSE, FALSE, FALSE);
        if (!ok) {
            // Fall back to switching the display off, which lets Modern Standby take over.
            Log(L"SetSuspendState failed (%lu), turning the display off", GetLastError());
            SendMessageTimeoutW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2, SMTO_ABORTIFHUNG, 1000, nullptr);
            ok = TRUE;
        }
        break;
    case ActPowerOff:
        ok = ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF | (FastStartupEnabled() ? EWX_HYBRID_SHUTDOWN : 0), reason);
        break;
    case ActRestart:
        ok = ExitWindowsEx(EWX_REBOOT, reason);
        break;
    }
    if (!ok) {
        DWORD err = GetLastError();
        Log(L"action failed: %lu", err);
        if (g_service) return;  // no desktop to show a message on
        wchar_t msg[160];
        swprintf_s(msg, L"Could not %s (error %lu).", kActions[act].verb, err);
        MessageBoxW(nullptr, msg, L"PowerMenu", MB_OK | MB_ICONERROR | MB_TOPMOST);
    }
}

void OnDisplayState(bool on) {
    bool was = g_displayOn.exchange(on);
    if (on != was) Log(L"screen %s", on ? L"on" : L"off");
    if (on && !was) g_displayOnTick = GetTickCount64();
}

// Why a press should be ignored as the one that woke the PC, or nullptr.
const wchar_t* WakePress(ULONGLONG now) {
    if (!g_displayOn) return L"screen was off (wake press)";
    ULONGLONG on = g_displayOnTick;
    if (on && now - on < kWakeGraceMs) return L"screen just came on (wake press)";
    return nullptr;
}

// ---------------------------------------------------------------------------
// Windows power-button setting (--take-button / --restore-button, run elevated by the installer)

std::vector<GUID> PowerSchemes() {
    std::vector<GUID> out;
    for (ULONG i = 0;; i++) {
        GUID g;
        DWORD cb = sizeof(g);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, i, reinterpret_cast<UCHAR*>(&g), &cb) != ERROR_SUCCESS) break;
        out.push_back(g);
    }
    return out;
}

void ReapplyActiveScheme() {
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) == ERROR_SUCCESS) {
        PowerSetActiveScheme(nullptr, active);
        LocalFree(active);
    }
}

int TakeButton() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSavedKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return 2;
    int failures = 0;
    for (const GUID& s : PowerSchemes()) {
        wchar_t name[64];
        StringFromGUID2(s, name, 64);
        DWORD ac = 0, dc = 0;
        if (PowerReadACValueIndex(nullptr, &s, &kSubButtons, &kPowerButtonAction, &ac) != ERROR_SUCCESS) continue;
        PowerReadDCValueIndex(nullptr, &s, &kSubButtons, &kPowerButtonAction, &dc);
        // Keep the very first saved value, so a reinstall doesn't save our own "Do nothing".
        if (RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            ULONGLONG packed = (ULONGLONG(ac) << 32) | dc;
            RegSetValueExW(key, name, 0, REG_QWORD, reinterpret_cast<BYTE*>(&packed), sizeof(packed));
        }
        if (PowerWriteACValueIndex(nullptr, &s, &kSubButtons, &kPowerButtonAction, 0) != ERROR_SUCCESS) failures++;
        if (PowerWriteDCValueIndex(nullptr, &s, &kSubButtons, &kPowerButtonAction, 0) != ERROR_SUCCESS) failures++;
    }
    RegCloseKey(key);
    ReapplyActiveScheme();
    return failures ? 1 : 0;
}

int RestoreButton() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSavedKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    for (const GUID& s : PowerSchemes()) {
        wchar_t name[64];
        StringFromGUID2(s, name, 64);
        ULONGLONG packed;
        DWORD cb = sizeof(packed), type;
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&packed), &cb) != ERROR_SUCCESS || type != REG_QWORD)
            continue;
        PowerWriteACValueIndex(nullptr, &s, &kSubButtons, &kPowerButtonAction, DWORD(packed >> 32));
        PowerWriteDCValueIndex(nullptr, &s, &kSubButtons, &kPowerButtonAction, DWORD(packed));
    }
    RegCloseKey(key);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\PowerMenu");
    ReapplyActiveScheme();
    return 0;
}

// ---------------------------------------------------------------------------
// Menu layout (in device-independent pixels, scaled by g_scale)

constexpr float kMargin = 28, kTileW = 152, kTileH = 180, kGap = 14, kTilesTop = 96;
constexpr float kCancelW = 148, kCancelH = 36;
constexpr float kPanelW = kMargin * 2 + kTileW * ActCount + kGap * (ActCount - 1);
constexpr float kCancelTop = kTilesTop + kTileH + 22;
constexpr float kPanelH = kCancelTop + kCancelH + 24;

float S(float v) { return v * g_scale; }

RectF TileRect(int i) { return RectF(S(kMargin + i * (kTileW + kGap)), S(kTilesTop), S(kTileW), S(kTileH)); }
RectF CancelRect() { return RectF(S((kPanelW - kCancelW) / 2), S(kCancelTop), S(kCancelW), S(kCancelH)); }

int HitTile(POINT pt) {
    for (int i = 0; i < ActCount; i++)
        if (TileRect(i).Contains(float(pt.x), float(pt.y))) return i;
    return -1;
}

bool HitCancel(POINT pt) { return CancelRect().Contains(float(pt.x), float(pt.y)) != FALSE; }

// The option for a shortcut key: its letter, or its position (1-4, main row or keypad).
int KeyAction(WPARAM vk) {
    for (int i = 0; i < ActCount; i++)
        if (vk == WPARAM(kActions[i].key)) return i;
    if (vk >= '1' && vk < WPARAM('1' + ActCount)) return int(vk - '1');
    if (vk >= VK_NUMPAD1 && vk < WPARAM(VK_NUMPAD1 + ActCount)) return int(vk - VK_NUMPAD1);
    return -1;
}

void RoundRectPath(GraphicsPath& p, const RectF& r, float rad) {
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

const wchar_t* PickFamily(std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names) {
        FontFamily f(n);
        if (f.IsAvailable()) return n;
    }
    return L"Segoe UI";
}

float HoldProgress() {
    if (g_doneIdx >= 0) return 1.0f;
    if (g_holdIdx < 0) return 0.0f;
    ULONGLONG el = GetTickCount64() - g_holdStart;
    return el >= kHoldMs ? 1.0f : float(el) / float(kHoldMs);
}

void PaintPanel(HWND hwnd, HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    Bitmap buffer(rc.right, rc.bottom, PixelFormat32bppPARGB);
    Graphics g(&buffer);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.Clear(Color(255, 32, 32, 32));

    static const wchar_t* text = PickFamily({L"Segoe UI Variable Text", L"Segoe UI"});
    static const wchar_t* textBold = PickFamily({L"Segoe UI Variable Display Semib", L"Segoe UI Semibold", L"Segoe UI"});
    static const wchar_t* icons = PickFamily({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});

    SolidBrush white(Color(255, 255, 255, 255));
    SolidBrush dim(Color(255, 170, 170, 170));
    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    center.SetLineAlignment(StringAlignmentCenter);

    Font title(textBold, S(20), FontStyleRegular, UnitPixel);
    Font sub(text, S(13), FontStyleRegular, UnitPixel);
    g.DrawString(L"What do you want to do?", -1, &title, PointF(S(kMargin), S(26)), &white);
    wchar_t subtitle[128];
    swprintf_s(subtitle, L"Lock works with one click. Hold the others, or their key, for %llu seconds.  Esc cancels.", kHoldMs / 1000);
    g.DrawString(subtitle, -1, &sub, PointF(S(kMargin), S(58)), &dim);

    Font label(textBold, S(16), FontStyleRegular, UnitPixel);
    Font hint(text, S(12.5f), FontStyleRegular, UnitPixel);
    Font glyph(icons, S(30), FontStyleRegular, UnitPixel);
    Font keyFont(textBold, S(12), FontStyleRegular, UnitPixel);
    float progress = HoldProgress();

    for (int i = 0; i < ActCount; i++) {
        const ActionInfo& a = kActions[i];
        RectF r = TileRect(i);
        bool active = (g_holdIdx == i || g_doneIdx == i);
        bool pressed = (g_pressIdx == i);
        bool dimmed = (g_doneIdx >= 0 && g_doneIdx != i);
        float p = active ? progress : pressed ? 1.0f : 0.0f;

        GraphicsPath shape;
        RoundRectPath(shape, r, S(10));
        BYTE base = (g_hover == i && g_doneIdx < 0) ? 52 : 43;
        SolidBrush tileBg(Color(255, base, base, base));
        g.FillPath(&tileBg, &shape);

        if (p > 0) {
            // Fill rises from the bottom as the hold progresses.
            Region clip(&shape);
            g.SetClip(&clip);
            SolidBrush fill(Color(70, a.color.GetR(), a.color.GetG(), a.color.GetB()));
            g.FillRectangle(&fill, r.X, r.GetBottom() - r.Height * p, r.Width, r.Height * p);
            g.ResetClip();
        }

        if (g_showFocus && g_focus == i && g_doneIdx < 0) {
            Pen ring(Color(255, 255, 255, 255), S(2));
            RectF fr = r;
            fr.Inflate(-S(1), -S(1));
            GraphicsPath fp;
            RoundRectPath(fp, fr, S(9));
            g.DrawPath(&ring, &fp);
        }

        // Keycap with the option's shortcut letter.
        RectF cap(r.X + S(10), r.Y + S(10), S(22), S(22));
        GraphicsPath capPath;
        RoundRectPath(capPath, cap, S(4));
        Pen capPen(Color(255, 100, 100, 100), S(1));
        g.DrawPath(&capPen, &capPath);
        wchar_t key[2] = {a.key, 0};
        SolidBrush faint(Color(255, 175, 175, 175));
        g.DrawString(key, -1, &keyFont, cap, &center, &faint);

        // Progress ring around the icon.
        float cx = r.X + r.Width / 2, cy = r.Y + S(66), rad = S(36);
        RectF ringRect(cx - rad, cy - rad, rad * 2, rad * 2);
        Pen track(Color(255, 70, 70, 70), S(4));
        g.DrawEllipse(&track, ringRect);
        if (p > 0) {
            Pen arc(a.color, S(4));
            arc.SetStartCap(LineCapRound);
            arc.SetEndCap(LineCapRound);
            g.DrawArc(&arc, ringRect, -90.0f, 360.0f * p);
        }

        wchar_t gl[2] = {a.glyph, 0};
        SolidBrush glyphBrush(dimmed ? Color(255, 110, 110, 110) : (p > 0 ? a.color : Color(255, 235, 235, 235)));
        g.DrawString(gl, -1, &glyph, RectF(cx - rad, cy - rad, rad * 2, rad * 2), &center, &glyphBrush);

        SolidBrush labelBrush(dimmed ? Color(255, 120, 120, 120) : Color(255, 255, 255, 255));
        g.DrawString(a.label, -1, &label, RectF(r.X, r.Y + S(116), r.Width, S(24)), &center, &labelBrush);

        wchar_t line[64];
        if (g_doneIdx == i) {
            wcscpy_s(line, a.busy);
        } else if (g_holdIdx == i) {
            ULONGLONG el = GetTickCount64() - g_holdStart;
            double left = el >= kHoldMs ? 0.0 : (kHoldMs - el) / 1000.0;
            swprintf_s(line, L"Keep holding  %.1f s", left);
        } else if (a.instant) {
            swprintf_s(line, pressed ? L"Release to %s" : L"Click to %s", a.verb);
        } else {
            swprintf_s(line, L"Hold for %llu seconds", kHoldMs / 1000);
        }
        SolidBrush hintBrush(active || pressed ? a.color : Color(255, 150, 150, 150));
        g.DrawString(line, -1, &hint, RectF(r.X, r.Y + S(142), r.Width, S(22)), &center, &hintBrush);
    }

    // Cancel button.
    RectF cr = CancelRect();
    GraphicsPath cp;
    RoundRectPath(cp, cr, S(6));
    BYTE cb = g_cancelPressed && g_cancelHot ? 38 : g_cancelHot ? 62 : 52;
    SolidBrush cancelBg(Color(255, cb, cb, cb));
    g.FillPath(&cancelBg, &cp);
    Pen cancelBorder(Color(255, 80, 80, 80), S(1));
    g.DrawPath(&cancelBorder, &cp);
    Font cancelFont(text, S(14), FontStyleRegular, UnitPixel);
    g.DrawString(L"Cancel", -1, &cancelFont, cr, &center, &white);

    Graphics screen(hdc);
    screen.DrawImage(&buffer, 0, 0);
}

// ---------------------------------------------------------------------------
// Menu behaviour

void ResetIdleTimer() {
    if (g_panel) SetTimer(g_panel, TIMER_IDLE, kIdleCloseMs, nullptr);
}

void CancelHold() {
    if (g_holdIdx < 0) return;
    g_holdIdx = -1;
    KillTimer(g_panel, TIMER_HOLD);
    InvalidateRect(g_panel, nullptr, FALSE);
}

void CancelPress() {
    if (g_pressIdx < 0) return;
    g_pressIdx = -1;
    InvalidateRect(g_panel, nullptr, FALSE);
}

void StartHold(int idx, UINT32 pointer, POINTER_INPUT_TYPE pointerType, WPARAM key) {
    if (g_doneIdx >= 0) return;
    g_holdIdx = idx;
    g_holdPointer = pointer;
    g_holdPointerType = pointerType;
    g_holdKey = key;
    g_holdStart = GetTickCount64();
    SetTimer(g_panel, TIMER_HOLD, 15, nullptr);
    ResetIdleTimer();
    InvalidateRect(g_panel, nullptr, FALSE);
}

// Checked on every tick: a mouse hold also needs the button physically down right now, so a
// release this window never heard about (outside it, say) can't let Power Off go through.
bool HoldInputStillDown() {
    if (g_holdKey || (g_holdPointerType != PT_MOUSE && g_holdPointerType != PT_TOUCHPAD)) return true;
    return GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) < 0;
}

void CloseMenu() {
    if (g_panel) DestroyWindow(g_panel);
    if (g_backdrop) DestroyWindow(g_backdrop);
    g_panel = g_backdrop = nullptr;
}

void RunInstant(int idx) {
    CloseMenu();
    RunAction(idx);
}

void ForceForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD me = GetCurrentThreadId();
    if (fgThread && fgThread != me) AttachThreadInput(fgThread, me, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (fgThread && fgThread != me) AttachThreadInput(fgThread, me, FALSE);
}

void ShowMenu() {
    if (g_panel) {
        ForceForeground(g_panel);
        return;
    }
    g_hover = -1;
    g_focus = 0;
    g_showFocus = false;
    g_holdIdx = g_doneIdx = g_pressIdx = -1;
    g_cancelHot = g_cancelPressed = false;

    POINT cur;
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    g_scale = dpiX / 96.0f;

    // Dimmed backdrop over every monitor; clicking it cancels.
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g_backdrop = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kBackdropClass, L"", WS_POPUP,
                                 vx, vy, vw, vh, nullptr, nullptr, g_inst, nullptr);
    SetLayeredWindowAttributes(g_backdrop, 0, 150, LWA_ALPHA);

    int w = int(S(kPanelW) + 0.5f), h = int(S(kPanelH) + 0.5f);
    const RECT& wa = mi.rcWork;
    int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) / 2;
    g_panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kPanelClass, L"PowerMenu", WS_POPUP,
                              x, y, w, h, g_backdrop, nullptr, g_inst, nullptr);
    if (!g_backdrop || !g_panel) {
        Log(L"could not create the menu window: %lu", GetLastError());
        CloseMenu();
        return;
    }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_panel, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_panel, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    // Holding a finger or pen still is the whole point here; no right-click ring.
    BOOL off = FALSE;
    SetWindowFeedbackSetting(g_panel, FEEDBACK_TOUCH_PRESSANDHOLD, 0, sizeof(off), &off);
    SetWindowFeedbackSetting(g_panel, FEEDBACK_PEN_PRESSANDHOLD, 0, sizeof(off), &off);
    SetWindowFeedbackSetting(g_panel, FEEDBACK_TOUCH_RIGHTTAP, 0, sizeof(off), &off);

    ShowWindow(g_backdrop, SW_SHOWNOACTIVATE);
    ShowWindow(g_panel, SW_SHOW);
    ForceForeground(g_panel);
    if (GetForegroundWindow() != g_panel) {
        SwitchToThisWindow(g_panel, TRUE);
        Log(L"menu shown; foreground %s", GetForegroundWindow() == g_panel ? L"after retry" : L"refused (mouse/touch still work)");
    }
    ResetIdleTimer();
}

void OnPowerButton(const wchar_t* source) {
    ULONGLONG now = GetTickCount64();
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const wchar_t* ignore = WTSGetActiveConsoleSessionId() != session ? L"another user's session has the screen" : WakePress(now);
    Log(L"power button pressed (%s)%s%s%s", source, g_locked ? L" while locked" : L"", ignore ? L" - ignored: " : L"",
        ignore ? ignore : L"");
    if (ignore) return;

    if (g_locked) {
        // The menu cannot appear above the lock screen; behave like a normal laptop and sleep.
        if (!g_dryRun) RunAction(ActSleep);
        return;
    }
    if (g_panel && g_holdIdx < 0 && g_doneIdx < 0)
        CloseMenu();  // pressing the button again dismisses the menu
    else
        ShowMenu();
}

LRESULT CALLBACK BackdropProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_POINTERACTIVATE:
        return PA_NOACTIVATE;
    case WM_POINTERDOWN:
        if (g_holdIdx < 0 && g_doneIdx < 0) CloseMenu();
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

POINT PointerClientPos(HWND hwnd, LPARAM lp) {
    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd, &pt);
    return pt;
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintPanel(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (wp == TIMER_HOLD) {
            if (g_holdIdx >= 0 && !HoldInputStillDown()) {
                CancelHold();
                return 0;
            }
            if (g_holdIdx >= 0 && GetTickCount64() - g_holdStart >= kHoldMs) {
                g_doneIdx = g_holdIdx;
                g_holdIdx = -1;
                KillTimer(hwnd, TIMER_HOLD);
                SetTimer(hwnd, TIMER_RUN, 450, nullptr);  // let "Powering off..." etc. show briefly
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == TIMER_RUN) {
            KillTimer(hwnd, TIMER_RUN);
            int act = g_doneIdx;
            CloseMenu();
            RunAction(act);
        } else if (wp == TIMER_IDLE) {
            if (g_holdIdx < 0 && g_doneIdx < 0) CloseMenu();
        }
        return 0;

    case WM_POINTERDOWN: {
        POINT pt = PointerClientPos(hwnd, lp);
        UINT32 id = GET_POINTERID_WPARAM(wp);
        ResetIdleTimer();
        if (!IS_POINTER_PRIMARY_WPARAM(wp) || g_holdIdx >= 0 || g_doneIdx >= 0 || g_pressIdx >= 0) return 0;
        POINTER_INPUT_TYPE type = PT_POINTER;
        GetPointerType(id, &type);
        if (type == PT_MOUSE && !IS_POINTER_FIRSTBUTTON_WPARAM(wp)) return 0;
        // No SetCapture: WM_POINTERDOWN already captures the pointer to this window until it
        // lifts, and calling SetCapture on top of that cancelled the hold it had just started.
        int hit = HitTile(pt);
        if (hit >= 0) {
            g_showFocus = false;
            g_focus = hit;
            if (kActions[hit].instant) {
                g_pressIdx = hit;
                g_pressPointer = id;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                StartHold(hit, id, type, 0);
            }
        } else if (HitCancel(pt)) {
            g_cancelPressed = g_cancelHot = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_POINTERUPDATE: {
        POINT pt = PointerClientPos(hwnd, lp);
        UINT32 id = GET_POINTERID_WPARAM(wp);
        int hit = HitTile(pt);
        bool cancelHot = HitCancel(pt);
        if (g_holdIdx >= 0 && g_holdKey == 0 && id == g_holdPointer && hit != g_holdIdx)
            CancelHold();  // sliding off the tile aborts the hold
        if (g_pressIdx >= 0 && id == g_pressPointer && hit != g_pressIdx)
            CancelPress();  // and a click
        if (hit != g_hover || cancelHot != g_cancelHot) {
            g_hover = hit;
            g_cancelHot = cancelHot;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_POINTERUP: {
        POINT pt = PointerClientPos(hwnd, lp);
        UINT32 id = GET_POINTERID_WPARAM(wp);
        if (g_pressIdx >= 0 && id == g_pressPointer) {
            int idx = g_pressIdx;
            g_pressIdx = -1;
            if (HitTile(pt) == idx) {
                RunInstant(idx);
                return 0;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_holdIdx >= 0 && g_holdKey == 0 && id == g_holdPointer) CancelHold();
        if (g_cancelPressed) {
            g_cancelPressed = false;
            if (HitCancel(pt)) {
                CloseMenu();
                return 0;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_POINTERLEAVE:
    case WM_POINTERCAPTURECHANGED: {
        // Only the pointer doing the hold or press matters; others (the mouse hovering past
        // while a finger holds, say) must not cancel it.
        UINT32 id = GET_POINTERID_WPARAM(wp);
        if (g_holdIdx >= 0 && g_holdKey == 0 && id == g_holdPointer) CancelHold();
        if (g_pressIdx >= 0 && id == g_pressPointer) CancelPress();
        if (msg == WM_POINTERLEAVE && (g_hover != -1 || g_cancelHot)) {
            g_hover = -1;
            g_cancelHot = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        bool repeat = (lp & (1 << 30)) != 0;
        ResetIdleTimer();
        if (wp == VK_ESCAPE) {
            if (g_doneIdx < 0) CloseMenu();
            return 0;
        }
        if (g_doneIdx >= 0 || repeat) return 0;
        if (g_holdIdx < 0) {
            if (wp == VK_LEFT || wp == VK_UP || (wp == VK_TAB && GetKeyState(VK_SHIFT) < 0)) {
                g_focus = (g_focus + ActCount - 1) % ActCount;
                g_showFocus = true;
            } else if (wp == VK_RIGHT || wp == VK_DOWN || wp == VK_TAB) {
                g_focus = (g_focus + 1) % ActCount;
                g_showFocus = true;
            } else if (wp == VK_SPACE || wp == VK_RETURN) {
                g_showFocus = true;
                if (kActions[g_focus].instant) {
                    RunInstant(g_focus);
                    return 0;
                }
                StartHold(g_focus, 0, PT_POINTER, wp);
            } else if (int idx = KeyAction(wp); idx >= 0) {
                if (kActions[idx].instant) {
                    RunInstant(idx);
                    return 0;
                }
                g_focus = idx;
                g_showFocus = true;
                StartHold(idx, 0, PT_POINTER, wp);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_KEYUP:
        if (g_holdIdx >= 0 && g_holdKey == wp) CancelHold();
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            CancelHold();
            CancelPress();
        }
        break;
    case WM_KILLFOCUS:
        CancelHold();
        CancelPress();
        break;

    case WM_DPICHANGED: {
        g_scale = HIWORD(wp) / 96.0f;
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, int(S(kPanelW) + 0.5f), int(S(kPanelH) + 0.5f), SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        if (g_doneIdx < 0) CloseMenu();
        return 0;
    case WM_DESTROY:
        if (hwnd == g_panel) g_panel = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Power button input: a worker thread reads every HID collection that has a
// System Power Down button and posts WM_APP_POWER on each press.

struct HidReader {
    std::wstring path;
    HANDLE file = INVALID_HANDLE_VALUE;
    OVERLAPPED ov = {};
    bool pending = false;
    std::vector<BYTE> report;
    PHIDP_PREPARSED_DATA ppd = nullptr;
    bool down = false;
};

bool HasPowerButton(PHIDP_PREPARSED_DATA ppd, const HIDP_CAPS& caps) {
    USHORT n = caps.NumberInputButtonCaps;
    if (!n) return false;
    std::vector<HIDP_BUTTON_CAPS> bc(n);
    if (HidP_GetButtonCaps(HidP_Input, bc.data(), &n, ppd) != HIDP_STATUS_SUCCESS) return false;
    for (USHORT i = 0; i < n; i++) {
        const HIDP_BUTTON_CAPS& b = bc[i];
        if (b.UsagePage != HID_USAGE_PAGE_GENERIC) continue;
        if (b.IsRange ? (b.Range.UsageMin <= HID_USAGE_GENERIC_SYSCTL_POWER && HID_USAGE_GENERIC_SYSCTL_POWER <= b.Range.UsageMax)
                      : b.NotRange.Usage == HID_USAGE_GENERIC_SYSCTL_POWER)
            return true;
    }
    return false;
}

bool StartRead(HidReader* r) {
    // A read that completes at once still signals the event, so the wait loop sees it.
    r->pending = ReadFile(r->file, r->report.data(), DWORD(r->report.size()), nullptr, &r->ov) || GetLastError() == ERROR_IO_PENDING;
    return r->pending;
}

void FreeReader(HidReader* r) {
    if (r->pending) {
        DWORD got;
        CancelIoEx(r->file, &r->ov);
        GetOverlappedResult(r->file, &r->ov, &got, TRUE);
    }
    if (r->file != INVALID_HANDLE_VALUE) CloseHandle(r->file);
    if (r->ov.hEvent) CloseHandle(r->ov.hEvent);
    if (r->ppd) HidD_FreePreparsedData(r->ppd);
    delete r;
}

void OpenNewReaders(std::vector<HidReader*>& readers) {
    HDEVINFO set = SetupDiGetClassDevsW(&kHidInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
    for (DWORD i = 0; readers.size() < MAXIMUM_WAIT_OBJECTS - 2 && SetupDiEnumDeviceInterfaces(set, nullptr, &kHidInterface, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) continue;
        std::wstring path = detail->DevicePath;
        bool known = false;
        for (const HidReader* r : readers) known = known || _wcsicmp(r->path.c_str(), path.c_str()) == 0;
        if (known) continue;

        // Zero access is enough to read the descriptor, even for collections Windows holds exclusively.
        HANDLE probe = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (probe == INVALID_HANDLE_VALUE) continue;
        PHIDP_PREPARSED_DATA ppd = nullptr;
        HIDP_CAPS caps = {};
        bool wanted = HidD_GetPreparsedData(probe, &ppd) && HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS && HasPowerButton(ppd, caps);
        CloseHandle(probe);
        if (!wanted) {
            if (ppd) HidD_FreePreparsedData(ppd);
            continue;
        }

        auto r = new HidReader;
        r->path = path;
        r->ppd = ppd;
        r->report.resize(caps.InputReportByteLength);
        r->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        r->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (r->file == INVALID_HANDLE_VALUE || !r->ov.hEvent || r->report.empty() || !StartRead(r)) {
            Log(L"cannot read %s (error %lu)", path.c_str(), GetLastError());
            FreeReader(r);
            continue;
        }
        Log(L"listening: %s", path.c_str());
        readers.push_back(r);
    }
    SetupDiDestroyDeviceInfoList(set);
}

// True/false for a report that carries the Generic Desktop buttons, the old state otherwise.
bool PowerDown(const HidReader* r, DWORD len) {
    USAGE usages[32];
    ULONG count = ARRAYSIZE(usages);
    NTSTATUS st = HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, usages, &count, r->ppd,
                                 reinterpret_cast<PCHAR>(const_cast<BYTE*>(r->report.data())), len);
    if (st != HIDP_STATUS_SUCCESS) return r->down;  // a report with a different ID or no such buttons
    for (ULONG k = 0; k < count; k++)
        if (usages[k] == HID_USAGE_GENERIC_SYSCTL_POWER) return true;
    return false;
}

void ServicePress(const wchar_t* source);

DWORD WINAPI HidThread(void*) {
    std::vector<HidReader*> readers;
    ULONGLONG lastRelease = 0;
    OpenNewReaders(readers);
    if (readers.empty()) Log(L"no readable power-button HID collection found");
    for (;;) {
        std::vector<HANDLE> waits = {g_hidStop, g_hidRescan};
        for (const HidReader* r : readers) waits.push_back(r->ov.hEvent);
        DWORD w = WaitForMultipleObjects(DWORD(waits.size()), waits.data(), FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_OBJECT_0 + 1) {
            OpenNewReaders(readers);
            continue;
        }
        size_t idx = size_t(w - WAIT_OBJECT_0 - 2);
        if (w == WAIT_FAILED || idx >= readers.size()) {
            Log(L"HID wait failed: %lu", GetLastError());
            Sleep(1000);
            continue;
        }
        HidReader* r = readers[idx];
        DWORD got = 0;
        bool ok = GetOverlappedResult(r->file, &r->ov, &got, FALSE) != FALSE;
        r->pending = false;
        if (ok) {
            bool down = PowerDown(r, got);
            if (down && !r->down) {
                // One press can arrive through several collections: ACPI0011 reports press and
                // release as they happen, while the button converter replays a whole click at the
                // moment of release. So a press only counts while no other collection holds the
                // button down and not right after a release.
                bool otherDown = false;
                for (const HidReader* o : readers) otherDown = otherDown || (o != r && o->down);
                if (!otherDown && GetTickCount64() - lastRelease >= kEchoMs) {
                    if (g_service) {
                        ServicePress(r->path.c_str());
                    } else {
                        auto path = new std::wstring(r->path);
                        if (!PostMessageW(g_listener, WM_APP_POWER, 0, reinterpret_cast<LPARAM>(path))) delete path;
                    }
                }
            } else if (!down && r->down) {
                lastRelease = GetTickCount64();
            }
            r->down = down;
        }
        if (!ok || !StartRead(r)) {
            Log(L"stopped listening (error %lu): %s", GetLastError(), r->path.c_str());
            FreeReader(r);
            readers.erase(readers.begin() + idx);
        }
    }
    for (HidReader* r : readers) FreeReader(r);
    return 0;
}

// ---------------------------------------------------------------------------
// Tray icon and listener window

void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &nid.hIcon);
    wcscpy_s(nid.szTip, g_dryRun ? L"PowerMenu (dry run)" : L"PowerMenu - power button menu");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    if (nid.hIcon) DestroyIcon(nid.hIcon);
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowTrayMenu(HWND hwnd, int x, int y) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_TRAY_SHOW, L"Show power menu");
    AppendMenuW(m, MF_STRING, ID_TRAY_LOG, L"Open log");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetMenuDefaultItem(m, ID_TRAY_SHOW, FALSE);
    SetForegroundWindow(hwnd);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, hwnd, nullptr);
    DestroyMenu(m);
}

LRESULT CALLBACK ListenerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && msg) {
        AddTrayIcon(hwnd);
        return 0;
    }
    switch (msg) {
    case WM_CREATE: {
        DEV_BROADCAST_DEVICEINTERFACE_W filter = {sizeof(filter), DBT_DEVTYP_DEVICEINTERFACE};
        filter.dbcc_classguid = kHidInterface;
        RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        RegisterPowerSettingNotification(hwnd, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
        WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
        AddTrayIcon(hwnd);
        return 0;
    }
    case WM_APP_POWER: {
        std::unique_ptr<std::wstring> source(reinterpret_cast<std::wstring*>(lp));
        OnPowerButton(source->c_str());
        return 0;
    }
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL && g_hidRescan) SetEvent(g_hidRescan);  // e.g. a keyboard with a power key
        return TRUE;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            auto ps = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
            if (IsEqualGUID(ps->PowerSetting, kConsoleDisplayState) && ps->DataLength >= sizeof(DWORD)) {
                bool on = *reinterpret_cast<const DWORD*>(ps->Data) != 0;  // 0 off, 1 on, 2 dimmed
                if (!on && g_doneIdx < 0) CloseMenu();
                OnDisplayState(on);
            }
        } else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            g_displayOnTick = GetTickCount64();
            if (g_hidRescan) SetEvent(g_hidRescan);
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) g_locked = true;
        if (wp == WTS_SESSION_UNLOCK) g_locked = false;
        if (wp == WTS_SESSION_LOCK) CloseMenu();
        return 0;
    case WM_APP_SHOW:
        ShowMenu();
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowMenu();
            break;
        case WM_CONTEXTMENU:
            ShowTrayMenu(hwnd, GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TRAY_SHOW:
            ShowMenu();
            break;
        case ID_TRAY_LOG:
            if (!g_logPath.empty()) ShellExecuteW(nullptr, L"open", g_logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case ID_TRAY_EXIT:
            if (MessageBoxW(hwnd,
                            L"Without the menu app the power button won't show the menu; with the PowerMenu service "
                            L"installed it just puts the PC to sleep until you sign in again.\n\nExit anyway?",
                            L"PowerMenu", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
                DestroyWindow(hwnd);
            break;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);  // --quit, the installer and session end all land here
        return 0;
    case WM_DESTROY:
        CloseMenu();
        if (g_hidThread) {
            SetEvent(g_hidStop);
            WaitForSingleObject(g_hidThread, 3000);
        }
        WTSUnRegisterSessionNotification(hwnd);
        RemoveTrayIcon(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterClasses() {
    HICON icon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP));
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.hInstance = g_inst;
    wc.hIcon = icon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    wc.lpfnWndProc = ListenerProc;
    wc.lpszClassName = kListenerClass;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = BackdropProc;
    wc.lpszClassName = kBackdropClass;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = PanelProc;
    wc.lpszClassName = kPanelClass;
    RegisterClassExW(&wc);
}

bool HasArg(const wchar_t* name) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i < argc && !found; i++) found = _wcsicmp(argv[i], name) == 0;
    LocalFree(argv);
    return found;
}

// ---------------------------------------------------------------------------
// Service mode: "PowerMenu" runs as LocalSystem from system startup. It starts the menu app in
// each session as the user signs in, and handles the power button itself whenever no menu app
// runs on the console (the sign-in screen, or after Exit): a press then means Sleep, as it
// would without PowerMenu.

bool AgentInSession(DWORD session) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {sizeof(pe)};
    bool found = false;
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe)) {
        DWORD s = 0;
        found = pe.th32ProcessID != GetCurrentProcessId() && _wcsicmp(pe.szExeFile, L"PowerMenu.exe") == 0 &&
                ProcessIdToSessionId(pe.th32ProcessID, &s) && s == session;
    }
    CloseHandle(snap);
    return found;
}

void ServicePress(const wchar_t* source) {
    DWORD console = WTSGetActiveConsoleSessionId();
    if (AgentInSession(console)) {
        Log(L"power button pressed; the menu app in session %lu handles it", console);
        return;
    }
    const wchar_t* ignore = WakePress(GetTickCount64());
    Log(L"power button pressed with no menu app on the console (%s)%s%s", source, ignore ? L" - ignored: " : L" - sleeping",
        ignore ? ignore : L"");
    if (!ignore) RunAction(ActSleep);
}

void LaunchAgent(DWORD session) {
    HANDLE token = nullptr;
    if (session == 0 || !WTSQueryUserToken(session, &token)) return;  // nobody signed in there
    void* env = nullptr;
    CreateEnvironmentBlock(&env, token, FALSE);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION pi;
    if (CreateProcessAsUserW(token, exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env, nullptr, &si, &pi)) {
        Log(L"started the menu app in session %lu", session);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        Log(L"could not start the menu app in session %lu (error %lu)", session, GetLastError());
    }
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(token);
}

void LaunchAgentsForSignedInUsers() {
    WTS_SESSION_INFOW* sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return;
    for (DWORD i = 0; i < count; i++)
        if (!AgentInSession(sessions[i].SessionId)) LaunchAgent(sessions[i].SessionId);
    WTSFreeMemory(sessions);
}

void SetServiceState(DWORD state) {
    SERVICE_STATUS st = {SERVICE_WIN32_OWN_PROCESS, state};
    if (state == SERVICE_RUNNING)
        st.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE | SERVICE_ACCEPT_POWEREVENT;
    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) st.dwWaitHint = 5000;
    SetServiceStatus(g_svcHandle, &st);
}

DWORD WINAPI ServiceHandler(DWORD control, DWORD type, LPVOID data, LPVOID) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        SetServiceState(SERVICE_STOP_PENDING);
        SetEvent(g_svcStop);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
        if (type == WTS_SESSION_LOGON) LaunchAgent(static_cast<WTSSESSION_NOTIFICATION*>(data)->dwSessionId);
        return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT:
        if (type == PBT_POWERSETTINGCHANGE) {
            auto ps = static_cast<const POWERBROADCAST_SETTING*>(data);
            if (IsEqualGUID(ps->PowerSetting, kConsoleDisplayState) && ps->DataLength >= sizeof(DWORD)) {
                DWORD state = *reinterpret_cast<const DWORD*>(ps->Data);
                static bool first = true;
                if (first) Log(L"display state notifications arriving (now %lu)", state);
                first = false;
                OnDisplayState(state != 0);
            }
        } else if (type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND) {
            g_displayOnTick = GetTickCount64();
            if (g_hidRescan) SetEvent(g_hidRescan);
        }
        return NO_ERROR;
    case SERVICE_CONTROL_DEVICEEVENT:
        if (type == DBT_DEVICEARRIVAL && g_hidRescan) SetEvent(g_hidRescan);
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_svcHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceHandler, nullptr);
    if (!g_svcHandle) return;
    SetServiceState(SERVICE_START_PENDING);
    g_svcStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_hidStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_hidRescan = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    RegisterPowerSettingNotification(g_svcHandle, &kConsoleDisplayState, DEVICE_NOTIFY_SERVICE_HANDLE);
    DEV_BROADCAST_DEVICEINTERFACE_W filter = {sizeof(filter), DBT_DEVTYP_DEVICEINTERFACE};
    filter.dbcc_classguid = kHidInterface;
    RegisterDeviceNotificationW(g_svcHandle, &filter, DEVICE_NOTIFY_SERVICE_HANDLE);
    g_hidThread = CreateThread(nullptr, 0, HidThread, nullptr, 0, nullptr);
    SetServiceState(SERVICE_RUNNING);
    Log(L"service started");
    LaunchAgentsForSignedInUsers();

    WaitForSingleObject(g_svcStop, INFINITE);
    SetEvent(g_hidStop);
    WaitForSingleObject(g_hidThread, 3000);
    Log(L"service stopped");
    SetServiceState(SERVICE_STOPPED);
}

int RunService() {
    g_service = true;
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", dir, MAX_PATH);
    if (n && n < MAX_PATH) {
        std::wstring d = std::wstring(dir) + L"\\PowerMenu";
        CreateDirectoryW(d.c_str(), nullptr);
        g_logPath = d + L"\\service.log";
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 512 * 1024)
            DeleteFileW(g_logPath.c_str());
    }
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), ServiceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}

// --install-service / --uninstall-service, run elevated by the installer.
int InstallService() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring bin = L"\"" + std::wstring(exe) + L"\" --service";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return 2;
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, L"PowerMenu", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                   SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (svc)
            ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin.c_str(), nullptr,
                                 nullptr, nullptr, nullptr, nullptr, L"PowerMenu");
    }
    if (!svc) {
        CloseServiceHandle(scm);
        return 3;
    }
    SERVICE_DESCRIPTIONW desc = {const_cast<LPWSTR>(L"Shows the PowerMenu when the power button is pressed, from system startup on.")};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    SC_ACTION restart[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW failure = {86400, nullptr, nullptr, 3, restart};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
    bool ok = StartServiceW(svc, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? 0 : 4;
}

int UninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 2;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (svc) {
        SERVICE_STATUS st;
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        for (int i = 0; i < 50 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; i++) Sleep(100);
        DeleteService(svc);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    g_inst = inst;

    if (HasArg(L"--take-button")) return TakeButton();
    if (HasArg(L"--restore-button")) return RestoreButton();
    if (HasArg(L"--install-service")) return InstallService();
    if (HasArg(L"--uninstall-service")) return UninstallService();
    if (HasArg(L"--service")) return RunService();

    bool show = HasArg(L"--show");
    g_dryRun = HasArg(L"--dry-run");

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // A dry run only hands off to another dry run: a real instance would carry out the actions.
        HWND other = FindWindowW(kListenerClass, g_dryRun ? kDryRunTitle : nullptr);
        if (g_dryRun && !other) return 5;
        if (other) {
            if (HasArg(L"--quit")) PostMessageW(other, WM_CLOSE, 0, 0);
            else if (show) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(other, WM_APP_SHOW, 0, 0);
            }
        }
        return 0;
    }
    if (HasArg(L"--quit")) return 0;

    InitLog();
    Log(L"started%s", g_dryRun ? L" (dry run)" : L"");

    EnableMouseInPointer(TRUE);  // mouse, touch and pen all arrive as WM_POINTER*
    GdiplusStartupInput gsi;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gsi, nullptr);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    RegisterClasses();
    // A hidden top-level window, not message-only: those miss the resume and TaskbarCreated broadcasts.
    g_listener = CreateWindowExW(WS_EX_TOOLWINDOW, kListenerClass, g_dryRun ? kDryRunTitle : L"PowerMenu", WS_POPUP, 0, 0, 0, 0,
                                 nullptr, nullptr, inst, nullptr);
    if (!g_listener) return 1;
    g_hidStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_hidRescan = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_hidThread = CreateThread(nullptr, 0, HidThread, nullptr, 0, nullptr);
    if (show) PostMessageW(g_listener, WM_APP_SHOW, 0, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    Log(L"exited");
    if (mutex) CloseHandle(mutex);
    return 0;
}

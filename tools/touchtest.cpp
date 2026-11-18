// touchtest - inject one real touch contact (or mouse press) into the PowerMenu panel, for testing.
//   touchtest [mouse] <tile> <holdMs> [<slideTile>]
// Tile 0-3 are the options, -1 is the title area, -2 is outside the panel (on its backdrop).
// With <slideTile> the finger/cursor moves there halfway through the hold. Refuses to inject
// unless the panel (or its backdrop, for -2) is the window under each point, and lets go at once
// if the panel goes away, so a stray press never lands on another app. Mouse mode moves the real
// cursor and puts it back afterwards.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static POINT TilePoint(HWND panel, int tile) {
    RECT r;
    GetWindowRect(panel, &r);
    double s = (r.right - r.left) / 706.0;  // layout DIPs: margin 28, tile 152, gap 14, top 96, height 180
    if (tile == -2) return {r.left - 40, (r.top + r.bottom) / 2};
    if (tile < 0) return {LONG(r.left + 60 * s), LONG(r.top + 30 * s)};
    return {LONG(r.left + (28 + tile * 166 + 76) * s), LONG(r.top + 186 * s)};
}

static bool OursAt(HWND panel, POINT p) {
    HWND root = GetAncestor(WindowFromPoint(p), GA_ROOT);
    wchar_t cls[64] = {};
    GetClassNameW(root, cls, 64);
    return root == panel || wcscmp(cls, L"PowerMenu.Backdrop") == 0;
}

static void Button(bool down) {
    bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? (swap ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN) : (swap ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
    SendInput(1, &in, sizeof(in));
}

static int Mouse(HWND panel, POINT start, POINT end, bool slide, ULONGLONG hold) {
    POINT saved;
    GetCursorPos(&saved);
    SetCursorPos(start.x, start.y);
    Sleep(80);
    Button(true);
    ULONGLONG t0 = GetTickCount64();
    bool moved = false;
    while (GetTickCount64() - t0 < hold && IsWindow(panel)) {
        Sleep(30);
        if (slide && !moved && GetTickCount64() - t0 >= hold / 2) {
            SetCursorPos(end.x, end.y);
            moved = true;
        }
    }
    Button(false);
    Sleep(80);
    SetCursorPos(saved.x, saved.y);
    printf("ok\n");
    return 0;
}

static bool Inject(POINTER_TOUCH_INFO& c, POINT p, POINTER_FLAGS flags) {
    c.pointerInfo.ptPixelLocation = p;
    c.rcContact = {p.x - 2, p.y - 2, p.x + 2, p.y + 2};
    c.pointerInfo.pointerFlags = flags;
    if (InjectTouchInput(1, &c)) return true;
    printf("InjectTouchInput failed: %lu\n", GetLastError());
    return false;
}

int wmain(int argc, wchar_t** argv) {
    bool mouse = argc >= 2 && _wcsicmp(argv[1], L"mouse") == 0;
    if (mouse) {
        argc--;
        argv++;
    }
    if (argc < 3) {
        printf("usage: touchtest [mouse] <tile> <holdMs> [<slideTile>]\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND panel = FindWindowW(L"PowerMenu.Panel", nullptr);
    if (!panel || !IsWindowVisible(panel)) {
        printf("no PowerMenu panel\n");
        return 3;
    }
    int tile = _wtoi(argv[1]);
    ULONGLONG hold = _wtoi(argv[2]);
    bool slide = argc >= 4;
    POINT start = TilePoint(panel, tile), end = slide ? TilePoint(panel, _wtoi(argv[3])) : start;
    if (GetAncestor(WindowFromPoint(start), GA_ROOT) != panel || !OursAt(panel, end)) {
        printf("panel is not under (%ld,%ld)/(%ld,%ld); not injecting\n", start.x, start.y, end.x, end.y);
        return 3;
    }
    if (mouse) return Mouse(panel, start, end, slide, hold);
    if (!InitializeTouchInjection(1, TOUCH_FEEDBACK_NONE)) {
        printf("InitializeTouchInjection failed: %lu\n", GetLastError());
        return 1;
    }
    POINTER_TOUCH_INFO c = {};
    c.pointerInfo.pointerType = PT_TOUCH;
    c.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    c.orientation = 90;
    c.pressure = 32000;
    if (!Inject(c, start, POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT)) return 1;
    ULONGLONG t0 = GetTickCount64();
    POINT p = start;
    while (GetTickCount64() - t0 < hold) {
        Sleep(30);
        if (!IsWindow(panel)) break;  // the menu closed: lift right away
        if (slide && GetTickCount64() - t0 >= hold / 2) p = end;
        if (!Inject(c, p, POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT)) return 1;
    }
    if (!Inject(c, p, POINTER_FLAG_UP)) return 1;
    printf("ok\n");
    return 0;
}

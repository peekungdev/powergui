// hidprobe - diagnostics for finding the power button.
//   hidprobe list         HID collections: usages, button caps, whether a user can read them
//   hidprobe watch <sec>  print every report from system-control style collections, via both
//                         direct ReadFile and Raw Input, plus display/power notifications
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <initguid.h>
#include <hidclass.h>
#include <stdio.h>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "user32.lib")

static const GUID kConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

static void Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static std::vector<std::wstring> HidPaths() {
    std::vector<std::wstring> out;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HID, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_HID, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        std::vector<BYTE> buf(need);
        auto det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(*det);
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) out.push_back(det->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

struct Coll {
    std::wstring path;
    HIDP_CAPS caps = {};
    PHIDP_PREPARSED_DATA ppd = nullptr;
};

static bool Describe(const std::wstring& path, Coll& c) {
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = HidD_GetPreparsedData(h, &c.ppd) && HidP_GetCaps(c.ppd, &c.caps) == HIDP_STATUS_SUCCESS;
    CloseHandle(h);
    c.path = path;
    return ok;
}

static bool Interesting(const HIDP_CAPS& caps) {
    return (caps.UsagePage == 0x01 && (caps.Usage == 0x80 || caps.Usage == 0x0D)) || caps.UsagePage == 0x0C;
}

static void PrintButtonCaps(const Coll& c) {
    USHORT n = c.caps.NumberInputButtonCaps;
    std::vector<HIDP_BUTTON_CAPS> bc(n);
    if (n && HidP_GetButtonCaps(HidP_Input, bc.data(), &n, c.ppd) == HIDP_STATUS_SUCCESS) {
        for (USHORT i = 0; i < n; i++) {
            const auto& b = bc[i];
            if (b.IsRange)
                printf("      button  rid=%u page=0x%02X usages 0x%02X-0x%02X%s\n", b.ReportID, b.UsagePage, b.Range.UsageMin, b.Range.UsageMax,
                       b.IsAbsolute ? "" : " (relative)");
            else
                printf("      button  rid=%u page=0x%02X usage 0x%02X\n", b.ReportID, b.UsagePage, b.NotRange.Usage);
        }
    }
    n = c.caps.NumberInputValueCaps;
    std::vector<HIDP_VALUE_CAPS> vc(n);
    if (n && HidP_GetValueCaps(HidP_Input, vc.data(), &n, c.ppd) == HIDP_STATUS_SUCCESS) {
        for (USHORT i = 0; i < n; i++) {
            const auto& v = vc[i];
            printf("      value   rid=%u page=0x%02X usage 0x%02X%s bits=%u x%u\n", v.ReportID, v.UsagePage,
                   v.IsRange ? v.Range.UsageMin : v.NotRange.Usage, v.IsRange ? "+" : "", v.BitSize, v.ReportCount);
        }
    }
}

static int List() {
    for (const auto& p : HidPaths()) {
        Coll c;
        if (!Describe(p, c)) {
            wprintf(L"?? %s (cannot query: %lu)\n", p.c_str(), GetLastError());
            continue;
        }
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD err = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        wprintf(L"%04X/%04X in=%u read=%s  %s\n", c.caps.UsagePage, c.caps.Usage, c.caps.InputReportByteLength,
                err ? (err == ERROR_ACCESS_DENIED ? L"DENIED" : L"ERR") : L"ok", p.c_str());
        if (Interesting(c.caps)) PrintButtonCaps(c);
        HidD_FreePreparsedData(c.ppd);
    }
    printf("\nraw input devices:\n");
    UINT n = 0;
    GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> list(n);
    GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
    for (auto& d : list) {
        if (d.dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info = {sizeof(info)};
        UINT cb = sizeof(info);
        GetRawInputDeviceInfoW(d.hDevice, RIDI_DEVICEINFO, &info, &cb);
        wchar_t name[512] = {};
        cb = 512;
        GetRawInputDeviceInfoW(d.hDevice, RIDI_DEVICENAME, name, &cb);
        if (info.hid.usUsagePage == 0x01 && (info.hid.usUsage == 0x80 || info.hid.usUsage == 0x0D))
            wprintf(L"  %04X/%04X  %s\n", info.hid.usUsagePage, info.hid.usUsage, name);
    }
    return 0;
}

// ---------------------------------------------------------------------------

struct Reader {
    Coll c;
    HANDLE h = INVALID_HANDLE_VALUE;
    OVERLAPPED ov = {};
    std::vector<BYTE> buf;
};

static void PrintUsages(PHIDP_PREPARSED_DATA ppd, BYTE* report, ULONG len) {
    for (USAGE page : {USAGE(0x01), USAGE(0x0C), USAGE(0x07)}) {
        USAGE u[32];
        ULONG n = 32;
        NTSTATUS st = HidP_GetUsages(HidP_Input, page, 0, u, &n, ppd, reinterpret_cast<PCHAR>(report), len);
        if (st == HIDP_STATUS_SUCCESS) {
            printf(" page%02X:[", page);
            for (ULONG i = 0; i < n; i++) printf("%s0x%02X", i ? " " : "", u[i]);
            printf("]");
        } else if (st != HIDP_STATUS_USAGE_NOT_FOUND && st != HIDP_STATUS_INCOMPATIBLE_REPORT_ID) {
            printf(" page%02X:err%08lX", page, ULONG(st));
        }
    }
}

static void Hex(const BYTE* p, ULONG n) {
    for (ULONG i = 0; i < n; i++) printf("%02X", p[i]);
}

static LRESULT CALLBACK WatchProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INPUT: {
        UINT size = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        std::vector<BYTE> b(size);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, b.data(), &size, sizeof(RAWINPUTHEADER)) != size) break;
        auto ri = reinterpret_cast<RAWINPUT*>(b.data());
        if (ri->header.dwType == RIM_TYPEKEYBOARD) {
            const RAWKEYBOARD& k = ri->data.keyboard;
            // Only power/sleep/wake style keys; ordinary typing is not printed.
            bool special = ((k.Flags & RI_KEY_E0) && (k.MakeCode == 0x5E || k.MakeCode == 0x5F || k.MakeCode == 0x63)) ||
                           k.VKey == VK_SLEEP || k.VKey == 0xFF;
            if (special) {
                Stamp();
                printf("RAWKBD make=%s%02X vkey=0x%02X %s\n", (k.Flags & RI_KEY_E0) ? "E0 " : "", k.MakeCode, k.VKey,
                       (k.Flags & RI_KEY_BREAK) ? "up" : "down");
            }
            break;
        }
        if (ri->header.dwType != RIM_TYPEHID) break;
        wchar_t name[512] = {};
        UINT cb = 512;
        GetRawInputDeviceInfoW(ri->header.hDevice, RIDI_DEVICENAME, name, &cb);
        UINT pcb = 0;
        GetRawInputDeviceInfoW(ri->header.hDevice, RIDI_PREPARSEDDATA, nullptr, &pcb);
        std::vector<BYTE> ppd(pcb);
        GetRawInputDeviceInfoW(ri->header.hDevice, RIDI_PREPARSEDDATA, ppd.data(), &pcb);
        for (DWORD i = 0; i < ri->data.hid.dwCount; i++) {
            BYTE* rep = ri->data.hid.bRawData + i * ri->data.hid.dwSizeHid;
            Stamp();
            printf("RAWINPUT ");
            Hex(rep, ri->data.hid.dwSizeHid);
            if (pcb) PrintUsages(reinterpret_cast<PHIDP_PREPARSED_DATA>(ppd.data()), rep, ri->data.hid.dwSizeHid);
            wprintf(L"  %s\n", name);
        }
        fflush(stdout);
        break;
    }
    case WM_POWERBROADCAST:
        Stamp();
        if (wp == PBT_POWERSETTINGCHANGE) {
            auto ps = reinterpret_cast<POWERBROADCAST_SETTING*>(lp);
            printf("POWERSETTING display state -> %lu (0 off, 1 on, 2 dimmed)\n", *reinterpret_cast<DWORD*>(ps->Data));
        } else {
            printf("WM_POWERBROADCAST 0x%llX\n", static_cast<unsigned long long>(wp));
        }
        fflush(stdout);
        return TRUE;
    case WM_QUERYENDSESSION:
        Stamp();
        printf("WM_QUERYENDSESSION\n");
        fflush(stdout);
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int Watch(int seconds) {
    std::vector<Reader*> readers;
    std::vector<HANDLE> events;
    for (const auto& p : HidPaths()) {
        auto r = new Reader;
        if (!Describe(p, r->c) || !Interesting(r->c.caps)) {
            delete r;
            continue;
        }
        r->h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (r->h == INVALID_HANDLE_VALUE) {
            wprintf(L"cannot open %04X/%04X for read (%lu): %s\n", r->c.caps.UsagePage, r->c.caps.Usage, GetLastError(), p.c_str());
            delete r;
            continue;
        }
        r->buf.resize(r->c.caps.InputReportByteLength);
        r->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ReadFile(r->h, r->buf.data(), DWORD(r->buf.size()), nullptr, &r->ov);
        wprintf(L"reading %04X/%04X %s\n", r->c.caps.UsagePage, r->c.caps.Usage, p.c_str());
        readers.push_back(r);
        events.push_back(r->ov.hEvent);
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WatchProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"hidprobe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"hidprobe", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    RAWINPUTDEVICE rid[4] = {
        {0x01, 0x80, RIDEV_INPUTSINK, hwnd},
        {0x01, 0x0D, RIDEV_INPUTSINK, hwnd},
        {0x0C, 0x01, RIDEV_INPUTSINK, hwnd},
        {0x01, 0x06, RIDEV_INPUTSINK, hwnd},
    };
    for (auto& r : rid)
        if (!RegisterRawInputDevices(&r, 1, sizeof(r))) printf("raw input %02X/%02X: register failed %lu\n", r.usUsagePage, r.usUsage, GetLastError());
    RegisterPowerSettingNotification(hwnd, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);

    Stamp();
    printf("watching for %d s - press the power button now\n", seconds);
    fflush(stdout);
    ULONGLONG end = GetTickCount64() + ULONGLONG(seconds) * 1000;
    while (GetTickCount64() < end) {
        DWORD left = DWORD(end - GetTickCount64());
        DWORD w = MsgWaitForMultipleObjects(DWORD(events.size()), events.data(), FALSE, left, QS_ALLINPUT);
        if (w < WAIT_OBJECT_0 + events.size()) {
            Reader* r = readers[w - WAIT_OBJECT_0];
            DWORD got = 0;
            if (GetOverlappedResult(r->h, &r->ov, &got, FALSE)) {
                Stamp();
                printf("READFILE %04X/%04X ", r->c.caps.UsagePage, r->c.caps.Usage);
                Hex(r->buf.data(), got);
                PrintUsages(r->c.ppd, r->buf.data(), got);
                wprintf(L"  %s\n", r->c.path.c_str());
                fflush(stdout);
            }
            ResetEvent(r->ov.hEvent);
            ReadFile(r->h, r->buf.data(), DWORD(r->buf.size()), nullptr, &r->ov);
        } else if (w == WAIT_OBJECT_0 + events.size()) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        }
    }
    Stamp();
    printf("done\n");
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    if (argc >= 2 && !_wcsicmp(argv[1], L"watch")) return Watch(argc >= 3 ? _wtoi(argv[2]) : 60);
    int r = List();
    fflush(stdout);
    return r;
}

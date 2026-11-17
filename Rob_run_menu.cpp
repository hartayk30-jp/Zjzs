// rob_run_menu.cpp — external ESP + aimbot + triggerbot, custom UI, rebindable keyring
// Build: cl /std:c++20 /W4 /permissive- /O2 rob_run_menu.cpp /link user32.lib gdi32.lib
// Run as admin. Menu opens top-left; game stays focused; clicks outside menu pass through.
// Config persists to robrun.ini next to the exe.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <string>

// ---------------- game offsets (FILL FROM CE) ----------------
namespace off {
    constexpr uintptr_t dwLocalPlayer = 0x000000;
    constexpr uintptr_t dwEntityList  = 0x000000;
    constexpr uintptr_t dwViewMatrix  = 0x000000;
    constexpr uintptr_t dwCameraPos   = 0x000000;
    constexpr uintptr_t ent_pos       = 0x000000;
    constexpr uintptr_t ent_team      = 0x000000;
    constexpr uintptr_t ent_health    = 0x000000;
    constexpr uintptr_t ent_bone      = 0x000000;
    constexpr uintptr_t ent_live      = 0x000000;
    constexpr int       ent_stride    = 0x8;
    constexpr int       ent_count     = 512;
    constexpr int       head_bone_idx = 8;
    constexpr int       bone_stride   = 0x30;
}

struct Vec3 { float x, y, z; };

// ---------------- process ----------------
static HANDLE    g_proc = nullptr;
static uintptr_t g_base = 0;
static HWND      g_overlay = nullptr;
static HDC       g_memdc = nullptr;
static HDC       g_screen = nullptr;
static HBITMAP   g_bmp = nullptr;
static int       g_sw = 0, g_sh = 0;

static DWORD find_pid(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do { if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; } }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static uintptr_t find_module_base(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{ sizeof(me) };
    uintptr_t base = 0;
    if (Module32FirstW(snap, &me)) {
        do { if (_wcsicmp(me.szModule, name) == 0) { base = (uintptr_t)me.modBaseAddr; break; } }
        while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

template <typename T>
static bool rpm(uintptr_t addr, T& out) {
    SIZE_T got = 0;
    return ReadProcessMemory(g_proc, (LPCVOID)addr, &out, sizeof(T), &got)
        && got == sizeof(T);
}

// ---------------- math ----------------
static bool w2s(const float m[16], const Vec3& p, const Vec3& cam,
                float sw, float sh, float& sx, float& sy) {
    const float px = p.x - cam.x, py = p.y - cam.y, pz = p.z - cam.z;
    float cw = m[3]*px + m[7]*py + m[11]*pz + m[15];
    if (cw < 0.001f) return false;
    float cx = m[0]*px + m[4]*py + m[8] *pz + m[12];
    float cy = m[1]*px + m[5]*py + m[9] *pz + m[13];
    sx = (cx / cw * 0.5f + 0.5f) * sw;
    sy = (1.0f - (cy / cw * 0.5f + 0.5f)) * sh;
    return true;
}

// ---------------- input ----------------
static void mouse_move(int dx, int dy) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx; in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

static void mouse_click() {
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

static bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// ---------------- config ----------------
enum BindID {
    B_ESP, B_BOXES, B_HEALTH, B_TEAM, B_SNAP,
    B_AIM_TOGGLE, B_AIM_HOLD,
    B_TRIG_TOGGLE, B_TRIG_HOLD,
    B_MENU, B_EXIT,
    B_COUNT
};

static int g_binds[B_COUNT] = {
    VK_F1, VK_F2, VK_F3, VK_F4, VK_F5,
    VK_F6, VK_XBUTTON2,
    VK_F7, VK_XBUTTON1,
    VK_INSERT, VK_END
};

static const wchar_t* g_bind_names[B_COUNT] = {
    L"esp", L"boxes", L"health", L"team", L"snap",
    L"aim_toggle", L"aim_hold",
    L"trig_toggle", L"trig_hold",
    L"menu", L"exit"
};

struct Settings {
    bool  esp        = true;
    bool  boxes      = true;
    bool  health     = true;
    bool  teamChk    = true;
    bool  snaplines  = false;
    bool  aimEnabled = true;
    bool  trigEnabled= true;
    float aimSmooth  = 0.35f;
    int   aimMaxStep = 40;
    int   aimFovPx   = 220;
    int   trigFovPx  = 6;
    int   trigDelayMs= 25;
};
static Settings g_set;
static bool g_menuOpen = true;

// ---------------- config persistence ----------------
static const wchar_t* CFG_PATH = L"robrun.ini";

static void save_config() {
    FILE* f = nullptr;
    _wfopen_s(&f, CFG_PATH, L"w");
    if (!f) return;
    for (int i = 0; i < B_COUNT; ++i)
        fwprintf(f, L"%ls=%d\n", g_bind_names[i], g_binds[i]);
    fwprintf(f, L"smooth=%.3f\n", g_set.aimSmooth);
    fwprintf(f, L"maxstep=%d\n", g_set.aimMaxStep);
    fwprintf(f, L"fov=%d\n", g_set.aimFovPx);
    fwprintf(f, L"trigfov=%d\n", g_set.trigFovPx);
    fwprintf(f, L"trigdelay=%d\n", g_set.trigDelayMs);
    fclose(f);
}

static void load_config() {
    FILE* f = nullptr;
    _wfopen_s(&f, CFG_PATH, L"r");
    if (!f) return;
    wchar_t key[64], val[64];
    while (fwscanf(f, L" %63[^=]=%63ls", key, val) == 2) {
        for (int i = 0; i < B_COUNT; ++i) {
            if (_wcsicmp(key, g_bind_names[i]) == 0) { g_binds[i] = _wtoi(val); goto next; }
        }
        if      (_wcsicmp(key, L"smooth")    == 0) g_set.aimSmooth  = (float)_wtof(val);
        else if (_wcsicmp(key, L"maxstep")   == 0) g_set.aimMaxStep = _wtoi(val);
        else if (_wcsicmp(key, L"fov")       == 0) g_set.aimFovPx   = _wtoi(val);
        else if (_wcsicmp(key, L"trigfov")   == 0) g_set.trigFovPx  = _wtoi(val);
        else if (_wcsicmp(key, L"trigdelay") == 0) g_set.trigDelayMs= _wtoi(val);
    next:;
    }
    fclose(f);
}

// ---------------- key names ----------------
static const wchar_t* key_name(int vk) {
    static wchar_t buf[32];
    if (vk >= 'A' && vk <= 'Z') { buf[0] = (wchar_t)vk; buf[1] = 0; return buf; }
    if (vk >= '0' && vk <= '9') { buf[0] = (wchar_t)vk; buf[1] = 0; return buf; }
    if (vk >= VK_F1 && vk <= VK_F12) { swprintf_s(buf, L"F%d", vk - VK_F1 + 1); return buf; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { swprintf_s(buf, L"NUM%d", vk - VK_NUMPAD0); return buf; }
    switch (vk) {
        case VK_LBUTTON:  return L"MOUSE1";
        case VK_RBUTTON:  return L"MOUSE2";
        case VK_MBUTTON:  return L"MOUSE3";
        case VK_XBUTTON1: return L"MOUSE4";
        case VK_XBUTTON2: return L"MOUSE5";
        case VK_INSERT:   return L"INSERT";
        case VK_DELETE:   return L"DELETE";
        case VK_HOME:     return L"HOME";
        case VK_END:      return L"END";
        case VK_PRIOR:    return L"PGUP";
        case VK_NEXT:     return L"PGDN";
        case VK_SPACE:    return L"SPACE";
        case VK_TAB:      return L"TAB";
        case VK_RETURN:   return L"ENTER";
        case VK_ESCAPE:   return L"ESC";
        case VK_SHIFT:    return L"SHIFT";
        case VK_CONTROL:  return L"CTRL";
        case VK_MENU:     return L"ALT";
        case VK_UP:       return L"UP";
        case VK_DOWN:     return L"DOWN";
        case VK_LEFT:     return L"LEFT";
        case VK_RIGHT:    return L"RIGHT";
        case VK_OEM_1:    return L";";
        case VK_OEM_2:    return L"/";
        case VK_OEM_3:    return L"`";
        case VK_OEM_4:    return L"[";
        case VK_OEM_5:    return L"\\";
        case VK_OEM_6:    return L"]";
        case VK_OEM_7:    return L"'";
        case VK_OEM_PLUS: return L"=";
        case VK_OEM_MINUS:return L"-";
        case VK_OEM_COMMA:return L",";
        case VK_OEM_PERIOD:return L".";
    }
    swprintf_s(buf, L"0x%02X", vk);
    return buf;
}

// ---------------- target cache ----------------
struct Target {
    uintptr_t ent;
    Vec3      head;
    float     sx, sy;
    float     dist2;
};
static std::vector<Target> g_targets;
static Vec3  g_cam{};
static DWORD g_lastShot = 0;
static POINT g_cursor{};

static void gather_targets() {
    g_targets.clear();
    float m[16];
    if (!rpm(g_base + off::dwViewMatrix, m)) return;
    rpm(g_base + off::dwCameraPos, g_cam);

    uintptr_t list = 0;
    if (!rpm(g_base + off::dwEntityList, list) || !list) return;

    uintptr_t local = 0;
    rpm(g_base + off::dwLocalPlayer, local);
    int myTeam = -1;
    if (local) rpm(local + off::ent_team, myTeam);

    const float cx = g_sw * 0.5f, cy = g_sh * 0.5f;

    for (int i = 0; i < off::ent_count; ++i) {
        uintptr_t ent = 0;
        if (!rpm(list + i * off::ent_stride, ent) || !ent) continue;
        if (ent == local) continue;

        int alive = 0;
        if (!rpm(ent + off::ent_live, alive) || !alive) continue;

        int team = 0;
        rpm(ent + off::ent_team, team);
        if (g_set.teamChk && myTeam != -1 && team == myTeam) continue;

        Vec3 head{};
        if (!rpm(ent + off::ent_bone + off::head_bone_idx * off::bone_stride, head)) continue;

        float hx, hy;
        if (!w2s(m, head, g_cam, (float)g_sw, (float)g_sh, hx, hy)) continue;

        const float dx = hx - cx, dy = hy - cy;
        g_targets.push_back({ ent, head, hx, hy, dx*dx + dy*dy });
    }
    std::sort(g_targets.begin(), g_targets.end(),
        [](const Target& a, const Target& b) { return a.dist2 < b.dist2; });
}

// ---------------- ESP ----------------
static void draw_esp(HDC hdc, const float m[16]) {
    HPEN penBox = CreatePen(PS_SOLID, 1, RGB(0, 255, 128));
    HPEN penFov = CreatePen(PS_DOT,  1, RGB(0, 200, 255));
    HGDIOBJ oldPen = SelectObject(hdc, penBox);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    if (g_set.aimEnabled || g_set.trigEnabled) {
        SelectObject(hdc, penFov);
        Ellipse(hdc, g_sw/2 - g_set.aimFovPx, g_sh/2 - g_set.aimFovPx,
                     g_sw/2 + g_set.aimFovPx, g_sh/2 + g_set.aimFovPx);
        SelectObject(hdc, penBox);
    }

    for (const auto& t : g_targets) {
        Vec3 feet{};
        if (!rpm(t.ent + off::ent_pos, feet)) continue;
        float fx, fy;
        if (!w2s(m, feet, g_cam, (float)g_sw, (float)g_sh, fx, fy)) continue;

        const int boxH = (int)(fy - t.sy);
        if (boxH <= 2 || boxH > g_sh) continue;
        const int boxW = boxH / 2;
        const int bx = (int)t.sx - boxW / 2;
        const int by = (int)t.sy;

        if (g_set.boxes) Rectangle(hdc, bx, by, bx + boxW, by + boxH);
        if (g_set.health) {
            int hp = 0;
            if (rpm(t.ent + off::ent_health, hp)) {
                char buf[16]; snprintf(buf, sizeof buf, "%d", hp);
                TextOutA(hdc, bx + boxW + 4, by, buf, (int)strlen(buf));
            }
        }
        if (g_set.snaplines) {
            MoveToEx(hdc, g_sw / 2, g_sh, nullptr);
            LineTo(hdc, (int)t.sx, (int)t.sy);
        }
    }
    SelectObject(hdc, oldPen);
    DeleteObject(penBox);
    DeleteObject(penFov);
}

// ---------------- UI: fonts, colors, helpers ----------------
static HFONT g_fontBody  = nullptr;
static HFONT g_fontHead  = nullptr;
static HFONT g_fontSmall = nullptr;
static HFONT g_fontBold  = nullptr;

static const COLORREF C_BG     = RGB(24, 24, 28);
static const COLORREF C_CARD   = RGB(32, 32, 38);
static const COLORREF C_BORDER = RGB(45, 45, 52);
static const COLORREF C_ACCENT = RGB(0, 224, 138);
static const COLORREF C_TEXT   = RGB(232, 232, 238);
static const COLORREF C_DIM    = RGB(140, 140, 150);
static const COLORREF C_MUTED  = RGB(90, 90, 100);
static const COLORREF C_HOVER  = RGB(38, 38, 44);

static void fill_round(HDC hdc, RECT r, int rad, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(hdc, ob); SelectObject(hdc, op);
    DeleteObject(br); DeleteObject(pen);
}

static void stroke_round(HDC hdc, RECT r, int rad, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ op = SelectObject(hdc, pen);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}

static void txt(HDC hdc, HFONT f, const wchar_t* s, RECT r, COLORREF c, UINT fmt) {
    HGDIOBJ of = SelectObject(hdc, f);
    SetTextColor(hdc, c);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, s, -1, &r, fmt);
    SelectObject(hdc, of);
}

static bool pt_in(RECT r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

static void draw_checkbox(HDC hdc, RECT r, bool checked) {
    fill_round(hdc, r, 4, checked ? C_ACCENT : RGB(30, 30, 34));
    stroke_round(hdc, r, 4, checked ? C_ACCENT : RGB(60, 60, 68));
    if (checked) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(20, 20, 24));
        HGDIOBJ op = SelectObject(hdc, pen);
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        MoveToEx(hdc, cx - 4, cy, nullptr);
        LineTo(hdc, cx - 1, cy + 3);
        LineTo(hdc, cx + 4, cy - 3);
        SelectObject(hdc, op);
        DeleteObject(pen);
    }
}

static void draw_chip(HDC hdc, RECT r, const wchar_t* text, bool hovered, bool rebinding) {
    COLORREF bg = rebinding ? RGB(20, 44, 34) : (hovered ? RGB(44, 44, 52) : RGB(28, 28, 34));
    COLORREF bd = rebinding ? C_ACCENT : (hovered ? RGB(80, 80, 90) : RGB(52, 52, 60));
    COLORREF tc = rebinding ? C_ACCENT : (hovered ? C_TEXT : RGB(200, 200, 210));
    fill_round(hdc, r, 4, bg);
    stroke_round(hdc, r, 4, bd);
    txt(hdc, g_fontSmall, text, r, tc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---------------- UI: click boxes + state ----------------
enum ClickType { CT_TOGGLE, CT_REBIND, CT_SMOOTH_UP, CT_SMOOTH_DN, CT_FOV_UP, CT_FOV_DN };
struct ClickBox {
    RECT      r;
    ClickType type;
    int       bind;
};
static std::vector<ClickBox> g_clickBoxes;
static int  g_rebindTarget = -1;
static bool g_suppressLmb  = false;
static bool g_keyPrev[256] = {};

// ---------------- menu layout constants ----------------
static const int MENU_X  = 40;
static const int MENU_Y  = 40;
static const int MENU_W  = 560;
static const int MENU_H  = 640;
static const int MENU_PAD= 20;
static const int CARD_PAD= 12;
static const int ROW_H   = 30;
static const int CHIP_W  = 88;
static const int CHIP_H  = 22;
static const int CHK_SZ  = 16;

// ---------------- menu draw ----------------
static void draw_row(HDC hdc, int x, int y, int w,
                     const wchar_t* label, bool* toggle, int bindId) {
    RECT row = { x, y, x + w, y + ROW_H };
    if (pt_in(row, g_cursor) && g_rebindTarget != bindId)
        fill_round(hdc, row, 4, C_HOVER);

    int chkY = y + (ROW_H - CHK_SZ) / 2;
    int chipY = y + (ROW_H - CHIP_H) / 2;
    int chipX = x + w - CHIP_W;

    if (toggle) {
        RECT cb = { x + 4, chkY, x + 4 + CHK_SZ, chkY + CHK_SZ };
        draw_checkbox(hdc, cb, *toggle);
        g_clickBoxes.push_back({ cb, CT_TOGGLE, bindId });
    }

    int lblX = x + 4 + CHK_SZ + 10;
    RECT lr = { lblX, y, chipX - 8, y + ROW_H };
    txt(hdc, g_fontBody, label, lr,
        toggle ? C_TEXT : RGB(160, 160, 172),
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT cr = { chipX, chipY, chipX + CHIP_W, chipY + CHIP_H };
    bool hov = pt_in(cr, g_cursor);
    bool reb = (g_rebindTarget == bindId);
    draw_chip(hdc, cr, reb ? L"press a key..." : key_name(g_binds[bindId]), hov, reb);
    g_clickBoxes.push_back({ cr, CT_REBIND, bindId });
}

static void draw_slider_float(HDC hdc, int x, int y, int w, const wchar_t* label,
                              float* v, ClickType up, ClickType dn) {
    RECT row = { x, y, x + w, y + ROW_H };
    int btnW = 22, valW = 90;
    int rightX = x + w;
    RECT bMinus = { rightX - valW - btnW - 4, y + 4, rightX - valW - 4, y + ROW_H - 4 };
    RECT vRect  = { rightX - valW,             y + 4, rightX,            y + ROW_H - 4 };
    RECT bPlus  = { rightX - btnW,             y + 4, rightX,            y + ROW_H - 4 };

    // reorder: [−] [value] [+]
    bMinus = { rightX - btnW * 2 - valW - 8, y + 4, rightX - valW - btnW - 8, y + ROW_H - 4 };
    vRect  = { rightX - valW - btnW - 8 + btnW + 4, y + 4, rightX - btnW - 4, y + ROW_H - 4 };
    bPlus  = { rightX - btnW, y + 4, rightX, y + ROW_H - 4 };

    bool hM = pt_in(bMinus, g_cursor), hP = pt_in(bPlus, g_cursor);
    fill_round(hdc, bMinus, 4, hM ? C_HOVER : C_CARD);
    stroke_round(hdc, bMinus, 4, C_BORDER);
    txt(hdc, g_fontBody, L"\x2212", bMinus, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    fill_round(hdc, bPlus, 4, hP ? C_HOVER : C_CARD);
    stroke_round(hdc, bPlus, 4, C_BORDER);
    txt(hdc, g_fontBody, L"+", bPlus, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    wchar_t buf[32]; swprintf_s(buf, L"%.2f", *v);
    txt(hdc, g_fontBody, buf, vRect, C_ACCENT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT lr = { x + 4 + CHK_SZ + 10, y, rightX - valW - btnW * 2 - 20, y + ROW_H };
    txt(hdc, g_fontBody, label, lr, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    g_clickBoxes.push_back({ bMinus, dn, 0 });
    g_clickBoxes.push_back({ bPlus,  up, 0 });
}

static void draw_slider_int(HDC hdc, int x, int y, int w, const wchar_t* label,
                            int* v, ClickType up, ClickType dn) {
    RECT row = { x, y, x + w, y + ROW_H };
    int btnW = 22, valW = 90;
    int rightX = x + w;
    RECT bMinus = { rightX - btnW * 2 - valW - 8, y + 4, rightX - valW - btnW - 8, y + ROW_H - 4 };
    RECT vRect  = { rightX - valW - btnW - 8 + btnW + 4, y + 4, rightX - btnW - 4, y + ROW_H - 4 };
    RECT bPlus  = { rightX - btnW, y + 4, rightX, y + ROW_H - 4 };

    bool hM = pt_in(bMinus, g_cursor), hP = pt_in(bPlus, g_cursor);
    fill_round(hdc, bMinus, 4, hM ? C_HOVER : C_CARD);
    stroke_round(hdc, bMinus, 4, C_BORDER);
    txt(hdc, g_fontBody, L"\x2212", bMinus, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    fill_round(hdc, bPlus, 4, hP ? C_HOVER : C_CARD);
    stroke_round(hdc, bPlus, 4, C_BORDER);
    txt(hdc, g_fontBody, L"+", bPlus, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    wchar_t buf[32]; swprintf_s(buf, L"%dpx", *v);
    txt(hdc, g_fontBody, buf, vRect, C_ACCENT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT lr = { x + 4 + CHK_SZ + 10, y, rightX - valW - btnW * 2 - 20, y + ROW_H };
    txt(hdc, g_fontBody, label, lr, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    g_clickBoxes.push_back({ bMinus, dn, 0 });
    g_clickBoxes.push_back({ bPlus,  up, 0 });
}

static void draw_menu(HDC hdc) {
    g_clickBoxes.clear();

    RECT panel = { MENU_X, MENU_Y, MENU_X + MENU_W, MENU_Y + MENU_H };
    fill_round(hdc, panel, 12, C_BG);
    stroke_round(hdc, panel, 12, C_BORDER);

    int cx = MENU_X + MENU_PAD;
    int cy = MENU_Y + MENU_PAD;
    int cardW = MENU_W - MENU_PAD * 2;

    // header
    RECT hTitle = { cx, cy, cx + 90, cy + 28 };
    txt(hdc, g_fontHead, L"OLLIE", hTitle, C_ACCENT, DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT hSub = { cx + 78, cy + 5, cx + cardW, cy + 28 };
    txt(hdc, g_fontBody, L"// ROB RUN", hSub, C_MUTED, DT_LEFT | DT_TOP | DT_SINGLELINE);

    // close hint on right
    RECT hHint = { cx + cardW - 160, cy + 6, cx + cardW, cy + 24 };
    txt(hdc, g_fontSmall, L"click chip to rebind", hHint, C_MUTED, DT_RIGHT | DT_TOP | DT_SINGLELINE);

    cy += 42;
    {
        HPEN dpen = CreatePen(PS_SOLID, 1, C_BORDER);
        HGDIOBJ opd = SelectObject(hdc, dpen);
        MoveToEx(hdc, cx, cy, nullptr);
        LineTo(hdc, MENU_X + MENU_W - MENU_PAD, cy);
        SelectObject(hdc, opd);
        DeleteObject(dpen);
    }
    cy += 14;

    // section label helper
    auto section = [&](const wchar_t* name) {
        RECT lr = { cx, cy, cx + cardW, cy + 18 };
        txt(hdc, g_fontBold, name, lr, C_DIM, DT_LEFT | DT_TOP | DT_SINGLELINE);
        cy += 22;
    };

    // ============ ESP ============
    section(L"ESP");
    {
        RECT card = { cx, cy, cx + cardW, cy + CARD_PAD * 2 + ROW_H * 5 };
        fill_round(hdc, card, 8, C_CARD);
        stroke_round(hdc, card, 8, C_BORDER);

        int rx = cx + CARD_PAD, ry = cy + CARD_PAD, rw = cardW - CARD_PAD * 2;
        draw_row(hdc, rx, ry, rw, L"ESP Master",  &g_set.esp,       B_ESP);    ry += ROW_H;
        draw_row(hdc, rx, ry, rw, L"Boxes",       &g_set.boxes,     B_BOXES);  ry += ROW_H;
        draw_row(hdc, rx, ry, rw, L"Health Bar",  &g_set.health,    B_HEALTH); ry += ROW_H;
        draw_row(hdc, rx, ry, rw, L"Team Check",  &g_set.teamChk,   B_TEAM);   ry += ROW_H;
        draw_row(hdc, rx, ry, rw, L"Snaplines",   &g_set.snaplines, B_SNAP);
        cy = card.bottom + 16;
    }

    // ============ COMBAT ============
    section(L"COMBAT");
    {
        RECT card = { cx, cy, cx + cardW, cy + CARD_PAD * 2 + ROW_H * 6 };
        fill_round(hdc, card, 8, C_CARD);
        stroke_round(hdc, card, 8, C_BORDER);

        int rx = cx + CARD_PAD, ry = cy + CARD_PAD, rw = cardW - CARD_PAD * 2;
        draw_row(hdc, rx, ry, rw, L"Aimbot",         &g_set.aimEnabled,  B_AIM_TOGGLE);  ry += ROW_H;
        draw_row(hdc, rx, ry, rw, L"  Hold",         nullptr,            B_AIM_HOLD);    ry += ROW_H;
        draw_row(hdc, rx, ry, rw, L"Triggerbot",     &g_set.trigEnabled, B_TRIG_TOGGLE); ry += ROW_H;
        draw_row(hdc, rx, ry, rw, L"  Hold",         nullptr,            B_TRIG_HOLD);   ry += ROW_H;
        draw_slider_float(hdc, rx, ry, rw, L"Smoothing",   &g_set.aimSmooth, CT_SMOOTH_UP, CT_SMOOTH_DN); ry += ROW_H;
        draw_slider_int  (hdc, rx, ry, rw, L"FOV Radius",  &g_set.aimFovPx,  CT_FOV_UP,    CT_FOV_DN);
        cy = card.bottom + 16;
    }

    // ============ SYSTEM ============
    section(L"SYSTEM");
    {
        RECT card = { cx, cy, cx + cardW, cy + CARD_PAD * 2 + ROW_H * 2 };
        fill_round(hdc, card, 8, C_CARD);
        stroke_round(hdc, card, 8, C_BORDER);

        int rx = cx + CARD_PAD, ry = cy + CARD_PAD, rw = cardW - CARD_PAD * 2;
        draw_row(hdc, rx, ry, rw, L"Toggle Menu",  nullptr, B_MENU); ry += ROW_H;
        draw_row(hdc, rx, ry, rw, L"Panic Exit",   nullptr, B_EXIT);
        cy = card.bottom + 14;
    }

    // footer
    RECT foot = { cx, cy, cx + cardW, cy + 16 };
    txt(hdc, g_fontSmall, L"config autosaves to robrun.ini", foot, C_MUTED,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
}

// ---------------- input processing ----------------
static void set_menu_open(bool open) {
    g_menuOpen = open;
    if (!open) g_rebindTarget = -1;
    InvalidateRect(g_overlay, nullptr, FALSE);
}

static void handle_toggle(int bindId) {
    switch (bindId) {
        case B_ESP:         g_set.esp         = !g_set.esp;         break;
        case B_BOXES:       g_set.boxes       = !g_set.boxes;       break;
        case B_HEALTH:      g_set.health      = !g_set.health;      break;
        case B_TEAM:        g_set.teamChk     = !g_set.teamChk;     break;
        case B_SNAP:        g_set.snaplines   = !g_set.snaplines;   break;
        case B_AIM_TOGGLE:  g_set.aimEnabled  = !g_set.aimEnabled;  break;
        case B_TRIG_TOGGLE: g_set.trigEnabled = !g_set.trigEnabled; break;
        case B_MENU:        set_menu_open(!g_menuOpen);             break;
        case B_EXIT:        PostQuitMessage(0);                     break;
    }
    InvalidateRect(g_overlay, nullptr, FALSE);
}

static void process_input() {
    if (g_rebindTarget >= 0) {
        for (int vk = 1; vk < 256; ++vk) {
            if (vk == VK_LBUTTON && g_suppressLmb) {
                bool cur = key_down(vk);
                if (!cur) g_suppressLmb = false;
                g_keyPrev[vk] = cur;
                continue;
            }
            bool cur = key_down(vk);
            if (cur && !g_keyPrev[vk]) {
                g_binds[g_rebindTarget] = vk;
                g_rebindTarget = -1;
                save_config();
                InvalidateRect(g_overlay, nullptr, FALSE);
                g_keyPrev[vk] = cur;
                return;
            }
            g_keyPrev[vk] = cur;
        }
        return;
    }

    for (int i = 0; i < B_COUNT; ++i) {
        if (i == B_AIM_HOLD || i == B_TRIG_HOLD) continue;
        int vk = g_binds[i];
        bool cur = key_down(vk);
        if (cur && !g_keyPrev[vk]) handle_toggle(i);
        g_keyPrev[vk] = cur;
    }
}

static void handle_click(const ClickBox& cb) {
    switch (cb.type) {
        case CT_TOGGLE:
            handle_toggle(cb.bind);
            break;
        case CT_REBIND:
            g_rebindTarget = cb.bind;
            g_suppressLmb  = true;   // ignore the LMB that just started rebind
            memset(g_keyPrev, 0, sizeof g_keyPrev);
            break;
        case CT_SMOOTH_UP: g_set.aimSmooth = std::min(1.0f, g_set.aimSmooth + 0.05f); save_config(); break;
        case CT_SMOOTH_DN: g_set.aimSmooth = std::max(0.05f, g_set.aimSmooth - 0.05f); save_config(); break;
        case CT_FOV_UP:    g_set.aimFovPx   = std::min(2000, g_set.aimFovPx   + 10);   save_config(); break;
        case CT_FOV_DN:    g_set.aimFovPx   = std::max(40,   g_set.aimFovPx   - 10);   save_config(); break;
    }
}

// ---------------- aim / trigger ----------------
static void run_aimbot() {
    if (!g_set.aimEnabled) return;
    if (!key_down(g_binds[B_AIM_HOLD])) return;
    if (g_targets.empty()) return;
    const Target& best = g_targets.front();
    float fov2 = (float)(g_set.aimFovPx * g_set.aimFovPx);
    if (best.dist2 > fov2) return;

    float cx = g_sw * 0.5f, cy = g_sh * 0.5f;
    float dx = (best.sx - cx) * g_set.aimSmooth;
    float dy = (best.sy - cy) * g_set.aimSmooth;
    int ms = g_set.aimMaxStep;
    if (dx >  ms) dx =  ms; if (dx < -ms) dx = -ms;
    if (dy >  ms) dy =  ms; if (dy < -ms) dy = -ms;
    int ix = (int)std::lround(dx), iy = (int)std::lround(dy);
    if (ix || iy) mouse_move(ix, iy);
}

static void run_triggerbot() {
    if (!g_set.trigEnabled) return;
    if (!key_down(g_binds[B_TRIG_HOLD])) return;
    if (g_targets.empty()) return;
    DWORD now = GetTickCount();
    if (now - g_lastShot < (DWORD)g_set.trigDelayMs) return;
    const Target& best = g_targets.front();
    float tol2 = (float)(g_set.trigFovPx * g_set.trigFovPx);
    if (best.dist2 > tol2) return;
    mouse_click();
    g_lastShot = now;
}

// ---------------- wndproc ----------------
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: {
            if (!g_menuOpen) return HTTRANSPARENT;
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            RECT m = { MENU_X, MENU_Y, MENU_X + MENU_W, MENU_Y + MENU_H };
            return pt_in(m, p) ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_LBUTTONDOWN: {
            if (!g_menuOpen) return 0;
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            // If a rebind is active, capture LMB as the new bind
            if (g_rebindTarget >= 0 && !g_suppressLmb) {
                g_binds[g_rebindTarget] = VK_LBUTTON;
                g_rebindTarget = -1;
                save_config();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            for (const auto& cb : g_clickBoxes) {
                if (pt_in(cb.r, p)) {
                    handle_click(cb);
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
            }
            return 0;
        }
        case WM_XBUTTONDOWN: {
            if (g_rebindTarget >= 0) {
                WORD btn = GET_XBUTTON_WPARAM(wp);
                g_binds[g_rebindTarget] = (btn == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
                g_rebindTarget = -1;
                save_config();
                InvalidateRect(h, nullptr, FALSE);
            }
            return TRUE;
        }
        case WM_KEYDOWN: {
            if (g_rebindTarget >= 0) {
                g_binds[g_rebindTarget] = (int)wp;
                g_rebindTarget = -1;
                save_config();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------- main ----------------
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    load_config();

    DWORD pid = find_pid(L"RobRun.exe");
    if (!pid) { MessageBoxW(nullptr, L"game not running", L"ollie", MB_OK); return 1; }

    g_proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!g_proc) { MessageBoxW(nullptr, L"OpenProcess failed — run as admin", L"ollie", MB_OK); return 1; }

    g_base = find_module_base(pid, L"GameAssembly.dll");
    if (!g_base) { MessageBoxW(nullptr, L"module base not found", L"ollie", MB_OK); return 1; }

    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);
    g_targets.reserve((size_t)off::ent_count);

    g_fontBody  = CreateFontW(-14, 0,0,0, FW_NORMAL,   FALSE,FALSE,FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    g_fontHead  = CreateFontW(-20, 0,0,0, FW_BOLD,     FALSE,FALSE,FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    g_fontSmall = CreateFontW(-12, 0,0,0, FW_NORMAL,   FALSE,FALSE,FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    g_fontBold  = CreateFontW(-12, 0,0,0, FW_SEMIBOLD, FALSE,FALSE,FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hi;
    wc.lpszClassName = L"RobRunMenu";
    RegisterClassExW(&wc);

    g_overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"RobRunMenu", L"", WS_POPUP,
        0, 0, g_sw, g_sh, nullptr, nullptr, hi, nullptr);
    SetLayeredWindowAttributes(g_overlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(g_overlay, SW_SHOW);

    g_screen = GetDC(nullptr);
    g_memdc  = CreateCompatibleDC(g_screen);
    g_bmp    = CreateCompatibleBitmap(g_screen, g_sw, g_sh);
    HGDIOBJ oldBmp = SelectObject(g_memdc, g_bmp);

    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // update cursor pos for hover states
        POINT p; GetCursorPos(&p); ScreenToClient(g_overlay, &p);
        g_cursor = p;

        process_input();

        float m[16]{};
        bool haveMatrix = rpm(g_base + off::dwViewMatrix, m);
        if (haveMatrix) gather_targets();

        PatBlt(g_memdc, 0, 0, g_sw, g_sh, BLACKNESS);
        if (haveMatrix && g_set.esp) draw_esp(g_memdc, m);
        if (g_menuOpen) draw_menu(g_memdc);

        if (haveMatrix) {
            run_aimbot();
            run_triggerbot();
        }

        BitBlt(g_screen, 0, 0, g_sw, g_sh, g_memdc, 0, 0, SRCCOPY);
        Sleep(4);
    }

done:
    save_config();
    SelectObject(g_memdc, oldBmp);
    DeleteObject(g_bmp);
    DeleteDC(g_memdc);
    ReleaseDC(nullptr, g_screen);
    if (g_proc) CloseHandle(g_proc);
    if (g_fontBody)  DeleteObject(g_fontBody);
    if (g_fontHead)  DeleteObject(g_fontHead);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    if (g_fontBold)  DeleteObject(g_fontBold);
    return 0;
}

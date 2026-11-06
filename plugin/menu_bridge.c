/* Camera Mods <-> FH2 Mod Platform bridge (CameraModsMenu.dll, goes into fh2-mods).
   Registers Camera Mods with the platform (Installed Mods page, update check) and gives every
   action a key row and a controller row under Pause > Settings > MODS, where players rebind them.
   The platform needs API 9 (platform 2.8.0). Bindings are saved in <azimuth.exe folder>\CameraMods\keybinds.ini.
   The real work is done in CameraMods.dll (Extra Options plugin), which exports CM_Attach / CM_Trigger. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "fh2mod.h"
#include "cm_actions.h"

#define BRIDGE_VERSION "1.3.0"
#ifndef CM_PLATFORM_ROWS
#define CM_PLATFORM_ROWS 0
#endif

typedef struct { unsigned vk, mods, chord; } Bind;

static const FH2ModApi *g_api;
static FH2Mod *g_mod;
static void (*volatile p_trigger)(int);
static wchar_t g_dir[MAX_PATH + 16];
static wchar_t g_ini[MAX_PATH + 40];
static Bind g_bind[CM_ACT_COUNT];
static FH2Hotkey g_hk[CM_ACT_COUNT];
static char g_text[16384];

/* Any-key support: the platform's own rows only offer F1-F12 / Ctrl+F1-F12, so the bridge also
   polls the keyboard itself and has a page (Mods > Camera Mods > Keys) where any key can be learned. */
typedef SHORT (WINAPI *KeyFn)(int);
typedef HWND (WINAPI *FgFn)(void);
typedef DWORD (WINAPI *PidFn)(HWND, LPDWORD);
typedef BOOL (WINAPI *BeepFn)(UINT);
static KeyFn k_key;
static FgFn k_fg;
static PidFn k_pid;
static BeepFn k_beep;
static volatile int g_learn = -1;      /* action being learned, -1 = none */
static volatile int g_learn_phase;     /* 0 = wait until all keys are released, 1 = wait for the new key */
static volatile ULONGLONG g_learn_t0;
static unsigned g_plat_vk[CM_ACT_COUNT]; /* what the platform's own row currently watches */
static ULONGLONG g_last[CM_ACT_COUNT];
static char g_val[CM_ACT_COUNT][48];
static FH2PageRow g_rows[CM_ACT_COUNT];
static unsigned char g_snap[256];
static int g_snap_n, g_seen_unfocused;

static const struct { const char *n; unsigned vk; } k_named[] = {
    {"Space", 0x20}, {"Tab", 0x09}, {"Enter", 0x0D}, {"Esc", 0x1B}, {"Backspace", 0x08},
    {"Insert", 0x2D}, {"Delete", 0x2E}, {"Home", 0x24}, {"End", 0x23}, {"PageUp", 0x21}, {"PageDown", 0x22},
    {"Left", 0x25}, {"Up", 0x26}, {"Right", 0x27}, {"Down", 0x28},
    {"Num0", 0x60}, {"Num1", 0x61}, {"Num2", 0x62}, {"Num3", 0x63}, {"Num4", 0x64},
    {"Num5", 0x65}, {"Num6", 0x66}, {"Num7", 0x67}, {"Num8", 0x68}, {"Num9", 0x69},
    {"Semicolon", 0xBA}, {"Comma", 0xBC}, {"Minus", 0xBD}, {"Period", 0xBE}, {"Tilde", 0xC0},
    {"Equals", 0xBB}, {"Slash", 0xBF}, {"LeftBracket", 0xDB}, {"Backslash", 0xDC}, {"RightBracket", 0xDD},
    {"Quote", 0xDE}, {"Less", 0xE2}, {"CapsLock", 0x14}, {"NumMultiply", 0x6A}, {"NumAdd", 0x6B},
    {"NumSubtract", 0x6D}, {"NumDecimal", 0x6E}, {"NumDivide", 0x6F}, {"PrintScreen", 0x2C},
    {"ScrollLock", 0x91}, {"Pause", 0x13}, {"NumLock", 0x90},
};
static const struct { const char *n; unsigned bit; } k_pad[] = {
    {"DPadUp", 0x0001}, {"DPadDown", 0x0002}, {"DPadLeft", 0x0004}, {"DPadRight", 0x0008},
    {"Start", 0x0010}, {"Back", 0x0020}, {"L3", 0x0040}, {"R3", 0x0080},
    {"LB", 0x0100}, {"RB", 0x0200}, {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int ieq(const char *a, const char *b, int n) { /* a[0..n) == b, ohne Gross/Klein */
    int i = 0;
    for (; i < n; ++i) if (!b[i] || lower(a[i]) != lower(b[i])) return 0;
    return b[i] == 0;
}
static int slen(const char *s) { int n = 0; while (s[n]) ++n; return n; }
static char *put_str(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
static char *put_uint(char *o, unsigned v) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *o++ = t[--n];
    return o;
}

static char *write_key(char *o, unsigned vk, unsigned mods) {
    if (!vk) return put_str(o, "Off");
    if (mods & FH2_KEY_CTRL) o = put_str(o, "Ctrl+");
    if (mods & FH2_KEY_SHIFT) o = put_str(o, "Shift+");
    if (mods & FH2_KEY_ALT) o = put_str(o, "Alt+");
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { *o++ = (char)vk; return o; }
    if (vk >= 0x70 && vk <= 0x87) { *o++ = 'F'; return put_uint(o, vk - 0x70u + 1u); }
    for (int i = 0; i < COUNT(k_named); ++i) if (k_named[i].vk == vk) return put_str(o, k_named[i].n);
    o = put_str(o, "VK"); return put_uint(o, vk);
}
static char *write_pad(char *o, unsigned chord) {
    if (!chord) return put_str(o, "Off");
    int first = 1;
    for (int i = 0; i < COUNT(k_pad); ++i) if (chord & k_pad[i].bit) {
        if (!first) *o++ = '+';
        first = 0; o = put_str(o, k_pad[i].n);
    }
    return o;
}

static int parse_key(const char *s, int n, unsigned *vk, unsigned *mods) {
    *vk = 0; *mods = 0;
    while (n > 0 && (*s == ' ' || *s == '\t')) { ++s; --n; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) --n;
    if (n <= 0) return 0;
    if (ieq(s, "off", n) || ieq(s, "none", n)) return 1;
    for (;;) { /* Modifikatoren vorn abschneiden */
        if (n > 5 && ieq(s, "ctrl", 4) && s[4] == '+') { *mods |= FH2_KEY_CTRL; s += 5; n -= 5; continue; }
        if (n > 6 && ieq(s, "shift", 5) && s[5] == '+') { *mods |= FH2_KEY_SHIFT; s += 6; n -= 6; continue; }
        if (n > 4 && ieq(s, "alt", 3) && s[3] == '+') { *mods |= FH2_KEY_ALT; s += 4; n -= 4; continue; }
        break;
    }
    if (n == 1) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) { *vk = (unsigned)c; return 1; }
    }
    if ((s[0] == 'F' || s[0] == 'f') && n >= 2 && n <= 3) {
        unsigned v = 0; int ok = 1;
        for (int i = 1; i < n; ++i) { if (s[i] < '0' || s[i] > '9') { ok = 0; break; } v = v * 10u + (unsigned)(s[i] - '0'); }
        if (ok && v >= 1 && v <= 24) { *vk = 0x70u + v - 1u; return 1; }
    }
    for (int i = 0; i < COUNT(k_named); ++i) if (ieq(s, k_named[i].n, n)) { *vk = k_named[i].vk; return 1; }
    if (n > 2 && (s[0] == 'V' || s[0] == 'v') && (s[1] == 'K' || s[1] == 'k')) {
        unsigned v = 0;
        for (int i = 2; i < n; ++i) { if (s[i] < '0' || s[i] > '9') return 0; v = v * 10u + (unsigned)(s[i] - '0'); }
        *vk = v; return v != 0;
    }
    return 0;
}
static int parse_pad(const char *s, int n, unsigned *chord) {
    *chord = 0;
    while (n > 0 && (*s == ' ' || *s == '\t')) { ++s; --n; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) --n;
    if (n <= 0 || ieq(s, "off", n) || ieq(s, "none", n)) return 1;
    int start = 0;
    for (int i = 0; i <= n; ++i) {
        if (i == n || s[i] == '+') {
            int found = 0;
            for (int k = 0; k < COUNT(k_pad); ++k) if (ieq(s + start, k_pad[k].n, i - start)) { *chord |= k_pad[k].bit; found = 1; break; }
            if (!found) return 0;
            start = i + 1;
        }
    }
    return 1;
}

static void make_paths(void) {
    int n = 0;
    while (g_api->azimuth_dir && g_api->azimuth_dir[n] && n < MAX_PATH - 1) { g_dir[n] = g_api->azimuth_dir[n]; ++n; }
    static const wchar_t sub[] = L"\\CameraMods";
    for (int i = 0; sub[i]; ++i) g_dir[n++] = sub[i];
    g_dir[n] = 0;
    int m = 0;
    while (g_dir[m]) { g_ini[m] = g_dir[m]; ++m; }
    static const wchar_t file[] = L"\\keybinds.ini";
    for (int i = 0; file[i]; ++i) g_ini[m++] = file[i];
    g_ini[m] = 0;
}

static void save_all(void) {
    char *o = g_text;
    o = put_str(o, "; Camera Mods key bindings. Change them in the game: Pause > Settings > MODS > Installed mods > Camera Mods > Keys (any key).\r\n");
    o = put_str(o, "; Format:  Action=Key | Controller    (Off = unbound)   e.g.  Toggle=O | Off   or   HideDriver=Ctrl+F9 | LB+RB+Y\r\n");
    for (int i = 0; i < CM_ACT_COUNT; ++i) {
        o = put_str(o, cm_action_name[i]); *o++ = '=';
        o = write_key(o, g_bind[i].vk, g_bind[i].mods);
        o = put_str(o, " | ");
        o = write_pad(o, g_bind[i].chord);
        o = put_str(o, "\r\n");
    }
    CreateDirectoryW(g_dir, 0);
    HANDLE h = CreateFileW(g_ini, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { g_api->log(g_mod, "could not write keybinds.ini"); return; }
    DWORD w = 0;
    WriteFile(h, g_text, (DWORD)(o - g_text), &w, 0);
    CloseHandle(h);
}

static void load_all(void) {
    for (int i = 0; i < CM_ACT_COUNT; ++i) { g_bind[i].vk = cm_action_vk[i]; g_bind[i].mods = 0; g_bind[i].chord = 0; }
    HANDLE h = CreateFileW(g_ini, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { save_all(); return; } /* first start: write the defaults */
    DWORD got = 0;
    ReadFile(h, g_text, sizeof(g_text) - 1, &got, 0);
    CloseHandle(h);
    g_text[got] = 0;
    char *p = g_text;
    while (*p) {
        char *e = p; while (*e && *e != '\n') ++e;
        char *eq = p; while (eq < e && *eq != '=') ++eq;
        if (*p != ';' && *p != '#' && eq < e) {
            int nl = (int)(eq - p);
            while (nl > 0 && (p[nl - 1] == ' ' || p[nl - 1] == '\t')) --nl;
            for (int i = 0; i < CM_ACT_COUNT; ++i) {
                if (!ieq(p, cm_action_name[i], nl) || slen(cm_action_name[i]) != nl) continue;
                char *bar = eq + 1; while (bar < e && *bar != '|') ++bar;
                unsigned vk, mods, chord;
                if (parse_key(eq + 1, (int)(bar - eq - 1), &vk, &mods)) { g_bind[i].vk = vk; g_bind[i].mods = mods; }
                if (bar < e && parse_pad(bar + 1, (int)(e - bar - 1), &chord)) g_bind[i].chord = chord;
                break;
            }
        }
        p = *e ? e + 1 : e;
    }
}

static int key_down(unsigned vk) { return k_key && vk && (k_key((int)vk) & 0x8000) != 0; }
static int mods_now(void) {
    return (key_down(0x11) ? FH2_KEY_CTRL : 0) | (key_down(0x10) ? FH2_KEY_SHIFT : 0) | (key_down(0x12) ? FH2_KEY_ALT : 0);
}
static void fire(int i) {
    ULONGLONG now = GetTickCount64();
    if (now - g_last[i] < 120) return; /* the platform and our own poller both saw the same press */
    g_last[i] = now;
    void (*t)(int) = p_trigger;
    if (t) t(i);
}
static void on_press(void *user, const char *key) {
    (void)key;
    int i = (int)(INT_PTR)user;
    if (i < 0 || i >= CM_ACT_COUNT || g_learn >= 0) return;
    unsigned pv = g_plat_vk[i];
    if (pv && pv != g_bind[i].vk && key_down(pv)) return; /* an old key of the platform row, replaced on the Keys page */
    fire(i);
}
static void on_rebind(void *user, const char *key, uint32_t vk, uint32_t modifiers, uint32_t pad_chord) {
    (void)key;
    int i = (int)(INT_PTR)user;
    if (i < 0 || i >= CM_ACT_COUNT) return;
    g_bind[i].vk = vk; g_bind[i].mods = modifiers; g_bind[i].chord = pad_chord;
    g_plat_vk[i] = vk;
    save_all();
}

static void fill_rows(void) {
    for (int i = 0; i < CM_ACT_COUNT; ++i) {
        char *o = write_key(g_val[i], g_bind[i].vk, g_bind[i].mods); *o = 0;
        FH2PageRow *r = &g_rows[i];
        r->size = sizeof *r;
        r->key = cm_action_name[i];
        r->label = cm_action_label[i];
        r->value = g_val[i];
        r->description = "Press A (or Enter), then press the key you want within 10 seconds. Any key works; hold Ctrl, Shift or Alt for a combination. Esc cancels. Go back to the MODS tab and open Installed mods again to see the new key.";
        r->footer = 0;
        r->style = FH2_ROW_NORMAL;
        r->value_alert = 0;
    }
    g_api->set_page_rows(g_mod, "Keys", g_rows, CM_ACT_COUNT);
}
static void page_open(void *user, const char *page_key) { (void)user; (void)page_key; fill_rows(); }
static void page_select(void *user, const char *page_key, const char *row_key) {
    (void)user; (void)page_key;
    if (!row_key) return;
    for (int i = 0; i < CM_ACT_COUNT; ++i) {
        const char *n = cm_action_name[i];
        int k = 0;
        while (n[k] && row_key[k] == n[k]) ++k;
        if (n[k] == 0 && row_key[k] == 0) {
            g_learn_phase = 0;
            g_seen_unfocused = 0;
            g_learn_t0 = GetTickCount64();
            g_learn = i;
            g_api->log(g_mod, "key learning: waiting for a key");
            return;
        }
    }
}

static void log_num(const char *what, unsigned v) {
    char line[96]; char *o = put_str(line, what); o = put_uint(o, v); *o = 0;
    g_api->log(g_mod, line);
}
static void learn_step(int focused) {
    if (!focused) g_seen_unfocused = 1;
    if (GetTickCount64() - g_learn_t0 > 10000) {
        g_learn = -1;
        g_api->log(g_mod, "key learning: timed out, no new key seen");
        log_num("  keys held at start: ", (unsigned)g_snap_n);
        log_num("  window was unfocused at some point: ", (unsigned)g_seen_unfocused);
        if (k_beep) k_beep(0x10);
        return;
    }
    if (!focused) return;
    if (g_learn_phase == 0) { /* remember what is held right now (the key that selected the row, stuck keys) */
        g_snap_n = 0;
        for (unsigned v = 8; v < 255; ++v) { g_snap[v] = (unsigned char)key_down(v); g_snap_n += g_snap[v]; }
        g_learn_phase = 1;
        return;
    }
    unsigned hit = 0;
    for (unsigned v = 8; v < 255; ++v) {
        if (v == 0x10 || v == 0x11 || v == 0x12 || (v >= 0xA0 && v <= 0xA5)) continue; /* modifiers */
        int d = key_down(v);
        if (!d) { g_snap[v] = 0; continue; }
        if (g_snap[v]) continue; /* was already down when learning started */
        if (!hit) hit = v;
    }
    if (!hit) return;
    int i = g_learn;
    g_learn = -1;
    log_num("key learning: got VK ", hit);
    if (hit == 0x1B) { if (k_beep) k_beep(0x10); return; } /* Esc cancels */
    g_bind[i].vk = hit;
    g_bind[i].mods = (unsigned)mods_now();
    save_all();
    {
        char line[96]; char *o = put_str(line, "key for "); o = put_str(o, cm_action_name[i]); o = put_str(o, " = ");
        o = write_key(o, g_bind[i].vk, g_bind[i].mods); *o = 0;
        g_api->log(g_mod, line);
    }
    fill_rows(); /* the page shows the new key the next time the menus write it */
    if (k_beep) k_beep(0x40);
}

static DWORD WINAPI poll_thread(LPVOID unused) {
    (void)unused;
    unsigned char prev[CM_ACT_COUNT];
    for (int i = 0; i < CM_ACT_COUNT; ++i) prev[i] = 0;
    DWORD me = GetCurrentProcessId();
    for (;;) {
        Sleep(15);
        if (!k_key || !k_fg || !k_pid) continue;
        HWND fg = k_fg();
        DWORD pid = 0;
        if (fg) k_pid(fg, &pid);
        int focused = fg && pid == me;
        if (g_learn >= 0) { learn_step(focused); continue; }
        int m = mods_now();
        for (int i = 0; i < CM_ACT_COUNT; ++i) {
            int down = focused && g_bind[i].vk && key_down(g_bind[i].vk) && m == (int)g_bind[i].mods;
            if (down && !prev[i]) fire(i);
            prev[i] = (unsigned char)down;
        }
    }
}

static DWORD WINAPI link_thread(LPVOID unused) {
    (void)unused;
    for (int i = 0; i < 1200; ++i) { /* bis zu 10 Minuten warten, bis Extra Options das Plugin geladen hat */
        HMODULE m = GetModuleHandleW(L"CameraMods.dll");
        if (m) {
            void (*attach)(void) = (void (*)(void))GetProcAddress(m, "CM_Attach");
            void (*trigger)(int) = (void (*)(int))GetProcAddress(m, "CM_Trigger");
            if (attach && trigger) {
                attach();
                p_trigger = trigger;
                g_api->log(g_mod, "linked to CameraMods.dll: keys now come from the Mods page");
                return 0;
            }
        }
        Sleep(500);
    }
    g_api->log(g_mod, "CameraMods.dll was not found (is the Extra Options plugin installed?): hotkeys do nothing");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi *api) {
    if (!api || api->api_version < 9) return 1; /* older platform: Camera Mods keeps its default keys */
    g_api = api;
    FH2ModInfo info;
    info.size = sizeof info;
    info.name = "Camera Mods";
    info.version = BRIDGE_VERSION;
    info.github_repo = "Deuquan/FH2-CameraMods";
    info.platform_version = "2.8.0";
    info.check_updates = 1;
    info.description = "Drone, walking, passenger and cinematic cameras for clips. Rebind all keys: open Keys here and pick any key.";
    info.offline_only = 0;
    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;
    make_paths();
    load_all();
    {
        HMODULE u = LoadLibraryExW(L"user32.dll", 0, 0x800);
        if (u) {
            k_key = (KeyFn)(void *)GetProcAddress(u, "GetAsyncKeyState");
            k_fg = (FgFn)(void *)GetProcAddress(u, "GetForegroundWindow");
            k_pid = (PidFn)(void *)GetProcAddress(u, "GetWindowThreadProcessId");
            k_beep = (BeepFn)(void *)GetProcAddress(u, "MessageBeep");
        }
        FH2Page pg;
        char *z = (char *)&pg; for (unsigned q = 0; q < sizeof pg; ++q) z[q] = 0;
        pg.size = sizeof pg;
        pg.key = "Keys";
        pg.title = "Camera Mods - Keys";
        pg.label = "Keys (any key on the keyboard)";
        pg.description = "Pick an action, press A or Enter, then press any key. The Mods page rows only offer F1-F12.";
        pg.on_open = page_open;
        pg.on_select = page_select;
        if (!api->add_page(g_mod, &pg)) api->log(g_mod, "could not add the Keys page");
        else fill_rows();
    }
    /* The platform's own hotkey rows (F1-F12 only, two rows per action) would push the Keys page far down the
       list, and the bridge polls the keyboard itself, so they are off by default. */
#if CM_PLATFORM_ROWS
    int added = 0;
    for (int i = 0; i < CM_ACT_COUNT; ++i) {
        FH2Hotkey *h = &g_hk[i];
        h->size = sizeof *h;
        h->key = cm_action_name[i];
        h->label = cm_action_label[i];
        g_plat_vk[i] = g_bind[i].vk;
        h->vk = g_bind[i].vk;
        h->modifiers = g_bind[i].mods;
        h->pad_chord = g_bind[i].chord;
        h->on_press = on_press;
        h->on_rebind = on_rebind;
        h->user = (void *)(INT_PTR)i;
        added += api->add_hotkey(g_mod, h);
    }
#else
    int added = 0;
    for (int i = 0; i < CM_ACT_COUNT; ++i) g_plat_vk[i] = 0;
    (void)g_hk; (void)on_press; (void)on_rebind;
#endif
    {
        char line[48]; char *o = put_str(line, "registered "); o = put_uint(o, (unsigned)added); o = put_str(o, " platform hotkeys"); *o = 0;
        api->log(g_mod, line);
    }
    HANDLE t = CreateThread(0, 0, link_thread, 0, 0, 0);
    if (t) CloseHandle(t);
    t = CreateThread(0, 0, poll_thread, 0, 0, 0);
    if (t) CloseHandle(t);
    return 0;
}

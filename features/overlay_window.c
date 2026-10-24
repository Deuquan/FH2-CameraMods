// Drohnen-Overlay als eigenes, durchklickbares Fenster ueber dem Spiel:
// Bildformat-Balken, Vignette und eine kleine Anzeige. Filter ueber die Windows-Lupe (Farbmatrix).
#include "features.h"
#include "game_input.h"
#include "logging.h"
#include "support.h"

/* Ohne C-Bibliothek: der Compiler erzeugt memset-Aufrufe, deshalb liefern wir es selbst. */
void *memset(void *destination, int value, size_t count) __attribute__((used, noinline, optnone));
void *memset(void *destination, int value, size_t count) {
    unsigned char *bytes = (unsigned char *)destination;
    while (count--) {
        *bytes++ = (unsigned char)value;
    }
    return destination;
}

typedef struct {
    DWORD biSize;
    LONG biWidth, biHeight;
    WORD biPlanes, biBitCount;
    DWORD biCompression, biSizeImage;
    LONG biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
} MyBmpHeader;

typedef struct {
    BYTE op, flags, alpha, format;
} MyBlend;

typedef struct {
    float m[5][5];
} MyColorEffect;

typedef struct {
    HWND hwnd;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
    DWORD time;
    POINT pt;
} MyMsg;

typedef HWND(WINAPI *FnCreateWindow)(DWORD, const char *, const char *, DWORD, int, int, int, int, HWND, void *,
                                     HINSTANCE, void *);
typedef BOOL(WINAPI *FnShow)(HWND, int);
typedef BOOL(WINAPI *FnSetPos)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL(WINAPI *FnUpdateLayered)(HWND, void *, POINT *, POINT *, void *, POINT *, DWORD, MyBlend *, DWORD);
typedef BOOL(WINAPI *FnGetRect)(HWND, RECT *);
typedef HWND(WINAPI *FnForeground)(void);
typedef DWORD(WINAPI *FnPid)(HWND, LPDWORD);
typedef BOOL(WINAPI *FnPeek)(MyMsg *, HWND, UINT, UINT, UINT);
typedef LRESULT(WINAPI *FnDispatch)(const MyMsg *);
typedef void *(WINAPI *FnCreateDc)(void *);
typedef void *(WINAPI *FnCreateDib)(void *, const MyBmpHeader *, UINT, void **, void *, DWORD);
typedef void *(WINAPI *FnSelect)(void *, void *);
typedef BOOL(WINAPI *FnDelete)(void *);
typedef BOOL(WINAPI *FnMagInit)(void);
typedef BOOL(WINAPI *FnMagEffect)(const MyColorEffect *);
typedef void(WINAPI *FnSleep)(DWORD);
typedef HANDLE(WINAPI *FnCreateThread)(void *, size_t, LPTHREAD_START_ROUTINE, LPVOID, DWORD, DWORD *);

/* ------------------------------------------------------------------ Zustand */
static int aspect_index; /* 0 voll, 1 = 9:16, 2 = 4:5, 3 = 1:1, 4 = 16:9, 5 = 21:9 */
static int filter_index;
static int started;

static const float aspect_values[] = {0.0f, 9.0f / 16.0f, 4.0f / 5.0f, 1.0f, 16.0f / 9.0f, 21.0f / 9.0f};
#define ASPECT_COUNT 6

typedef struct {
    const char *name;
    float sat, contrast, bright, vignette, tint[3];
} Filter;
static const Filter filters[] = {
    {"AUS", 1, 1, 1, 0, {1, 1, 1}},
};
#define FILTER_COUNT 1

/* ------------------------------------------------------------------ Zeichnen in die Bitmap */

static unsigned *bits; /* aktuelle Zielbitmap (ARGB, vormultipliziert) */
static int bmp_w, bmp_h;

static void fill_rect(int x0, int y0, int x1, int y1, unsigned argb) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bmp_w) x1 = bmp_w;
    if (y1 > bmp_h) y1 = bmp_h;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) bits[y * bmp_w + x] = argb;
}

static void clear_bits(void) {
    for (int i = 0; i < bmp_w * bmp_h; ++i) bits[i] = 0;
}

static void crop_rect(int w, int h, int *cx, int *cy, int *cw, int *ch) {
    float W = (float)w, H = (float)h;
    float x = 0, y = 0, cwf = W, chf = H;
    if (aspect_index > 0) {
        float ar = aspect_values[aspect_index];
        if (ar < W / H) { cwf = H * ar; x = (W - cwf) * 0.5f; }
        else { chf = W / ar; y = (H - chf) * 0.5f; }
    }
    *cx = (int)x; *cy = (int)y; *cw = (int)cwf; *ch = (int)chf;
}

/* Balken + Vignette ins grosse Fenster (nur bei Aenderung) */
static void build_base(void) {
    clear_bits();
    int cx, cy, cw, ch;
    crop_rect(bmp_w, bmp_h, &cx, &cy, &cw, &ch);
    if (aspect_index > 0) {
        fill_rect(0, 0, cx, bmp_h, 0x8C000000u);
        fill_rect(cx + cw, 0, bmp_w, bmp_h, 0x8C000000u);
        fill_rect(0, 0, bmp_w, cy, 0x8C000000u);
        fill_rect(0, cy + ch, bmp_w, bmp_h, 0x8C000000u);
    }
    float vig = filters[filter_index].vignette;
    if (vig > 0.0f && cw > 0 && ch > 0) {
        for (int y = cy; y < cy + ch && y < bmp_h; ++y) {
            float qy = ((float)(y - cy) + 0.5f) / (float)ch - 0.5f;
            for (int x = cx; x < cx + cw && x < bmp_w; ++x) {
                float qx = ((float)(x - cx) + 0.5f) / (float)cw - 0.5f;
                float a = vig * (qx * qx + qy * qy) * 2.0f;
                if (a > 1.0f) a = 1.0f;
                unsigned alpha = (unsigned)(a * 255.0f);
                if (alpha) bits[y * bmp_w + x] = alpha << 24;
            }
        }
    }
}

/* ------------------------------------------------------------------ Filter ueber die Lupe */
static void set_color_effect(FnMagEffect effect, int index) {
    MyColorEffect m;
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) m.m[i][j] = (i == j) ? 1.0f : 0.0f;
    if (index > 0) {
        const Filter *f = &filters[index];
        static const float luma[3] = {0.2126f, 0.7152f, 0.0722f};
        for (int j = 0; j < 3; ++j) {
            float k = f->contrast * f->bright * f->tint[j];
            for (int i = 0; i < 3; ++i) {
                m.m[i][j] = ((1.0f - f->sat) * luma[i] + (i == j ? f->sat : 0.0f)) * k;
            }
            m.m[4][j] = 0.5f * (1.0f - f->contrast) * f->bright * f->tint[j];
        }
    }
    effect(&m);
}

/* ------------------------------------------------------------------ Fensterebenen */
typedef struct {
    HWND hwnd;
    void *dc, *dib;
    unsigned *pixels;
    int w, h, shown;
} Layer;

static FnCreateWindow api_create_window;
static FnShow api_show;
static FnSetPos api_set_pos;
static FnUpdateLayered api_update_layered;
static FnCreateDc api_create_dc;
static FnCreateDib api_create_dib;
static FnSelect api_select;
static FnDelete api_delete_object, api_delete_dc;

static int layer_resize(Layer *layer, int w, int h, int x, int y) {
    if (!layer->hwnd) {
        layer->hwnd = api_create_window(0x80000u | 0x20u | 0x8u | 0x08000000u | 0x80u, "STATIC", "fh2drone", 0x80000000u,
                                        x, y, w, h, 0, 0, 0, 0);
        if (!layer->hwnd) return 0;
    }
    if (layer->dib && layer->w == w && layer->h == h) return 1;
    if (layer->dib) { api_delete_object(layer->dib); layer->dib = 0; }
    if (layer->dc) { api_delete_dc(layer->dc); layer->dc = 0; }
    layer->dc = api_create_dc(0);
    MyBmpHeader header = {sizeof(MyBmpHeader), w, -h, 1, 32, 0, 0, 0, 0, 0, 0};
    void *pixels = 0;
    layer->dib = api_create_dib(layer->dc, &header, 0, &pixels, 0, 0);
    if (!layer->dc || !layer->dib || !pixels) return 0;
    api_select(layer->dc, layer->dib);
    layer->pixels = (unsigned *)pixels;
    layer->w = w;
    layer->h = h;
    return 1;
}

static void layer_present(Layer *layer, int x, int y) {
    POINT dst = {x, y}, src = {0, 0};
    POINT size = {layer->w, layer->h};
    MyBlend blend = {0, 0, 255, 1};
    api_update_layered(layer->hwnd, 0, &dst, &size, layer->dc, &src, 0, &blend, 2);
    if (!layer->shown) {
        api_show(layer->hwnd, 4);
        api_set_pos(layer->hwnd, (HWND)(intptr_t)-1, x, y, layer->w, layer->h, 0x10);
        layer->shown = 1;
    }
}

static void layer_hide(Layer *layer) {
    if (layer->shown) {
        api_show(layer->hwnd, 0);
        layer->shown = 0;
    }
}

/* ------------------------------------------------------------------ Thread */
static DWORD WINAPI overlay_thread(LPVOID unused) {
    (void)unused;
    HMODULE user32 = LoadLibraryExW(L"user32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE gdi32 = LoadLibraryExW(L"gdi32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE kernel = LoadLibraryExW(L"kernel32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE mag = LoadLibraryExW(L"magnification.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!user32 || !gdi32 || !kernel) { log_append("OVERLAY: libs missing\r\n"); return 0; }
    api_create_window = (FnCreateWindow)(uintptr_t)GetProcAddress(user32, "CreateWindowExA");
    api_show = (FnShow)(uintptr_t)GetProcAddress(user32, "ShowWindow");
    api_set_pos = (FnSetPos)(uintptr_t)GetProcAddress(user32, "SetWindowPos");
    api_update_layered = (FnUpdateLayered)(uintptr_t)GetProcAddress(user32, "UpdateLayeredWindow");
    FnGetRect get_rect = (FnGetRect)(uintptr_t)GetProcAddress(user32, "GetWindowRect");
    FnForeground foreground = (FnForeground)(uintptr_t)GetProcAddress(user32, "GetForegroundWindow");
    FnPid window_pid = (FnPid)(uintptr_t)GetProcAddress(user32, "GetWindowThreadProcessId");
    FnPeek peek = (FnPeek)(uintptr_t)GetProcAddress(user32, "PeekMessageA");
    FnDispatch dispatch = (FnDispatch)(uintptr_t)GetProcAddress(user32, "DispatchMessageA");
    api_create_dc = (FnCreateDc)(uintptr_t)GetProcAddress(gdi32, "CreateCompatibleDC");
    api_create_dib = (FnCreateDib)(uintptr_t)GetProcAddress(gdi32, "CreateDIBSection");
    api_select = (FnSelect)(uintptr_t)GetProcAddress(gdi32, "SelectObject");
    api_delete_object = (FnDelete)(uintptr_t)GetProcAddress(gdi32, "DeleteObject");
    api_delete_dc = (FnDelete)(uintptr_t)GetProcAddress(gdi32, "DeleteDC");
    FnSleep sleep_fn = (FnSleep)(uintptr_t)GetProcAddress(kernel, "Sleep");
    FnMagInit mag_init = mag ? (FnMagInit)(uintptr_t)GetProcAddress(mag, "MagInitialize") : 0;
    FnMagEffect mag_effect = mag ? (FnMagEffect)(uintptr_t)GetProcAddress(mag, "MagSetFullscreenColorEffect") : 0;
    if (!api_create_window || !api_show || !api_set_pos || !api_update_layered || !get_rect || !foreground ||
        !window_pid || !peek || !dispatch || !api_create_dc || !api_create_dib || !api_select || !api_delete_object ||
        !api_delete_dc || !sleep_fn) {
        log_append("OVERLAY: API missing\r\n");
        return 0;
    }
    int mag_ok = mag_init && mag_effect && mag_init();
    log_append_value("OVERLAY: magnification ready", mag_ok);

    Layer base = {0};
    HWND game = 0;
    int applied_filter = 0;
    unsigned last_state = 0xFFFFFFFFu;
    int last_x = 0, last_y = 0, last_w = 0, last_h = 0;
    int b_down = 1;
    log_append("OVERLAY: thread running\r\n");

    for (;;) {
        sleep_fn(40);
        MyMsg msg;
        while (peek(&msg, 0, 0, 0, 1)) dispatch(&msg);

        HWND fg = foreground();
        if (fg && fg != base.hwnd) {
            DWORD pid = 0;
            window_pid(fg, &pid);
            if (pid == GetCurrentProcessId()) game = fg;
        }
        int on = drone_probe_is_on();
        if (!on || !game) {
            b_down = 1;
            if (base.hwnd) { layer_hide(&base); last_state = 0xFFFFFFFFu; }
            if (applied_filter && mag_ok) { set_color_effect(mag_effect, 0); applied_filter = 0; }
            continue;
        }
        if (fg == game) {
            int b = game_input_key_down(0x42);
            if (b && !b_down) aspect_index = (aspect_index + 1) % ASPECT_COUNT;
            b_down = b;
        }
        RECT r;
        if (!get_rect(game, &r)) continue;
        int w = r.right - r.left, h = r.bottom - r.top;
        if (w < 64 || h < 64) continue;

        if (!layer_resize(&base, w, h, r.left, r.top)) { log_append("OVERLAY: base layer failed\r\n"); return 0; }
        int cx, cy, cw, ch;
        crop_rect(w, h, &cx, &cy, &cw, &ch);
        (void)cx; (void)cy; (void)cw; (void)ch;
        unsigned state = (unsigned)aspect_index | ((unsigned)filter_index << 4);
        int moved = r.left != last_x || r.top != last_y || w != last_w || h != last_h;
        int base_dirty = state != last_state || moved;
        if (base_dirty) {
            int need_base = aspect_index > 0 || filters[filter_index].vignette > 0.0f;
            if (need_base) {
                bits = base.pixels; bmp_w = base.w; bmp_h = base.h;
                build_base();
                layer_present(&base, r.left, r.top);
            } else {
                layer_hide(&base); /* kein Fenster ueber dem Spiel, wenn nichts zu zeichnen ist */
            }
            if (mag_ok) { set_color_effect(mag_effect, filter_index); applied_filter = filter_index != 0; }
        }
        last_state = state; last_x = r.left; last_y = r.top; last_w = w; last_h = h;
    }
}

void overlay_start(void) {
    if (started) return;
    started = 1;
    HMODULE kernel = LoadLibraryExW(L"kernel32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!kernel) return;
    FnCreateThread create_thread = (FnCreateThread)(uintptr_t)GetProcAddress(kernel, "CreateThread");
    if (!create_thread) { log_append("OVERLAY: CreateThread missing\r\n"); return; }
    HANDLE thread = create_thread(0, 0, overlay_thread, 0, 0, 0);
    if (thread) CloseHandle(thread);
}

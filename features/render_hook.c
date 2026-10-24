// Drohnen-Overlay: Haengt sich in IDXGISwapChain::Present ein und zeichnet
// Bildformat-Balken (9:16 usw.), Filter und eine kleine eigene Anzeige.
#include "features.h"
#include "game_input.h"
#include "logging.h"
#include "support.h"

/* Der Compiler erzeugt memset-Aufrufe fuer Nullinitialisierung; ohne C-Bibliothek liefern wir es selbst. */
void *memset(void *destination, int value, size_t count) __attribute__((used, noinline, optnone));
void *memset(void *destination, int value, size_t count) {
    unsigned char *bytes = (unsigned char *)destination;
    while (count--) {
        *bytes++ = (unsigned char)value;
    }
    return destination;
}

/* ------------------------------------------------------------------ Typen */
typedef struct {
    UINT Width, Height;
    UINT RefreshN, RefreshD;
    UINT Format, ScanlineOrdering, Scaling;
} MyModeDesc;

typedef struct {
    MyModeDesc BufferDesc;
    UINT SampleCount, SampleQuality;
    UINT BufferUsage;
    UINT BufferCount;
    HWND OutputWindow;
    BOOL Windowed;
    UINT SwapEffect;
    UINT Flags;
} MySwapDesc;

typedef struct {
    UINT Width, Height, MipLevels, ArraySize, Format, SampleCount, SampleQuality, Usage, BindFlags, CpuAccess, Misc;
} MyTexDesc;

typedef struct {
    UINT ByteWidth, Usage, BindFlags, CpuAccess, Misc, Stride;
} MyBufDesc;

typedef struct {
    BOOL BlendEnable;
    UINT Src, Dst, Op, SrcA, DstA, OpA;
    UINT Mask;
} MyRtBlend;

typedef struct {
    BOOL AlphaToCoverage, Independent;
    MyRtBlend Rt[8];
} MyBlendDesc;

typedef struct {
    float x, y, w, h, zmin, zmax;
} MyViewport;

typedef struct {
    float a[4], b[4], c[4], d[4];
} MyConstants;

typedef HRESULT(WINAPI *CreateDevSwapFn)(void *, UINT, HMODULE, UINT, const UINT *, UINT, UINT, const MySwapDesc *,
                                          void **, void **, UINT *, void **);
typedef HWND(WINAPI *CreateWindowExAFn)(DWORD, const char *, const char *, DWORD, int, int, int, int, HWND, void *,
                                        HINSTANCE, void *);
typedef BOOL(WINAPI *DestroyWindowFn)(HWND);
typedef HRESULT(WINAPI *PresentFn)(void *, UINT, UINT);
typedef HRESULT(WINAPI *Present1Fn)(void *, UINT, UINT, const void *);
typedef HRESULT(WINAPI *CompileFn)(const void *, size_t, const char *, const void *, void *, const char *,
                                    const char *, UINT, UINT, void **, void **);

#define VT(object) (*(void ***)(object))
#define CALL(object, slot, type) ((type)VT(object)[slot])
typedef ULONG(WINAPI *FnRelease)(void *);
#define RELEASE(object) \
    do { \
        if (object) { CALL(object, 2, FnRelease)(object); object = 0; } \
    } while (0)

/* ------------------------------------------------------------------ Zustand */
static PresentFn original_present, original_present_b;
static Present1Fn original_present1;
static int install_state;
static int described;
static int overlay_failed;
static unsigned present_calls;

static void *cur_device, *cur_context;
static void *vs_shader, *ps_shader, *blend_state, *const_buffer, *copy_tex, *copy_srv, *ui_tex, *ui_srv;
static UINT copy_w, copy_h, copy_format;

#define UI_W 760
#define UI_H 64
static unsigned ui_pixels[UI_W * UI_H];
static char ui_text_cache[64];

static int aspect_index; /* 0 aus, 1 = 9:16, 2 = 4:5, 3 = 1:1, 4 = 16:9, 5 = 21:9 */
static int filter_index;
static int ui_visible = 1;

static const char *const aspect_names[] = {"VOLL", "9:16", "4:5", "1:1", "16:9", "21:9"};
static const float aspect_values[] = {0.0f, 9.0f / 16.0f, 4.0f / 5.0f, 1.0f, 16.0f / 9.0f, 21.0f / 9.0f};
#define ASPECT_COUNT 6

typedef struct {
    const char *name;
    float sat, contrast, bright, vignette, tint[3];
} Filter;
static const Filter filters[] = {
    {"AUS", 1, 1, 1, 0, {1, 1, 1}},
    {"KINO", 0.90f, 1.20f, 0.97f, 0.35f, {0.97f, 1.00f, 1.06f}},
    {"WARM", 1.15f, 1.05f, 1.02f, 0.20f, {1.08f, 1.00f, 0.88f}},
    {"KALT", 0.95f, 1.08f, 1.00f, 0.20f, {0.90f, 0.98f, 1.10f}},
    {"LEBENDIG", 1.45f, 1.12f, 1.00f, 0.15f, {1, 1, 1}},
    {"S/W", 0.00f, 1.25f, 1.00f, 0.35f, {1, 1, 1}},
};
#define FILTER_COUNT 6

static const char shader_source[] =
    "cbuffer C : register(b0) { float4 a; float4 b; float4 c; float4 d; };\n"
    "Texture2D T0 : register(t0);\n"
    "struct VO { float4 p : SV_Position; };\n"
    "VO vs(uint id : SV_VertexID) {\n"
    "  VO o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(VO i) : SV_Target {\n"
    "  int2 px = int2(i.p.xy);\n"
    "  if (a.x < 0.5) return float4(0, 0, 0, a.y);\n"
    "  if (a.x < 1.5) {\n"
    "    float3 col = T0.Load(int3(px, 0)).rgb;\n"
    "    float l = dot(col, float3(0.2126, 0.7152, 0.0722));\n"
    "    col = lerp(float3(l, l, l), col, b.x);\n"
    "    col = (col - 0.5) * b.y + 0.5;\n"
    "    col = col * b.z * c.rgb;\n"
    "    float2 q = (i.p.xy - d.xy) / d.zw - 0.5;\n"
    "    col = col * (1.0 - b.w * dot(q, q) * 2.0);\n"
    "    return float4(saturate(col), 1); }\n"
    "  int2 u = px - int2(d.xy);\n"
    "  if (u.x < 0 || u.y < 0 || u.x >= (int)d.z || u.y >= (int)d.w) return float4(0, 0, 0, 0);\n"
    "  return T0.Load(int3(u, 0)); }\n";

/* ------------------------------------------------------------------ Schrift 5x7 */
typedef struct {
    char c;
    unsigned char rows[7];
} Glyph;

static const Glyph glyphs[] = {
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},   {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {30, 1, 1, 14, 1, 1, 30}},     {'4', {2, 6, 10, 18, 31, 2, 2}},  {'5', {31, 16, 30, 1, 1, 17, 14}},
    {'6', {6, 8, 16, 30, 17, 17, 14}},   {'7', {31, 1, 2, 4, 8, 8, 8}},    {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 2, 12}},   {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'F', {31, 16, 16, 30, 16, 16, 16}}, {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}},      {'J', {7, 2, 2, 2, 2, 18, 12}},   {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'L', {16, 16, 16, 16, 16, 16, 31}}, {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}},
    {'R', {30, 17, 17, 30, 20, 18, 17}}, {'S', {15, 16, 16, 14, 1, 1, 30}}, {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}}, {'W', {17, 17, 17, 21, 21, 27, 17}},
    {'X', {17, 17, 10, 4, 10, 17, 17}},  {'Y', {17, 17, 10, 4, 4, 4, 4}},  {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {':', {0, 4, 0, 0, 0, 4, 0}},        {'.', {0, 0, 0, 0, 0, 12, 12}},   {'-', {0, 0, 0, 31, 0, 0, 0}},
    {'/', {1, 1, 2, 4, 8, 16, 16}},      {'%', {25, 25, 2, 4, 8, 19, 19}}, {' ', {0, 0, 0, 0, 0, 0, 0}},
};

static const Glyph *find_glyph(char c) {
    for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i) {
        if (glyphs[i].c == c) return &glyphs[i];
    }
    return &glyphs[sizeof(glyphs) / sizeof(glyphs[0]) - 1];
}

static void ui_clear(void) {
    for (unsigned i = 0; i < UI_W * UI_H; ++i) ui_pixels[i] = 0;
}

static void ui_pixel(int x, int y, unsigned color) {
    if (x >= 0 && y >= 0 && x < UI_W && y < UI_H) ui_pixels[y * UI_W + x] = color;
}

#define COLOR_WHITE 0xFFFFFFFFu
#define COLOR_SHADOW 0xC0000000u
#define COLOR_RED 0xFF3030F0u /* ABGR: R=F0 G=30 B=30 */

static int ui_text(int x, int y, const char *s, int scale, unsigned color) {
    for (; *s; ++s) {
        const Glyph *g = find_glyph(*s);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (g->rows[row] & (1 << (4 - col))) {
                    for (int dy = 0; dy < scale; ++dy) {
                        for (int dx = 0; dx < scale; ++dx) {
                            ui_pixel(x + col * scale + dx + 2, y + row * scale + dy + 2, COLOR_SHADOW);
                        }
                    }
                }
            }
        }
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (g->rows[row] & (1 << (4 - col))) {
                    for (int dy = 0; dy < scale; ++dy) {
                        for (int dx = 0; dx < scale; ++dx) {
                            ui_pixel(x + col * scale + dx, y + row * scale + dy, color);
                        }
                    }
                }
            }
        }
        x += 6 * scale;
    }
    return x;
}

static void format_number(char *out, unsigned value, unsigned min_digits) {
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while ((unsigned)n < min_digits) tmp[n++] = '0';
    while (n) *out++ = tmp[--n];
    *out = 0;
}

static size_t append(char *buffer, size_t pos, const char *text) {
    while (*text && pos < 63) buffer[pos++] = *text++;
    buffer[pos] = 0;
    return pos;
}

/* ------------------------------------------------------------------ Ressourcen */
static void release_resources(void) {
    RELEASE(copy_srv);
    RELEASE(copy_tex);
    RELEASE(ui_srv);
    RELEASE(ui_tex);
    RELEASE(const_buffer);
    RELEASE(blend_state);
    RELEASE(ps_shader);
    RELEASE(vs_shader);
    RELEASE(cur_context);
    cur_device = 0;
    copy_w = copy_h = 0;
}

static int create_shaders(void *device) {
    HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!compiler) {
        log_append("RENDER: d3dcompiler_47.dll missing\r\n");
        return 0;
    }
    CompileFn compile = (CompileFn)(uintptr_t)GetProcAddress(compiler, "D3DCompile");
    if (!compile) return 0;
    void *vs_blob = 0, *ps_blob = 0, *errors = 0;
    typedef void *(WINAPI * FnPtr)(void *);
    typedef size_t(WINAPI * FnSize)(void *);
    HRESULT hr = compile(shader_source, sizeof(shader_source) - 1, "fh2", 0, 0, "vs", "vs_4_0", 0, 0, &vs_blob, &errors);
    if (hr < 0 || !vs_blob) {
        log_append_value("RENDER: VS compile failed", (int)hr);
        if (errors) log_append((const char *)CALL(errors, 3, FnPtr)(errors));
        RELEASE(errors);
        return 0;
    }
    hr = compile(shader_source, sizeof(shader_source) - 1, "fh2", 0, 0, "ps", "ps_4_0", 0, 0, &ps_blob, &errors);
    if (hr < 0 || !ps_blob) {
        log_append_value("RENDER: PS compile failed", (int)hr);
        if (errors) log_append((const char *)CALL(errors, 3, FnPtr)(errors));
        RELEASE(errors);
        RELEASE(vs_blob);
        return 0;
    }
    typedef HRESULT(WINAPI * FnCreateShader)(void *, const void *, size_t, void *, void **);
    HRESULT r1 = CALL(device, 12, FnCreateShader)(device, CALL(vs_blob, 3, FnPtr)(vs_blob), CALL(vs_blob, 4, FnSize)(vs_blob), 0, &vs_shader);
    HRESULT r2 = CALL(device, 15, FnCreateShader)(device, CALL(ps_blob, 3, FnPtr)(ps_blob), CALL(ps_blob, 4, FnSize)(ps_blob), 0, &ps_shader);
    RELEASE(vs_blob);
    RELEASE(ps_blob);
    if (r1 < 0 || r2 < 0) {
        log_append("RENDER: CreateShader failed\r\n");
        return 0;
    }
    return 1;
}

static int create_state(void *device) {
    MyBlendDesc blend;
    memory_copy(&blend, &(MyBlendDesc){0}, sizeof(blend));
    blend.Rt[0].BlendEnable = 1;
    blend.Rt[0].Src = 5;   /* SRC_ALPHA */
    blend.Rt[0].Dst = 6;   /* INV_SRC_ALPHA */
    blend.Rt[0].Op = 1;    /* ADD */
    blend.Rt[0].SrcA = 1;  /* ZERO */
    blend.Rt[0].DstA = 2;  /* ONE: Alpha des Bildes unveraendert */
    blend.Rt[0].OpA = 1;   /* ADD */
    blend.Rt[0].Mask = 15;
    typedef HRESULT(WINAPI * FnBlend)(void *, const MyBlendDesc *, void **);
    if (CALL(device, 20, FnBlend)(device, &blend, &blend_state) < 0) {
        log_append("RENDER: CreateBlendState failed\r\n");
        return 0;
    }
    MyBufDesc buf = {sizeof(MyConstants), 0 /* DEFAULT */, 4 /* CONSTANT_BUFFER */, 0, 0, 0};
    typedef HRESULT(WINAPI * FnBuffer)(void *, const MyBufDesc *, const void *, void **);
    if (CALL(device, 3, FnBuffer)(device, &buf, 0, &const_buffer) < 0) {
        log_append("RENDER: CreateBuffer failed\r\n");
        return 0;
    }
    MyTexDesc tex = {UI_W, UI_H, 1, 1, 28 /* RGBA8 */, 1, 0, 0, 8 /* SRV */, 0, 0};
    typedef HRESULT(WINAPI * FnTex)(void *, const MyTexDesc *, const void *, void **);
    typedef HRESULT(WINAPI * FnSrv)(void *, void *, const void *, void **);
    if (CALL(device, 5, FnTex)(device, &tex, 0, &ui_tex) < 0 || CALL(device, 7, FnSrv)(device, ui_tex, 0, &ui_srv) < 0) {
        log_append("RENDER: UI texture failed\r\n");
        return 0;
    }
    return 1;
}

static int ensure_copy(void *device, UINT w, UINT h, UINT format) {
    if (copy_tex && copy_w == w && copy_h == h && copy_format == format) return 1;
    RELEASE(copy_srv);
    RELEASE(copy_tex);
    MyTexDesc tex = {w, h, 1, 1, format, 1, 0, 0, 8, 0, 0};
    typedef HRESULT(WINAPI * FnTex)(void *, const MyTexDesc *, const void *, void **);
    typedef HRESULT(WINAPI * FnSrv)(void *, void *, const void *, void **);
    if (CALL(device, 5, FnTex)(device, &tex, 0, &copy_tex) < 0 || CALL(device, 7, FnSrv)(device, copy_tex, 0, &copy_srv) < 0) {
        log_append("RENDER: copy texture failed\r\n");
        RELEASE(copy_srv);
        RELEASE(copy_tex);
        return 0;
    }
    copy_w = w;
    copy_h = h;
    copy_format = format;
    return 1;
}

/* ------------------------------------------------------------------ Zeichnen */
typedef void(WINAPI *FnVoid1)(void *);
typedef void(WINAPI *FnDraw)(void *, UINT, UINT);
typedef void(WINAPI *FnSetViewports)(void *, UINT, const MyViewport *);
typedef void(WINAPI *FnSetShader)(void *, void *, void *const *, UINT);
typedef void(WINAPI *FnSetRT)(void *, UINT, void *const *, void *);
typedef void(WINAPI *FnSetBlend)(void *, void *, const float *, UINT);
typedef void(WINAPI *FnTopology)(void *, UINT);
typedef void(WINAPI *FnPsSrv)(void *, UINT, UINT, void *const *);
typedef void(WINAPI *FnPsCb)(void *, UINT, UINT, void *const *);
typedef void(WINAPI *FnUpdate)(void *, void *, UINT, const void *, const void *, UINT, UINT);
typedef void(WINAPI *FnCopy)(void *, void *, void *);

static void set_constants(void *context, float mode, float alpha, const Filter *f, float x, float y, float w, float h) {
    MyConstants k;
    k.a[0] = mode; k.a[1] = alpha; k.a[2] = 0; k.a[3] = 0;
    k.b[0] = f ? f->sat : 1; k.b[1] = f ? f->contrast : 1; k.b[2] = f ? f->bright : 1; k.b[3] = f ? f->vignette : 0;
    k.c[0] = f ? f->tint[0] : 1; k.c[1] = f ? f->tint[1] : 1; k.c[2] = f ? f->tint[2] : 1; k.c[3] = 0;
    k.d[0] = x; k.d[1] = y; k.d[2] = w; k.d[3] = h;
    CALL(context, 48, FnUpdate)(context, const_buffer, 0, 0, &k, 0, 0);
}

static void draw_rect(void *context, float x, float y, float w, float h) {
    if (w < 1.0f || h < 1.0f) return;
    MyViewport vp = {x, y, w, h, 0.0f, 1.0f};
    CALL(context, 44, FnSetViewports)(context, 1, &vp);
    CALL(context, 13, FnDraw)(context, 3, 0);
}

static void build_ui(void *context, float frame_zoom) {
    char text[64];
    char num[16];
    size_t pos = 0;
    text[0] = 0;
    unsigned seconds = (unsigned)(drone_probe_seconds());
    format_number(num, seconds / 60, 2);
    pos = append(text, pos, num);
    pos = append(text, pos, ":");
    format_number(num, seconds % 60, 2);
    pos = append(text, pos, num);
    pos = append(text, pos, "  ZOOM ");
    unsigned z10 = (unsigned)(frame_zoom * 10.0f + 0.5f);
    format_number(num, z10 / 10, 1);
    pos = append(text, pos, num);
    pos = append(text, pos, ".");
    format_number(num, z10 % 10, 1);
    pos = append(text, pos, num);
    pos = append(text, pos, "X  ");
    pos = append(text, pos, aspect_names[aspect_index]);
    pos = append(text, pos, "  ");
    pos = append(text, pos, filters[filter_index].name);
    int same = 1;
    for (size_t i = 0; i < 64; ++i) {
        if (ui_text_cache[i] != text[i]) { same = 0; }
        if (!text[i]) break;
    }
    if (same) return;
    for (size_t i = 0; i < 64; ++i) {
        ui_text_cache[i] = text[i];
        if (!text[i]) break;
    }
    ui_clear();
    /* roter Aufnahmepunkt */
    for (int y = -10; y <= 10; ++y)
        for (int x = -10; x <= 10; ++x)
            if (x * x + y * y <= 100) ui_pixel(18 + x, 32 + y, COLOR_RED);
    ui_text(40, 16, "REC", 4, COLOR_WHITE);
    ui_text(150, 16, text, 4, COLOR_WHITE);
    CALL(context, 48, FnUpdate)(context, ui_tex, 0, 0, ui_pixels, UI_W * 4, 0);
}

static void draw_overlay(void *swap) {
    typedef HRESULT(WINAPI * FnGetBuffer)(void *, UINT, const GUID *, void **);
    typedef void(WINAPI * FnGetDev)(void *, void **);
    typedef void(WINAPI * FnTexDesc)(void *, MyTexDesc *);
    static const GUID iid_tex2d = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
    void *back = 0, *device = 0;
    static int logged_getbuffer;
    HRESULT gb = CALL(swap, 9, FnGetBuffer)(swap, 0, &iid_tex2d, &back);
    if (gb < 0 || !back) {
        if (!logged_getbuffer) {
            logged_getbuffer = 1;
            log_append_value("RENDER: GetBuffer(ID3D11Texture2D) failed hr", (int)gb);
        }
        return;
    }
    if (!logged_getbuffer) {
        logged_getbuffer = 1;
        log_append("RENDER: GetBuffer ok\r\n");
    }
    CALL(back, 3, FnGetDev)(back, &device);
    if (!device) { log_append("RENDER: GetDevice failed\r\n"); RELEASE(back); return; }
    MyTexDesc desc;
    memory_copy(&desc, &(MyTexDesc){0}, sizeof(desc));
    CALL(back, 10, FnTexDesc)(back, &desc);
    if (device != cur_device) {
        release_resources();
        if (!create_shaders(device) || !create_state(device)) {
            overlay_failed = 1;
            RELEASE(device);
            RELEASE(back);
            return;
        }
        cur_device = device;
        typedef void(WINAPI * FnImm)(void *, void **);
        CALL(device, 40, FnImm)(device, &cur_context);
        log_append("RENDER: overlay resources ready\r\n");
    }
    float W = (float)desc.Width, H = (float)desc.Height;
    float cx = 0.0f, cy = 0.0f, cw = W, ch = H;
    if (aspect_index > 0) {
        float ar = aspect_values[aspect_index];
        if (ar < W / H) { cw = H * ar; cx = (W - cw) * 0.5f; }
        else { ch = W / ar; cy = (H - ch) * 0.5f; }
        cw = (float)(int)cw; ch = (float)(int)ch; cx = (float)(int)cx; cy = (float)(int)cy;
    }
    int use_filter = filter_index > 0;
    if (use_filter && !ensure_copy(device, desc.Width, desc.Height, desc.Format)) use_filter = 0;

    void *deferred = 0, *rtv = 0, *list = 0;
    typedef HRESULT(WINAPI * FnDeferred)(void *, UINT, void **);
    typedef HRESULT(WINAPI * FnRtv)(void *, void *, const void *, void **);
    typedef HRESULT(WINAPI * FnFinish)(void *, BOOL, void **);
    typedef void(WINAPI * FnExec)(void *, void *, BOOL);
    if (CALL(device, 27, FnDeferred)(device, 0, &deferred) >= 0 && deferred &&
        CALL(device, 9, FnRtv)(device, back, 0, &rtv) >= 0 && rtv) {
        void *ctx = deferred;
        const float blend_factor[4] = {0, 0, 0, 0};
        float zoom = drone_probe_zoom();
        if (ui_visible) build_ui(ctx, zoom);
        if (use_filter) CALL(ctx, 47, FnCopy)(ctx, copy_tex, back);
        CALL(ctx, 33, FnSetRT)(ctx, 1, &rtv, 0);
        CALL(ctx, 24, FnTopology)(ctx, 4);
        CALL(ctx, 11, FnSetShader)(ctx, vs_shader, 0, 0);
        CALL(ctx, 9, FnSetShader)(ctx, ps_shader, 0, 0);
        CALL(ctx, 16, FnPsCb)(ctx, 0, 1, &const_buffer);
        CALL(ctx, 35, FnSetBlend)(ctx, blend_state, blend_factor, 0xffffffffu);
        if (use_filter) {
            CALL(ctx, 8, FnPsSrv)(ctx, 0, 1, &copy_srv);
            set_constants(ctx, 1.0f, 1.0f, &filters[filter_index], cx, cy, cw, ch);
            draw_rect(ctx, cx, cy, cw, ch);
        }
        if (aspect_index > 0) {
            set_constants(ctx, 0.0f, 0.62f, 0, 0, 0, 0, 0);
            if (cx > 0.0f) { draw_rect(ctx, 0, 0, cx, H); draw_rect(ctx, cx + cw, 0, W - cx - cw, H); }
            if (cy > 0.0f) { draw_rect(ctx, 0, 0, W, cy); draw_rect(ctx, 0, cy + ch, W, H - cy - ch); }
        }
        if (ui_visible) {
            void *null_srv = 0;
            (void)null_srv;
            CALL(ctx, 8, FnPsSrv)(ctx, 0, 1, &ui_srv);
            float ux = cx + 40.0f, uy = cy + 40.0f;
            set_constants(ctx, 2.0f, 1.0f, 0, ux, uy, (float)UI_W, (float)UI_H);
            draw_rect(ctx, ux, uy, (float)UI_W, (float)UI_H);
        }
        if (CALL(ctx, 114, FnFinish)(ctx, 0, &list) >= 0 && list) {
            CALL(cur_context, 58, FnExec)(cur_context, list, 1 /* Zustand wiederherstellen */);
        }
    }
    RELEASE(list);
    RELEASE(rtv);
    RELEASE(deferred);
    RELEASE(device);
    RELEASE(back);
}

static int key_pressed(int vk, int *was_down) {
    int down = game_input_key_down(vk);
    int edge = down && !*was_down;
    *was_down = down;
    return edge;
}

static void overlay_frame(void *swap) {
    static int b_down, n_down, h_down;
    if (overlay_failed) return;
    static int logged_first;
    if (!logged_first && drone_probe_is_on()) { logged_first = 1; log_append("RENDER: overlay_frame active\r\n"); }
    if (!drone_probe_is_on()) {
        b_down = n_down = h_down = 1; /* beim Einschalten keinen Fehlimpuls */
        return;
    }
    if (key_pressed(0x42, &b_down)) aspect_index = (aspect_index + 1) % ASPECT_COUNT;
    if (key_pressed(0x4E, &n_down)) filter_index = (filter_index + 1) % FILTER_COUNT;
    if (key_pressed(0x48, &h_down)) ui_visible = !ui_visible;
    if (aspect_index == 0 && filter_index == 0 && !ui_visible) return;
    draw_overlay(swap);
}

/* ------------------------------------------------------------------ Present-Hook */
static void describe(void *swap) {
    if (described) return;
    described = 1;
    typedef HRESULT(WINAPI * GetDescFn)(void *, MySwapDesc *);
    MySwapDesc desc;
    memory_copy(&desc, &(MySwapDesc){0}, sizeof(desc));
    if (CALL(swap, 12, GetDescFn)(swap, &desc) >= 0) {
        log_append_value("RENDER: swapchain width", (int)desc.BufferDesc.Width);
        log_append_value("RENDER: swapchain height", (int)desc.BufferDesc.Height);
        log_append_value("RENDER: swapchain format", (int)desc.BufferDesc.Format);
    }
}

static HRESULT WINAPI present_hook(void *swap, UINT sync, UINT flags) {
    ++present_calls;
    if (present_calls == 1) {
        log_append("RENDER: Present hook active\r\n");
        describe(swap);
    }
    overlay_frame(swap);
    return original_present(swap, sync, flags);
}

static HRESULT WINAPI present_hook_b(void *swap, UINT sync, UINT flags) {
    ++present_calls;
    if (present_calls == 1) {
        log_append("RENDER: Present (swapchain1 table) hook active\r\n");
        describe(swap);
    }
    overlay_frame(swap);
    return original_present_b(swap, sync, flags);
}

static HRESULT WINAPI present1_hook(void *swap, UINT sync, UINT flags, const void *params) {
    ++present_calls;
    if (present_calls == 1) {
        log_append("RENDER: Present1 hook active\r\n");
        describe(swap);
    }
    overlay_frame(swap);
    return original_present1(swap, sync, flags, params);
}

static int patch_slot(void **slot, void *replacement) {
    DWORD previous;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &previous)) return 0;
    *slot = replacement;
    VirtualProtect(slot, sizeof(void *), previous, &previous);
    return 1;
}

void render_hook_install(void) {
    if (install_state) return;
    install_state = 2;
    HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE user32 = LoadLibraryExW(L"user32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d3d11 || !user32) {
        log_append("RENDER: d3d11/user32 not loadable\r\n");
        return;
    }
    CreateDevSwapFn create = (CreateDevSwapFn)(uintptr_t)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    CreateWindowExAFn create_window = (CreateWindowExAFn)(uintptr_t)GetProcAddress(user32, "CreateWindowExA");
    DestroyWindowFn destroy_window = (DestroyWindowFn)(uintptr_t)GetProcAddress(user32, "DestroyWindow");
    if (!create || !create_window || !destroy_window) {
        log_append("RENDER: API missing\r\n");
        return;
    }
    HWND window = create_window(0, "STATIC", "fh2probe", 0x80000000u /* WS_POPUP */, 0, 0, 64, 64, 0, 0, 0, 0);
    if (!window) {
        log_append("RENDER: dummy window failed\r\n");
        return;
    }
    MySwapDesc desc;
    memory_copy(&desc, &(MySwapDesc){0}, sizeof(desc));
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = 28;
    desc.SampleCount = 1;
    desc.BufferUsage = 0x20;
    desc.BufferCount = 1;
    desc.OutputWindow = window;
    desc.Windowed = 1;
    void *swap = 0, *device = 0, *context = 0;
    UINT level = 0;
    HRESULT result = create(0, 1, 0, 0, 0, 0, 7, &desc, &swap, &device, &level, &context);
    if (result < 0 || !swap) {
        log_append_value("RENDER: dummy swapchain failed hr", (int)result);
        destroy_window(window);
        return;
    }
    void **vtable = *(void ***)swap;
    original_present = (PresentFn)vtable[8];
    int ok = patch_slot(&vtable[8], (void *)present_hook);
    log_append_value("RENDER: Present vtable patched", ok);
    typedef HRESULT(WINAPI * QueryFn)(void *, const GUID *, void **);
    static const GUID iid_swapchain1 = {0x790a45f7, 0x0d42, 0x4876, {0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa}};
    void *swap1 = 0;
    QueryFn query = (QueryFn)vtable[0];
    if (query(swap, &iid_swapchain1, &swap1) >= 0 && swap1) {
        void **vtable1 = *(void ***)swap1;
        if (vtable1 != vtable && vtable1[8] != (void *)present_hook) {
            original_present_b = (PresentFn)vtable1[8];
            patch_slot(&vtable1[8], (void *)present_hook_b);
        }
        original_present1 = (Present1Fn)vtable1[22];
        int ok1 = patch_slot(&vtable1[22], (void *)present1_hook);
        log_append_value("RENDER: Present1 vtable patched", ok1);
    }
    install_state = ok ? 1 : 2;
    destroy_window(window);
}

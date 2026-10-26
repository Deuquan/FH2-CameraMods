// Mess-Version fuer den Drohnenmodus: Mit F3 wird der Speicher der aktiven Kamera
// ins Log (FH2_ExtraOptions.log) geschrieben, damit die Position gefunden werden kann.
#include "autodrive_game.h"
#include "features.h"
#include "game_input.h"
#include "logging.h"
#include "support.h"

static int drone_input_enabled = 1;
static unsigned long long drone_start_tick;
#define game_input_key_down(k) (drone_input_enabled ? game_input_key_down(k) : 0)

#ifndef VK_F5
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#endif

static char text[16384];
static size_t used;

static void put(const char *message) {
    while (*message && used < sizeof(text) - 1) {
        text[used++] = *message++;
    }
    text[used] = 0;
}

static void put_hex(uint64_t value, unsigned digits) {
    char buffer[17];
    for (unsigned index = 0; index < digits; ++index) {
        unsigned nibble = (unsigned)((value >> (4 * (digits - 1 - index))) & 15);
        buffer[index] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    buffer[digits] = 0;
    put(buffer);
}

static void put_number(unsigned value) {
    char buffer[12];
    char *cursor = buffer + sizeof(buffer) - 1;
    *cursor = 0;
    do {
        *--cursor = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    put(cursor);
}

static void flush(void) {
    if (used) {
        log_append(text);
    }
    used = 0;
    text[0] = 0;
}

// ---------------------------------------------------------------------------
// Drohne v2: F9 schaltet um.
//  Position = double[3] (Haube: Kamera+0x1a0, Verfolger: Kamera+0x190)
//  Blickrichtung = float[3] (Haube: Kamera+0x1c0, Verfolger: Kamera+0x1b0)
//  Steuerung: WASD / IJKL, Q/E oder R/F hoch+runter, Maus = umsehen, Mausrad = Zoom,
//  Pfeiltasten = umsehen, Shift = schnell.
// ---------------------------------------------------------------------------
#define KEY_TOGGLE 0x4F /* O */
#define PI_F 3.14159265f

typedef BOOL(WINAPI *GetCursorPosFn)(POINT *);
typedef BOOL(WINAPI *SetCursorPosFn)(int, int);
typedef BOOL(WINAPI *GetWindowRectFn)(HWND, RECT *);
typedef HWND(WINAPI *GetForegroundWindowFn)(void);
typedef HHOOK(WINAPI *SetHookFn)(int, HOOKPROC, HINSTANCE, DWORD);
typedef LRESULT(WINAPI *CallNextFn)(HHOOK, int, WPARAM, LPARAM);
typedef BOOL(WINAPI *GetMessageFn)(LPMSG, HWND, UINT, UINT);
typedef HANDLE(WINAPI *CreateThreadFn)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID,
                                       DWORD, LPDWORD);

typedef struct {
    LONG x, y;
    DWORD mouse_data, flags, time;
    ULONG_PTR extra;
} WheelHookData;

static GetCursorPosFn api_get_cursor;
static SetCursorPosFn api_set_cursor;
static GetWindowRectFn api_window_rect;
static GetForegroundWindowFn api_foreground;
static SetHookFn api_set_hook;
static CallNextFn api_call_next;
static GetMessageFn api_get_message;
static int mouse_api_ready, mouse_api_tried, wheel_thread_started;
static volatile int wheel_accum;
static HHOOK wheel_hook_handle;

static void put_signed(int value) {
    if (value < 0) {
        put("-");
        value = -value;
    }
    put_number((unsigned)value);
}

static double last_written[3];
static void *drone_camera_ptr;
static void *last_seen_camera;
static int last_focus = 1;
static int have_written;
static unsigned moved_logs, spike_logs;
static unsigned state_logs_left;
static double state_g[3];
static double last_game_pos[3];
static int have_last_game;
static unsigned jump_logs;

static int drone_on;
static int toggle_was_down;
static double drone_pos[3];
static double drone_anchor[3];
static int walk_mode;
static int shake_on = 1;
static int walk_level_pct = 40;
/* Bodenhoehe: Spur des Autos (x, z, Boden-y), damit die Augenhoehe am Boden bleibt (Huegel, Verfolgen) */
enum { TRAIL_MAX = 256 };
static float trail_x[TRAIL_MAX], trail_z[TRAIL_MAX], trail_g[TRAIL_MAX];
static int trail_n, trail_head;
static double trail_last_x, trail_last_z;
static void trail_push(double x, double z, double g) {
    trail_x[trail_head] = (float)x; trail_z[trail_head] = (float)z; trail_g[trail_head] = (float)g;
    trail_head = (trail_head + 1) % TRAIL_MAX;
    if (trail_n < TRAIL_MAX) ++trail_n;
    trail_last_x = x; trail_last_z = z;
}
static float t_noise;
/* Beifahrer-Modus: Kamera sitzt im Auto auf dem anderen Sitz und dreht sich mit dem Auto mit */
static int passenger_on, passenger_side = 1, rear_on, drv_on;
/* Gespeicherte Sitzpositionen je Kamera: 0 Beifahrer, 1 Fahrer, 2 360-Kamera (bleiben beim Durchschalten erhalten) */
static float prof_r[3] = {0.72f, 0.0f, 0.0f}, prof_f[3] = {0.0f, 0.20f, -3.4f}, prof_e[3] = {1.6f, 1.62f, 2.9f}, prof_z[3] = {1.0f, 1.0f, 0.5f}, prof_p[3];
static int cur_mode = -1;
static float car_hx = 1.0f, car_hz, pch_x = 1.0f, pch_z, start_hx = 1.0f, start_hz;
static float last_out_fwd[3];
static double car_prev_p[3];
static float car_vx, car_vz;
static int car_prev_ok;
static float acc_s, prev_vl, head_p, head_v;
static int have_prev_vl;
static float spd_s, yr_s, lat_s, pc_hx = 1.0f, pc_hz, prev_vy, vacc_s, bump_p, bump_v, pv[3];
static int have_prev_c, seat_ok;
static int cinema_on, cin_shot, cin_n;
static int ego_view_chase, chase_on = 1; /* Verfolger-Kamera: gleicher Effekt (Taste J) */
static int ego_on = 0, ego_level_pct = 100; /* verbesserte Ego-Kamera (Taste K, F7/F8 Staerke) */
static float ego_pitch, ego_yaw, ego_roll, ego_dx, ego_dy, ego_dz;
static float seat_r = 0.72f, seat_f; /* Sitzposition zum Fahrer: rechts / vor (m), bleibt gespeichert */
static float cin_t, cin_dur, cin_b, cin_pause, cin_speed = 2.6f, cin_z0 = 1.0f, cin_z1 = 1.0f, cin_wob = 0.1f;
static int cin_prev;
static float cin_yv, cin_pv, cin_lat, cin_fw, cin_rl;
static unsigned cin_seed, qpc_lo;
static double seat_y;
static float car_pitch_s;
static int cs_ok, cs_seen, cs_lost_logs, cs_prev_ok = 1;
static float cs_sign = 1.0f, gfh_x, gfh_z;
static int gfh_fresh, pad_sign_pending;
static float cs_fx, cs_fz, cs_v[3];
static float cur_roll, bank_roll, pan_rate_s, prev_hx = 1.0f, prev_hz, rphase[3];
static int focus_on;
static int follow_on;
static double car_pos[3];
static int have_car_pos;
static float dt_smooth;
static float eye_height = 1.6f, walk_phase, walk_amp, pend_yaw, pend_pitch;
static double ground_ref;
static float drone_hx, drone_hz, drone_pitch;
static float drone_vel[3];
static float look_yaw_rate, look_pitch_rate;
static float phase[6];
static int mouse_primed;
static float mouse_yaw_rate, mouse_pitch_rate;
static float zoom_target = 1.0f, zoom_current = 1.0f;
static void seat_mode(int m) {
    if (cur_mode >= 0) { prof_r[cur_mode] = seat_r; prof_f[cur_mode] = seat_f; prof_e[cur_mode] = eye_height; prof_p[cur_mode] = drone_pitch; if (cur_mode != 2) prof_z[cur_mode] = zoom_target; }
    cur_mode = m;
    if (m >= 0) { seat_r = prof_r[m]; seat_f = prof_f[m]; eye_height = prof_e[m]; drone_pitch = prof_p[m]; zoom_target = m == 2 ? 0.5f : prof_z[m]; }
}

static LRESULT CALLBACK wheel_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == 0 && wparam == 0x020A) {
        const WheelHookData *data = (const WheelHookData *)lparam;
        wheel_accum += (short)(data->mouse_data >> 16);
    }
    return api_call_next(wheel_hook_handle, code, wparam, lparam);
}

static DWORD WINAPI wheel_thread(LPVOID unused) {
    (void)unused;
    {   /* Hook-Thread mit hoher Prioritaet: SetCursorPos wartet sonst auf ihn (Ruckler auf schwachen PCs) */
        typedef BOOL (WINAPI *FnSetPrio)(HANDLE, int);
        HMODULE k32 = LoadLibraryExW(L"kernel32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
        FnSetPrio sp = k32 ? (FnSetPrio)(uintptr_t)GetProcAddress(k32, "SetThreadPriority") : 0;
        if (sp) sp((HANDLE)(intptr_t)-2, 15);
    }
    HMODULE user32 = LoadLibraryExW(L"user32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    wheel_hook_handle = api_set_hook(14, wheel_hook_proc, (HINSTANCE)user32, 0);
    MSG message;
    while (api_get_message(&message, 0, 0, 0) > 0) {
    }
    return 0;
}

static void init_mouse_api(void) {
    if (mouse_api_tried) return;
    mouse_api_tried = 1;
    HMODULE user32 = LoadLibraryExW(L"user32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE kernel = LoadLibraryExW(L"kernel32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!user32 || !kernel) return;
    api_get_cursor = (GetCursorPosFn)(uintptr_t)GetProcAddress(user32, "GetCursorPos");
    api_set_cursor = (SetCursorPosFn)(uintptr_t)GetProcAddress(user32, "SetCursorPos");
    api_window_rect = (GetWindowRectFn)(uintptr_t)GetProcAddress(user32, "GetWindowRect");
    api_foreground = (GetForegroundWindowFn)(uintptr_t)GetProcAddress(user32, "GetForegroundWindow");
    api_set_hook = (SetHookFn)(uintptr_t)GetProcAddress(user32, "SetWindowsHookExW");
    api_call_next = (CallNextFn)(uintptr_t)GetProcAddress(user32, "CallNextHookEx");
    api_get_message = (GetMessageFn)(uintptr_t)GetProcAddress(user32, "GetMessageW");
    CreateThreadFn create_thread = (CreateThreadFn)(uintptr_t)GetProcAddress(kernel, "CreateThread");
    mouse_api_ready = api_get_cursor && api_set_cursor && api_window_rect && api_foreground;
    if (api_set_hook && api_call_next && api_get_message && create_thread && !wheel_thread_started) {
        wheel_thread_started = 1;
        HANDLE thread = create_thread(0, 0, wheel_thread, 0, 0, 0);
        if (thread) CloseHandle(thread);
    }
    put(mouse_api_ready ? "DRONE: mouse look ready\r\n" : "DRONE: mouse api missing\r\n");
    flush();
}

static float wrap_pi(float x) {
    while (x > PI_F) x -= 2.0f * PI_F;
    while (x < -PI_F) x += 2.0f * PI_F;
    return x;
}

static float my_sin(float x) {
    x = wrap_pi(x);
    float x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}

static float my_atan(float x) {
    /* Naeherung, Fehler < 0,005 rad */
    float a = x < 0.0f ? -x : x;
    float r;
    if (a <= 1.0f) r = a / (1.0f + 0.28f * a * a);
    else r = 0.5f * PI_F - (1.0f / a) / (1.0f + 0.28f / (a * a));
    return x < 0.0f ? -r : r;
}

static float my_fabs_f(float v) { return v < 0.0f ? -v : v; }
static float my_atan2(float y, float x) {
    if (x > 0.0f) return my_atan(y / x);
    if (x < 0.0f) return my_atan(y / x) + (y >= 0.0f ? PI_F : -PI_F);
    return y > 0.0f ? 0.5f * PI_F : (y < 0.0f ? -0.5f * PI_F : 0.0f);
}

/* Glattes Zufallsrauschen (Wertrauschen) fuer unregelmaessige Staerke: Kanal ch, Zeit t */
static float vn_hash(unsigned i, unsigned ch) {
    unsigned x = i * 0x9E3779B1u ^ (ch + 1u) * 0x85EBCA6Bu;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; x *= 0x297A2D39u; x ^= x >> 15;
    return (float)(x & 0xFFFFu) / 65535.0f;
}
static float vnoise(unsigned ch, float t) {
    if (t < 0.0f) t = -t;
    unsigned i = (unsigned)t;
    float f = t - (float)i;
    f = f * f * (3.0f - 2.0f * f);
    return vn_hash(i, ch) * (1.0f - f) + vn_hash(i + 1u, ch) * f;
}

static float my_cos(float x) {
    return my_sin(x + 0.5f * PI_F);
}

static float my_sqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    float g = x > 1.0f ? x * 0.5f : 1.0f;
    for (int i = 0; i < 12; ++i) g = 0.5f * (g + x / g);
    return g;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int key2(int a, int b) {
    return game_input_key_down(a) || game_input_key_down(b);
}

static float axis(int plus_a, int plus_b, int minus_a, int minus_b) {
    return (float)(key2(plus_a, plus_b) ? 1 : 0) - (float)(key2(minus_a, minus_b) ? 1 : 0);
}

static void rotate_heading(float angle) {
    float c = my_cos(angle), sn = my_sin(angle);
    float nx = c * drone_hx + sn * drone_hz;
    float nz = c * drone_hz - sn * drone_hx;
    float len = my_sqrt(nx * nx + nz * nz);
    if (len > 0.0001f) {
        drone_hx = nx / len;
        drone_hz = nz / len;
    }
}

static void window_center(int *cx, int *cy) {
    RECT rect;
    HWND window = api_foreground();
    *cx = 960;
    *cy = 540;
    if (window && api_window_rect(window, &rect)) {
        *cx = (rect.left + rect.right) / 2;
        *cy = (rect.top + rect.bottom) / 2;
    }
}

static void drone_start(void *camera, unsigned view) {
    unsigned pos_off = view <= 3 ? 0x190u : 0x1a0u;
    unsigned dir_off = view <= 3 ? 0x1b0u : 0x1c0u;
    double position[3];
    float forward[3];
    unsigned char *base = (unsigned char *)camera;
    if (!memory_read(base + pos_off, &position[0], sizeof(double)) ||
        !memory_read(base + pos_off + 8, &position[1], sizeof(double)) ||
        !memory_read(base + pos_off + 16, &position[2], sizeof(double)) ||
        !memory_read(base + dir_off, forward, sizeof(forward)) || !float_is_finite(forward[0]) ||
        !float_is_finite(forward[2]) || !float_is_finite(forward[1])) {
        return;
    }
    init_mouse_api();
    drone_pos[0] = position[0];
    drone_pos[1] = position[1];
    drone_pos[2] = position[2];
    drone_anchor[0] = position[0]; drone_anchor[1] = position[1]; drone_anchor[2] = position[2];
    float length = my_sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (length < 0.001f) {
        drone_hx = 0.0f;
        drone_hz = 1.0f;
    } else {
        drone_hx = forward[0] / length;
        drone_hz = forward[2] / length;
    }
    drone_pitch = clampf(forward[1], -1.2f, 1.2f);
    start_hx = drone_hx; start_hz = drone_hz; car_hx = drone_hx; car_hz = drone_hz; passenger_on = 0; rear_on = 0; drv_on = 0; cs_seen = 0;
    drone_vel[0] = drone_vel[1] = drone_vel[2] = 0.0f;
    look_yaw_rate = look_pitch_rate = 0.0f;
    zoom_target = zoom_current = 1.0f;
    wheel_accum = 0;
    mouse_primed = 0;
    have_last_game = 0;
    have_written = 0;
    moved_logs = 0;
    spike_logs = 0;
    jump_logs = 0;
    mouse_yaw_rate = mouse_pitch_rate = 0.0f;
    walk_mode = 0; focus_on = 0; follow_on = 0;
    car_pos[0] = drone_anchor[0]; car_pos[1] = drone_anchor[1]; car_pos[2] = drone_anchor[2]; have_car_pos = 1;
    walk_amp = 0.0f; pend_yaw = pend_pitch = 0.0f;
    drone_on = 1;
    drone_start_tick = GetTickCount64();
    last_seen_camera = 0;
    put("DRONE ON\r\n");
    flush();
}

static void drone_stop(void) {
    seat_mode(-1); /* aktuelle Sitzposition speichern */
    drone_on = 0; passenger_on = 0; rear_on = 0; drv_on = 0; cinema_on = 0;
    cur_roll = 0.0f; bank_roll = 0.0f;
    drone_camera_ptr = 0;
    zoom_target = zoom_current = 1.0f;
    put("DRONE OFF\r\n");
    flush();
}

static void drone_frame(void *camera, float dt, unsigned view) {
    unsigned char *base = (unsigned char *)camera;
    unsigned pos_off = view <= 3 ? 0x190u : 0x1a0u;
    unsigned dir_off = view <= 3 ? 0x1b0u : 0x1c0u;
    if (!(dt > 0.0f) || dt > 0.1f) dt = 0.016f;
    {
        typedef BOOL (WINAPI *FnQ)(LARGE_INTEGER *);
        static FnQ qpc, qpf;
        static LARGE_INTEGER qf;
        static int qinit;
        LARGE_INTEGER qn;
        qn.QuadPart = 0;
        if (!qinit) {
            qinit = 1;
            HMODULE k = LoadLibraryExW(L"kernel32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (k) { qpc = (FnQ)(uintptr_t)GetProcAddress(k, "QueryPerformanceCounter"); qpf = (FnQ)(uintptr_t)GetProcAddress(k, "QueryPerformanceFrequency"); }
            if (qpf) qpf(&qf);
        }
        if (qpc) qpc(&qn);
        qpc_lo = (unsigned)qn.QuadPart;
    }
    {
        double g[3];
        if (memory_read(base + pos_off, &g[0], 8) && memory_read(base + pos_off + 8, &g[1], 8) &&
            memory_read(base + pos_off + 16, &g[2], 8)) {
            if (have_last_game) {
                double dx = g[0] - last_game_pos[0], dy = g[1] - last_game_pos[1], dz = g[2] - last_game_pos[2];
                double d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > 9.0 && d2 < 1.0e12 && jump_logs < 60) {
                    ++jump_logs;
                    put("DRONE-JUMP view=");
                    put_number(view);
                    put(" game_delta=");
                    put_signed((int)dx); put(","); put_signed((int)dy); put(","); put_signed((int)dz);
                    put(" game_now=");
                    put_signed((int)g[0]); put(","); put_signed((int)g[1]); put(","); put_signed((int)g[2]);
                    put(" drone=");
                    put_signed((int)drone_pos[0]); put(","); put_signed((int)drone_pos[1]); put(","); put_signed((int)drone_pos[2]);
                    put("\r\n");
                    flush();
                }
            }
            last_game_pos[0] = g[0]; last_game_pos[1] = g[1]; last_game_pos[2] = g[2];
            state_g[0] = g[0]; state_g[1] = g[1]; state_g[2] = g[2];
            cs_ok = drift_camera_car_state(camera, &cs_fx, &cs_fz, cs_v);
            {
                /* Neigung des Autos (Berg auf/ab): die Kamera kippt mit dem Auto, sonst wandert der Innenraum im Bild hoch und runter */
                float pfy, phl;
                if (cs_ok && drift_camera_car_pitch(camera, &pfy, &phl) && phl > 0.1f) {
                    float tp = clampf(my_atan2(pfy, phl), -0.6f, 0.6f);
                    car_pitch_s += (tp - car_pitch_s) * clampf(dt * 8.0f, 0.0f, 1.0f);
                }
            }
            {
                /* Hat das Spiel die Position selbst neu gesetzt (nicht unser eigener Wert), ist es die Auto-Position */
                double ex = g[0] - last_written[0], ey = g[1] - last_written[1], ez = g[2] - last_written[2];
                if (!have_written || ex * ex + ey * ey + ez * ez > 1.0e-4) {
                    double cx = g[0] - car_pos[0], cy = g[1] - car_pos[1], cz = g[2] - car_pos[2];
                    if (passenger_on && cs_ok && have_car_pos && cx * cx + cy * cy + cz * cz < 9.0 && dt > 0.0f && dt < 0.1f) {
                        /* Beifahrer: Auto mit der Geschwindigkeit weiterrechnen und mit ca. 0,3 s Traegheit zur Spiel-Kamera nachfuehren, damit der Kopf ruhig bleibt */
                        car_pos[0] += (double)(cs_v[0] * dt); car_pos[2] += (double)(cs_v[2] * dt);
                        double kc = (double)clampf(dt * 3.0f, 0.0f, 1.0f);
                        car_pos[0] += (g[0] - car_pos[0]) * kc; car_pos[1] = g[1]; /* Hoehe fest am Auto (keine Geschwindigkeits-Rechnung, die an Haengen wegdriftet) */ car_pos[2] += (g[2] - car_pos[2]) * kc;
                    } else {
                        car_pos[0] = g[0]; car_pos[1] = g[1]; car_pos[2] = g[2];
                    }
                    have_car_pos = 1;
                } else if (cs_ok && have_car_pos && dt > 0.0f && dt < 0.1f) {
                    /* Spiel hat die Kamera diesmal nicht neu gesetzt: Auto-Position mit der Geschwindigkeit weiterrechnen */
                    car_pos[0] += (double)(cs_v[0] * dt); if (!passenger_on) car_pos[1] += (double)(cs_v[1] * dt); car_pos[2] += (double)(cs_v[2] * dt);
                }
            }
            {
                /* Fahrtrichtung des Autos: aus der Spiel-Kamera (wenn das Spiel sie selbst setzt), sonst aus der Bewegung */
                float gf[3];
                int fresh = 0;
                gfh_fresh = 0;
                if (memory_read(base + dir_off, gf, sizeof(gf))) {
                    float dgx = gf[0] - last_out_fwd[0], dgy = gf[1] - last_out_fwd[1], dgz = gf[2] - last_out_fwd[2];
                    float gl = my_sqrt(gf[0] * gf[0] + gf[2] * gf[2]);
                    if (dgx * dgx + dgy * dgy + dgz * dgz > 1.0e-8f && gl > 0.2f) { gfh_x = gf[0] / gl; gfh_z = gf[2] / gl; gfh_fresh = 1; }
                }
                if (cs_ok != cs_prev_ok && cs_lost_logs < 20) { ++cs_lost_logs; put(cs_ok ? "CARSTATE back\r\n" : "CARSTATE lost\r\n"); flush(); }
                cs_prev_ok = cs_ok;
                if (cs_ok) {
                    /* leicht glaetten, damit einzelne Ausreisser den Sitz nicht verrutschen lassen */
                    if (!cs_seen && pad_sign_pending) {
                        /* Start per Controller aus der Fahrer-Ansicht: die Kamera zeigt nach vorn, daran die Achse des Autos ausrichten */
                        cs_sign = (cs_fx * drone_hx + cs_fz * drone_hz) < 0.0f ? -1.0f : 1.0f;
                        pad_sign_pending = 0;
                    }
                    float k5 = cs_seen ? clampf(dt * 20.0f, 0.0f, 1.0f) : 1.0f;
                    float nx5 = car_hx + (cs_fx * cs_sign - car_hx) * k5, nz5 = car_hz + (cs_fz * cs_sign - car_hz) * k5;
                    float nl5 = my_sqrt(nx5 * nx5 + nz5 * nz5);
                    if (nl5 > 0.2f) { car_hx = nx5 / nl5; car_hz = nz5 / nl5; }
                    cs_seen = 1; fresh = 1;
                }
                else if (cs_seen) { fresh = 1; /* Karosseriedaten kurz weg: letzte Richtung halten, nicht die Kamera des Spiels nehmen */ }
                else if (memory_read(base + dir_off, gf, sizeof(gf))) {
                    float dfx = gf[0] - last_out_fwd[0], dfy = gf[1] - last_out_fwd[1], dfz = gf[2] - last_out_fwd[2];
                    float hl = my_sqrt(gf[0] * gf[0] + gf[2] * gf[2]);
                    if (dfx * dfx + dfy * dfy + dfz * dfz > 1.0e-8f && hl > 0.2f) { car_hx = gf[0] / hl; car_hz = gf[2] / hl; fresh = 1; }
                }
                if (car_prev_ok && dt > 0.0f) {
                    float k3 = clampf(dt * 4.0f, 0.0f, 1.0f);
                    float vx = (float)((car_pos[0] - car_prev_p[0]) / (double)dt), vz = (float)((car_pos[2] - car_prev_p[2]) / (double)dt);
                    if (vx > -150.0f && vx < 150.0f && vz > -150.0f && vz < 150.0f) { car_vx += (vx - car_vx) * k3; car_vz += (vz - car_vz) * k3; }
                    float sp2 = my_sqrt(car_vx * car_vx + car_vz * car_vz);
                    if (!fresh && sp2 > 3.0f) { car_hx = car_vx / sp2; car_hz = car_vz / sp2; }
                }
                if (cs_ok && dt > 0.0f && dt < 0.1f) {
                    /* Laengsbeschleunigung des Autos (m/s^2), geglaettet: positiv = Gas, negativ = Bremsen */
                    float vl = cs_v[0] * car_hx + cs_v[2] * car_hz;
                    if (have_prev_vl) {
                        float a = clampf((vl - prev_vl) / dt, -20.0f, 20.0f);
                        acc_s += (a - acc_s) * clampf(dt * 8.0f, 0.0f, 1.0f);
                    }
                    prev_vl = vl; have_prev_vl = 1;
                    /* Tempo, Kurvenbeschleunigung (zur Seite) und Staesse (Auf/Ab) fuers Beifahrer-Zittern */
                    float sp3 = my_sqrt(cs_v[0] * cs_v[0] + cs_v[2] * cs_v[2]);
                    spd_s += (sp3 - spd_s) * clampf(dt * 6.0f, 0.0f, 1.0f);
                    if (have_prev_c) {
                        float yr = -(pc_hx * car_hz - pc_hz * car_hx) / dt; /* >0 = Rechtskurve */
                        yr_s += (clampf(yr, -3.0f, 3.0f) - yr_s) * clampf(dt * 8.0f, 0.0f, 1.0f);
                        float la = clampf(yr_s * spd_s, -16.0f, 16.0f);
                        lat_s += (la - lat_s) * clampf(dt * 6.0f, 0.0f, 1.0f);
                        float va = clampf((cs_v[1] - prev_vy) / dt, -40.0f, 40.0f);
                        vacc_s += (va - vacc_s) * clampf(dt * 14.0f, 0.0f, 1.0f);
                    }
                    pc_hx = car_hx; pc_hz = car_hz; prev_vy = cs_v[1]; have_prev_c = 1;
                }
                car_prev_p[0] = car_pos[0]; car_prev_p[1] = car_pos[1]; car_prev_p[2] = car_pos[2]; car_prev_ok = 1;
            }
            have_last_game = 1;
        }
    }

    /* Zoom per Mausrad (Bild-hoch/Bild-runter als Ersatz) */
    int notches = wheel_accum;
    wheel_accum = 0;
    float steps = (float)notches / 120.0f;
    if (game_input_key_down(0x21)) steps += dt * 6.0f;
    if (game_input_key_down(0x22)) steps -= dt * 6.0f;
    zoom_target = clampf(zoom_target * (1.0f + 0.2f * steps), 0.5f, 50.0f);
    if (rear_on) zoom_target = 0.5f; /* 360-Kamera: immer 0,5x */
    {
        float zf = 1.0f, zs = (cinema_on && passenger_on) ? 2.2f : 8.0f;
        if (focus_on && walk_mode && have_car_pos) {
            /* Auto im Blick: weit weg -> heranzoomen, nah dran -> herauszoomen */
            float ax = (float)(car_pos[0] - drone_pos[0]), ay = (float)(car_pos[1] - drone_pos[1]), az = (float)(car_pos[2] - drone_pos[2]);
            float dist = my_sqrt(ax * ax + ay * ay + az * az);
            /* <= 3 m: 1x, >= 60 m: 20x, dazwischen weich ansteigend */
            float tz = clampf((dist - 3.0f) / 57.0f, 0.0f, 1.0f);
            zf = (1.0f + 19.0f * tz * my_sqrt(tz)) * (zoom_target > 0.0f ? 1.0f : 1.0f);
            zs = 7.0f;
        }
        float zt = (focus_on && walk_mode && have_car_pos) ? clampf(zf, 1.0f, 20.0f) : zoom_target;
        zoom_current += (zt - zoom_current) * clampf(dt * zs, 0.0f, 1.0f);
    }
    float look_scale = 1.0f / zoom_current;

    /* Umsehen: Maus (mit Neuzentrierung) + Pfeiltasten */
    if (!drone_input_enabled) {
        mouse_primed = 0; mouse_yaw_rate = 0.0f; mouse_pitch_rate = 0.0f;
    } else if (mouse_api_ready) {
        int cx, cy;
        window_center(&cx, &cy);
        POINT point;
        if (api_get_cursor(&point)) {
            if (mouse_primed) {
                float dx = (float)(point.x - cx), dy = (float)(point.y - cy);
                if ((dx > 300.0f || dx < -300.0f || dy > 300.0f || dy < -300.0f) && spike_logs < 40) {
                    ++spike_logs;
                    put("DRONE-MOUSE-SPIKE dx="); put_signed((int)dx); put(" dy="); put_signed((int)dy);
                    put(" center="); put_signed(cx); put(","); put_signed(cy); put("\r\n"); flush();
                    dx = 0.0f; dy = 0.0f;
                }
                float k = clampf(dt * 16.0f, 0.0f, 1.0f);
                mouse_yaw_rate += (dx * 0.0018f / dt - mouse_yaw_rate) * k;
                mouse_pitch_rate += (-dy * 0.0018f / dt - mouse_pitch_rate) * k;
            }
            if (point.x != cx || point.y != cy) api_set_cursor(cx, cy); /* nur wenn die Maus sich bewegt hat */
            mouse_primed = 1;
        }
    }
    if (walk_mode) {
        /* Handy: Blickrichtung folgt der Maus mit leichter Traegheit */
        pend_yaw += mouse_yaw_rate * dt * look_scale;
        pend_pitch += mouse_pitch_rate * dt * look_scale;
        float f = clampf(dt / 0.10f, 0.0f, 1.0f);
        rotate_heading(pend_yaw * f);
        drone_pitch = clampf(drone_pitch + pend_pitch * f, -1.2f, 1.2f);
        pend_yaw -= pend_yaw * f;
        pend_pitch -= pend_pitch * f;
    } else {
        pend_yaw = pend_pitch = 0.0f;
        rotate_heading(mouse_yaw_rate * dt * look_scale);
        drone_pitch = clampf(drone_pitch + mouse_pitch_rate * dt * look_scale, -1.45f, 1.45f);
    }
    if (focus_on && walk_mode && have_car_pos) {
        /* Auto im Blick halten: Kamera dreht sanft zum Auto */
        static double car_prev[3]; static float car_vel[3]; static int prev_ok;
        if (prev_ok && dt > 0.0f) {
            float k2 = clampf(dt * 8.0f, 0.0f, 1.0f);
            for (int i = 0; i < 3; ++i) {
                float v = (float)((car_pos[i] - car_prev[i]) / (double)dt);
                if (v > 120.0f || v < -120.0f) v = 0.0f;
                car_vel[i] += (v - car_vel[i]) * k2;
            }
        }
        for (int i = 0; i < 3; ++i) car_prev[i] = car_pos[i];
        prev_ok = 1;
        float fx = (float)(car_pos[0] - drone_pos[0]) + car_vel[0] * 0.15f;
        float fy = (float)(car_pos[1] - 0.25 - drone_pos[1]);
        float fz = (float)(car_pos[2] - drone_pos[2]) + car_vel[2] * 0.15f;
        float hd = my_sqrt(fx * fx + fz * fz);
        if (hd > 2.0f) {
            float nx = fx / hd, nz = fz / hd;
            float err = my_sqrt((nx - drone_hx) * (nx - drone_hx) + (nz - drone_hz) * (nz - drone_hz));
            float k = clampf(dt * (9.0f + 40.0f * err), 0.0f, 1.0f); /* je weiter das Auto aus der Mitte, desto schneller */
            float hx2 = drone_hx + (nx - drone_hx) * k, hz2 = drone_hz + (nz - drone_hz) * k;
            float hl = my_sqrt(hx2 * hx2 + hz2 * hz2);
            if (hl > 0.01f) { drone_hx = hx2 / hl; drone_hz = hz2 / hl; }
            drone_pitch = clampf(drone_pitch + (my_atan(fy / (hd > 4.0f ? hd : 4.0f)) - drone_pitch) * k, -1.2f, 1.2f);
            pend_yaw = pend_pitch = 0.0f;
        }
    }
    if (cinema_on && passenger_on && have_car_pos) {
        /* Cinema-Modus: zufaellige Regie - Einstellung, Dauer, Tempo, Zoom und Wandern wechseln jedes Mal */
        if (mouse_yaw_rate > 0.15f || mouse_yaw_rate < -0.15f || mouse_pitch_rate > 0.15f || mouse_pitch_rate < -0.15f) cin_pause = 2.0f;
        if (cin_pause > 0.0f) cin_pause -= dt;
        else {
            cin_t += dt;
            if (cin_dur <= 0.0f || cin_t >= cin_dur) {
                unsigned sd0 = cin_seed + (unsigned)cin_n * 7u;
                int ns = cin_shot;
                for (int tries = 0; tries < 10; ++tries) {
                    ns = (int)(vn_hash(sd0 + (unsigned)tries, 7) * 10.99f);
                    if (ns != cin_shot && ns != cin_prev) break;
                }
                cin_prev = cin_shot; cin_shot = ns;
                float h1 = vn_hash(sd0, 8);
                cin_dur = 1.6f + 7.0f * h1 * h1 + 1.2f * vn_hash(sd0, 14); /* viele kurze, ein paar lange */
                cin_b = (vn_hash(sd0, 9) - 0.5f) * 2.0f;
                cin_speed = 0.7f + 3.8f * vn_hash(sd0, 10);
                cin_z0 = 1.0f + 0.7f * vn_hash(sd0, 11) * vn_hash(sd0, 15);
                cin_z1 = 1.0f + 1.1f * vn_hash(sd0, 12);
                cin_wob = 0.04f + 0.20f * vn_hash(sd0, 13);
                if ((cin_shot == 7 || cin_shot == 0 || cin_shot == 3) && vn_hash(sd0, 16) < 0.4f) cin_dur *= 1.6f + 1.2f * vn_hash(sd0, 17); /* manchmal lange auf der Strasse bleiben */
                cin_t = 0.0f; ++cin_n;
            }
            float fxc = car_hx, fzc = car_hz;
            float rxc = car_hz, rzc = -car_hx; /* rechts */
            float sd = (float)passenger_side;
            float tx = fxc, tz = fzc, tp = 0.0f;
            float ph = cin_t / (cin_dur > 0.1f ? cin_dur : 0.1f);
            float tzoom = cin_z0 + (cin_z1 - cin_z0) * ph;
            float wy = cin_wob * (vnoise(20, cin_t * 0.45f + (float)cin_n * 5.0f) - 0.5f) * 2.0f;
            float wp = cin_wob * 0.5f * (vnoise(21, cin_t * 0.5f + (float)cin_n * 3.0f) - 0.5f) * 2.0f;
            if (cin_shot == 0) {
                /* Strasse voraus, schaut in Kurven hinein */
                float a = clampf(yr_s * 0.35f, -0.55f, 0.55f) + wy;
                float ca = my_cos(a), sa = my_sin(a);
                tx = fxc * ca + rxc * sa; tz = fzc * ca + rzc * sa;
                tp = -0.03f + wp;
            } else if (cin_shot == 1) {
                /* Fahrer: Blick quer zur Fahrerseite, mal mehr nach vorn, mal mehr nach hinten */
                float ex = -rxc * sd * 0.96f + fxc * (0.25f + 0.5f * cin_b), ez = -rzc * sd * 0.96f + fzc * (0.25f + 0.5f * cin_b);
                float el = my_sqrt(ex * ex + ez * ez);
                if (el > 0.01f) { tx = ex / el; tz = ez / el; }
                tp = 0.04f + wp; tzoom = 1.0f + (tzoom - 1.0f) * 0.5f;
            } else if (cin_shot == 2) {
                /* Aus dem Seitenfenster, gleitet von vorn nach aussen oder zurueck */
                float b = 1.15f + cin_b * 0.5f + (cin_b > 0.0f ? 1.0f : -1.0f) * 0.5f * (ph - 0.5f) + wy * 0.5f;
                float cb2 = my_cos(b), sb2 = my_sin(b);
                tx = fxc * cb2 + rxc * sd * sb2; tz = fzc * cb2 + rzc * sd * sb2;
                tp = 0.02f + wp;
            } else if (cin_shot == 3) {
                /* Ruhiger Blick nach vorn mit langsamem Wandern */
                float a = 0.25f * my_sin(cin_t * (0.5f + 0.3f * cin_b) + cin_b * 3.0f) + wy;
                float ca = my_cos(a), sa = my_sin(a);
                tx = fxc * ca + rxc * sa; tz = fzc * ca + rzc * sa;
                tp = -0.04f + 0.03f * my_sin(cin_t * 0.5f) + wp;
            } else if (cin_shot == 4) {
                /* Blick nach hinten ueber die Schulter */
                float ex = -fxc * 0.9f + rxc * sd * (0.35f + 0.3f * cin_b), ez = -fzc * 0.9f + rzc * sd * (0.35f + 0.3f * cin_b);
                float el = my_sqrt(ex * ex + ez * ez);
                if (el > 0.01f) { tx = ex / el; tz = ez / el; }
                tp = 0.04f + wp; tzoom = 1.0f;
            } else if (cin_shot == 6) {
                /* Langsamer Schwenk quer ueber die Windschutzscheibe */
                float e = ph * ph * (3.0f - 2.0f * ph);
                float a = (cin_b >= 0.0f ? 1.0f : -1.0f) * (-0.55f + 1.1f * e) + wy * 0.5f;
                float ca = my_cos(a), sa = my_sin(a);
                tx = fxc * ca + rxc * sa; tz = fzc * ca + rzc * sa;
                tp = -0.02f + wp;
            } else if (cin_shot == 7) {
                /* Tiefer Blick auf die Strasse, Kopf lehnt leicht vor */
                float a = clampf(yr_s * 0.25f, -0.4f, 0.4f) + wy * 0.5f;
                float ca = my_cos(a), sa = my_sin(a);
                tx = fxc * ca + rxc * sa; tz = fzc * ca + rzc * sa;
                tp = -0.12f + wp; tzoom = 1.2f + 0.6f * ph;
            } else if (cin_shot == 8) {
                /* Ueber den Fahrer hinweg aus dem Fahrerfenster, gleitet nach vorn */
                float b = 1.2f - 0.7f * ph + cin_b * 0.2f;
                float cb2 = my_cos(b), sb2 = my_sin(b);
                tx = fxc * cb2 - rxc * sd * sb2; tz = fzc * cb2 - rzc * sd * sb2;
                tp = 0.01f + wp;
            } else if (cin_shot == 9) {
                /* Langsames Aufschauen: von der Strasse zum Horizont / Himmel */
                float e = ph * ph * (3.0f - 2.0f * ph);
                float a = 0.15f * cin_b + wy * 0.5f;
                float ca = my_cos(a), sa = my_sin(a);
                tx = fxc * ca + rxc * sa; tz = fzc * ca + rzc * sa;
                tp = -0.08f + 0.38f * e; tzoom = cin_z0;
            } else if (cin_shot == 10) {
                /* Vom Fahrer zurueck auf die Strasse schwenken */
                float e = ph * ph * (3.0f - 2.0f * ph);
                float ex = -rxc * sd * (0.96f * (1.0f - e)) + fxc * (0.25f + 0.75f * e), ez = -rzc * sd * (0.96f * (1.0f - e)) + fzc * (0.25f + 0.75f * e);
                float el = my_sqrt(ex * ex + ez * ez);
                if (el > 0.01f) { tx = ex / el; tz = ez / el; }
                tp = 0.04f - 0.07f * e + wp; tzoom = 1.0f + 0.3f * e;
            } else {
                /* Blick nach unten zu den Haenden / Armaturen des Fahrers */
                float ex = fxc * 0.6f - rxc * sd * (0.5f + 0.2f * cin_b), ez = fzc * 0.6f - rzc * sd * (0.5f + 0.2f * cin_b);
                float el = my_sqrt(ex * ex + ez * ez);
                if (el > 0.01f) { tx = ex / el; tz = ez / el; }
                tp = -0.28f + wp; tzoom = 1.0f + (tzoom - 1.0f) * 0.5f;
            }
            /* weiche Kamerafuehrung: gedaempfte Feder auf Winkel (langsames Anfahren und Abbremsen, keine Spruenge) */
            {
                float w = 0.5f + cin_speed * 0.32f;
                float cr3 = drone_hx * tz - drone_hz * tx, cd3 = drone_hx * tx + drone_hz * tz;
                float ea = -my_atan2(cr3, cd3);
                float dts = clampf(dt, 0.0f, 0.05f);
                cin_yv += (w * w * ea - 2.0f * w * cin_yv) * dts;
                cin_yv = clampf(cin_yv, -0.9f, 0.9f);
                rotate_heading(cin_yv * dts);
                cin_pv += (w * w * (tp - drone_pitch) - 2.0f * w * cin_pv) * dts;
                cin_pv = clampf(cin_pv, -0.6f, 0.6f);
                drone_pitch += cin_pv * dts;
            }
            /* Koerper-Animation: Kopf wandert seitlich/vor-zurueck und neigt sich langsam */
            {
                float kb = clampf(dt * 1.1f, 0.0f, 1.0f);
                float tl = 0.14f * my_sin(cin_t * 0.35f + cin_b * 4.0f) + 0.07f * cin_b;
                float tf = 0.06f * my_sin(cin_t * 0.27f + 2.0f) + (cin_shot == 7 ? 0.12f : 0.0f);
                float tr = 0.022f * my_sin(cin_t * 0.5f + cin_b * 2.0f) + 0.012f * my_sin(cin_t * 1.1f + 1.0f);
                cin_lat += (tl - cin_lat) * kb; cin_fw += (tf - cin_fw) * kb; cin_rl += (tr - cin_rl) * kb;
            }
            zoom_target = tzoom;
            pend_yaw = pend_pitch = 0.0f;
        }
    } else {
        cin_dur = 0.0f; cin_pause = 0.0f; cin_yv = cin_pv = 0.0f;
        { float kb = clampf(dt * 2.0f, 0.0f, 1.0f); cin_lat -= cin_lat * kb; cin_fw -= cin_fw * kb; cin_rl -= cin_rl * kb; }
    }
    float yaw_in = axis(0x27, 0, 0x25, 0);
    float pitch_in = axis(0x26, 0, 0x28, 0);
    float smooth = clampf(dt * 8.0f, 0.0f, 1.0f);
    look_yaw_rate += (yaw_in * 1.3f - look_yaw_rate) * smooth;
    look_pitch_rate += (pitch_in * 1.0f - look_pitch_rate) * smooth;
    rotate_heading(look_yaw_rate * dt * look_scale);
    drone_pitch = clampf(drone_pitch + look_pitch_rate * dt * look_scale, -1.45f, 1.45f);

    /* Bewegung: W/S vor-zurueck, A/D seitlich, E/R hoch, Q/F runter, Shift = schnell */
    float fast = game_input_key_down(0x11) ? 0.08f : (game_input_key_down(0x10) ? 4.0f : 1.0f); /* Strg = sehr langsam */
    float speed = 14.0f * fast;
    float move_f = axis(0x57, 0, 0x53, 0);
    float move_r = axis(0x44, 0, 0x41, 0);
    float move_u = axis(0x45, 0x52, 0x51, 0x46);
    float cp = my_cos(drone_pitch), sp = my_sin(drone_pitch);
    float target[3];
    target[0] = (cp * drone_hx * move_f + drone_hz * move_r) * speed;
    target[1] = (sp * move_f + move_u) * speed;
    target[2] = (cp * drone_hz * move_f - drone_hx * move_r) * speed;
    if (walk_mode) {
        /* Gehen: nur waagerecht, Tempo 1,5 m/s, Shift = rennen, Strg = ganz langsam */
        float walk_speed = game_input_key_down(0x11) ? 0.5f : (game_input_key_down(0x10) ? 3.8f : 1.5f);
        target[0] = (drone_hx * move_f + drone_hz * move_r) * walk_speed;
        target[1] = 0.0f;
        target[2] = (drone_hz * move_f - drone_hx * move_r) * walk_speed;
        if (follow_on && have_car_pos) {
            /* Hinterherlaufen: ca. 15 km/h (4,2 m/s) zum Auto, ab 10 m Abstand */
            float cx = (float)(car_pos[0] - drone_pos[0]), cz = (float)(car_pos[2] - drone_pos[2]);
            float cd = my_sqrt(cx * cx + cz * cz);
            if (cd > 10.0f) { target[0] = cx / cd * 4.2f; target[2] = cz / cd * 4.2f; }
        }
        if (have_car_pos) {
            double cdx = car_pos[0] - trail_last_x, cdz = car_pos[2] - trail_last_z;
            if (trail_n == 0 || cdx * cdx + cdz * cdz > 2.25) trail_push(car_pos[0], car_pos[2], car_pos[1] - 1.15);
        }
        if (trail_n > 0) {
            int best = -1; float bd = 6400.0f; /* nur Spurpunkte innerhalb von 80 m */
            for (int ti = 0; ti < trail_n; ++ti) {
                float ex = trail_x[ti] - (float)drone_pos[0], ez = trail_z[ti] - (float)drone_pos[2];
                float dd = ex * ex + ez * ez;
                if (dd < bd) { bd = dd; best = ti; }
            }
            if (best >= 0 && bd < 64.0f) {
                /* nah an der Fahrspur (<8 m): Boden aus der Spur */
                ground_ref += ((double)trail_g[best] - ground_ref) * (double)clampf(dt * 5.0f, 0.0f, 1.0f);
            } else {
                /* weiter weg: Boden unbekannt -> Hoehe halten, Q/E verschiebt sie direkt (Shift schneller) */
                ground_ref += (double)(move_u * (game_input_key_down(0x10) ? 6.0f : 2.5f) * dt);
            }
        }
        eye_height = clampf(eye_height + move_u * 0.6f * dt, 0.4f, rear_on ? 4.5f : 1.9f);
        float ws = clampf(dt * 5.0f, 0.0f, 1.0f);
        dt_smooth = (dt_smooth <= 0.0f || dt > 0.1f) ? dt : dt_smooth + (dt - dt_smooth) * 0.15f;
        drone_vel[0] += (target[0] - drone_vel[0]) * ws;
        drone_vel[1] = 0.0f;
        drone_vel[2] += (target[2] - drone_vel[2]) * ws;
        drone_pos[0] += (double)(drone_vel[0] * dt_smooth);
        drone_pos[2] += (double)(drone_vel[2] * dt_smooth);
        drone_pos[1] += (ground_ref + (double)eye_height - drone_pos[1]) * (double)clampf(dt * 4.0f, 0.0f, 1.0f);
        if (passenger_on && have_car_pos) {
            /* Beifahrer: mit dem Auto mitdrehen, Position = Fahrerauge seitlich versetzt */
            float cr = pch_x * car_hz - pch_z * car_hx; /* -sin(Drehung) */
            float cd = pch_x * car_hx + pch_z * car_hz;
            if (cd > 0.1f) rotate_heading(-my_atan(cr / cd));
            if (!rear_on && !cinema_on) {
                /* Blick im Auto begrenzen: Beifahrer ca. 75 Grad, Fahrer ca. 100 Grad nach links/rechts, nie nach hinten */
                float crd = drone_hx * car_hz - drone_hz * car_hx, cdd = drone_hx * car_hx + drone_hz * car_hz;
                float rel = my_atan2(crd, cdd), lim = drv_on ? 1.75f : 1.30f;
                if (rel > lim) rotate_heading(lim - rel); else if (rel < -lim) rotate_heading(-lim - rel);
            }
            if (drv_on) {
                /* Fahrer-Kamera: Maus loslassen -> Blick geht weich wieder nach vorn */
                static float idle_t;
                float act = my_fabs_f(mouse_yaw_rate) + my_fabs_f(mouse_pitch_rate) + my_fabs_f(look_yaw_rate) + my_fabs_f(look_pitch_rate);
                if (act > 0.05f) idle_t = 0.0f; else idle_t += dt;
                if (idle_t > 0.35f) {
                    float crd = drone_hx * car_hz - drone_hz * car_hx, cdd = drone_hx * car_hx + drone_hz * car_hz;
                    float k = clampf(dt * 3.0f, 0.0f, 1.0f);
                    rotate_heading(-my_atan2(crd, cdd) * k);
                    drone_pitch -= drone_pitch * k;
                }
            }
            pch_x = car_hx; pch_z = car_hz;
            double rx = (double)car_hz, rz = (double)(-car_hx);
            /* WASD verschiebt den Sitz (jeder Sitz ist anders); die Position bleibt danach gespeichert */
            seat_f = clampf(seat_f + move_f * 0.6f * dt, rear_on ? -6.0f : -2.0f, 2.0f);
            seat_r = clampf(seat_r + move_r * 0.6f * dt, -1.6f, 1.6f);
            if (seat_r > 0.15f) passenger_side = 1; else if (seat_r < -0.15f) passenger_side = -1;
            double off = (double)seat_r;
            drone_pos[0] = car_pos[0] + rx * off + (double)(car_hx * seat_f);
            drone_pos[2] = car_pos[2] + rz * off + (double)(car_hz * seat_f);
            /* Sitzhoehe: folgt dem Auto ueber seine Vertikalgeschwindigkeit und gleicht nur sehr langsam mit der Spiel-Kamera ab,
               damit Hoehenaenderungen der Spiel-Kamera dich nicht von selbst hoch/runter schieben. Deine Q/E-Hoehe (eye_height) bleibt gespeichert. */
            if (!seat_ok) { seat_y = car_pos[1]; seat_ok = 1; }
            else {
                seat_y = car_pos[1];
            }
            drone_pos[1] = seat_y + (double)(eye_height - 1.6f) + 0.05 + (double)(seat_f * my_sin(car_pitch_s));
            drone_vel[0] = drone_vel[1] = drone_vel[2] = 0.0f;
            walk_amp = 0.0f;
            {
                /* Kopf: Feder-Daempfer, nickt bei Gas nach hinten und beim Bremsen nach vorn, mit leichtem Nachschwingen */
                float tgt = rear_on ? 0.0f : clampf(acc_s * 0.014f, -0.20f, 0.20f);
                head_v += ((tgt - head_p) * 45.0f - head_v * 9.0f) * dt;
                head_p = clampf(head_p + head_v * dt, -0.30f, 0.30f);
            }
            /* Reichweiten-Grenze folgt dem Auto, sonst haelt sie dich bei hohem Tempo nach 400 m an der Stelle fest */
            drone_anchor[0] = car_pos[0]; drone_anchor[1] = car_pos[1]; drone_anchor[2] = car_pos[2];
        }
    } else {
    float vs = clampf(dt * 3.5f, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i) {
        drone_vel[i] += (target[i] - drone_vel[i]) * vs;
        drone_pos[i] += (double)(drone_vel[i] * dt);
    }
    }
    {
        /* Reichweite: nicht weiter als 120 m vom Startpunkt (Auto), sonst setzt das Spiel die Kamera zurueck */
        double ox = drone_pos[0] - drone_anchor[0], oy = drone_pos[1] - drone_anchor[1], oz = drone_pos[2] - drone_anchor[2];
        double d2 = ox * ox + oy * oy + oz * oz;
        const double R = 400.0;
        if (d2 > R * R) {
            double d = (double)my_sqrt((float)d2);
            double k = R / d;
            drone_pos[0] = drone_anchor[0] + ox * k; drone_pos[1] = drone_anchor[1] + oy * k; drone_pos[2] = drone_anchor[2] + oz * k;
            float nx = (float)(ox / d), ny = (float)(oy / d), nz = (float)(oz / d);
            float vn = drone_vel[0] * nx + drone_vel[1] * ny + drone_vel[2] * nz;
            if (vn > 0.0f) { drone_vel[0] -= vn * nx; drone_vel[1] -= vn * ny; drone_vel[2] -= vn * nz; }
        }
    }

    /* Leichtes Drohnen-Schweben: sehr sanft, bei Tempo etwas mehr */
    float v2 = drone_vel[0] * drone_vel[0] + drone_vel[1] * drone_vel[1] + drone_vel[2] * drone_vel[2];
    float speed_frac = clampf(my_sqrt(v2) / 56.0f, 0.0f, 1.0f);
    float drift = (0.0009f + 0.0014f * speed_frac) * look_scale;
    float tremor = 0.00012f * look_scale;
    phase[0] = wrap_pi(phase[0] + dt * 2.0f * PI_F * 0.23f);
    phase[1] = wrap_pi(phase[1] + dt * 2.0f * PI_F * 0.61f);
    phase[2] = wrap_pi(phase[2] + dt * 2.0f * PI_F * 0.37f);
    phase[3] = wrap_pi(phase[3] + dt * 2.0f * PI_F * 6.3f);
    phase[4] = wrap_pi(phase[4] + dt * 2.0f * PI_F * 7.7f);
    phase[5] = wrap_pi(phase[5] + dt * 2.0f * PI_F * 0.31f);
    float shake_yaw = drift * (0.65f * my_sin(phase[0]) + 0.35f * my_sin(phase[1] + 1.3f)) +
                      tremor * my_sin(phase[3]);
    float shake_pitch = drift * (0.65f * my_sin(phase[5] + 2.1f) + 0.35f * my_sin(phase[2])) +
                        tremor * my_sin(phase[4]);
    float bob = (0.004f + 0.008f * speed_frac) * my_sin(phase[1] + 0.7f);
    float lateral = 0.0f;
    float roll_out = 0.0f;
    if (walk_mode) {
        float sp_h = my_sqrt(drone_vel[0] * drone_vel[0] + drone_vel[2] * drone_vel[2]);
        float amp_target = clampf(sp_h / 1.5f, 0.0f, 1.7f);
        walk_amp += (amp_target - walk_amp) * clampf(dt * 3.0f, 0.0f, 1.0f);
        walk_phase += dt * 2.0f * PI_F * (1.0f + 0.5f * sp_h);
        if (walk_phase > 4.0f * PI_F) walk_phase -= 4.0f * PI_F; /* Periode 4 pi: auch der halbe Takt bleibt stetig */
        t_noise += dt;
        /* mal staerker, mal schwaecher: langsam wechselnde Zufallsstaerke je Bewegungsart */
        float env_w = 0.45f + 1.25f * vnoise(3, t_noise * 0.35f);
        float env_h = 0.35f + 1.50f * vnoise(2, t_noise * 0.45f);
        float env_r = 0.30f + 1.60f * vnoise(1, t_noise * 0.55f);
        float wa = walk_amp * (float)walk_level_pct * 0.01f * env_w;
        float hand = (shake_on && !rear_on) ? 0.010f * look_scale * env_h * (passenger_on ? 0.75f : 1.0f) : 0.0f;
        float tre = shake_on ? 0.0005f * look_scale : 0.0f;
        shake_yaw = hand * (0.6f * my_sin(phase[0]) + 0.4f * my_sin(phase[1] + 1.3f)) + tre * my_sin(phase[3]) +
                    wa * 0.020f * look_scale * my_sin(walk_phase * 0.5f + 0.5f);
        shake_pitch = hand * (0.6f * my_sin(phase[5] + 2.1f) + 0.4f * my_sin(phase[2])) + tre * my_sin(phase[4]) +
                      wa * 0.007f * look_scale * my_sin(walk_phase + 1.0f);
        bob = (shake_on ? 0.003f : 0.0f) * my_sin(phase[0] * 1.3f) + wa * 0.030f * my_sin(walk_phase);
        lateral = wa * 0.045f * my_sin(walk_phase * 0.5f) + (shake_on ? 0.007f * my_sin(phase[2] + 0.4f) : 0.0f);
        /* Kippen wie beim Handy: Schaukeln beim Gehen, leichtes Wandern im Stand, Neigung beim schnellen Schwenken */
        /* Tatsaechliche Schwenkgeschwindigkeit (Maus oder Auto-Fokus): aus der Blickrichtung gemessen */
        float pan_raw = dt > 0.0f ? -(prev_hx * drone_hz - prev_hz * drone_hx) / dt : 0.0f;
        prev_hx = drone_hx; prev_hz = drone_hz;
        pan_rate_s += (clampf(pan_raw, -6.0f, 6.0f) - pan_rate_s) * clampf(dt * 12.0f, 0.0f, 1.0f);
        /* Aus dem Referenzvideo: Neigung ca. 0,03 Grad pro Grad/s Schwenk, mit ~0,1 s Verzoegerung; Kippen insgesamt ca. 2 Grad */
        float bank_target = clampf(pan_rate_s * 0.04f, -0.07f, 0.07f);
        bank_roll += (bank_target - bank_roll) * clampf(dt * 8.0f, 0.0f, 1.0f);
        rphase[0] = wrap_pi(rphase[0] + dt * 2.0f * PI_F * 1.3f);
        rphase[1] = wrap_pi(rphase[1] + dt * 2.0f * PI_F * 2.2f);
        rphase[2] = wrap_pi(rphase[2] + dt * 2.0f * PI_F * 0.8f);
        roll_out = bank_roll + wa * 0.03f * my_sin(walk_phase * 0.5f + 1.7f);
        if (shake_on) {
            roll_out += env_r * (0.014f * my_sin(rphase[0]) + 0.010f * my_sin(rphase[1] + 1.0f) + 0.012f * my_sin(rphase[2] + 2.0f));
        }
    }

    if (passenger_on && shake_on) {
        float rf = rear_on ? 0.8f : 1.0f; /* 360-Kamera am Stab: Tempo, Kurven und Bodenwellen wirken, aber etwas sanfter */
        /* Beifahrer: Zittern je nach Lage - Tempo (ab ~150 km/h deutlich, bei 300 km/h kraeftig), Kurven, Huckel */
        float vf = clampf((spd_s - 30.0f) / 60.0f, 0.0f, 1.0f); /* ab ~90 km/h, bei 200 km/h etwa halb so stark wie vorher */
        float amp = (vf * vf * (0.6f + 0.8f * vnoise(4, t_noise * 1.3f)) + 0.05f * vf) * 0.4f * (cinema_on ? 0.5f : 1.0f) * rf;
        pv[0] = wrap_pi(pv[0] + dt * 2.0f * PI_F * 13.0f);
        pv[1] = wrap_pi(pv[1] + dt * 2.0f * PI_F * 19.0f);
        pv[2] = wrap_pi(pv[2] + dt * 2.0f * PI_F * 27.0f);
        shake_pitch += amp * (0.0040f * my_sin(pv[0]) + 0.0025f * my_sin(pv[1] + 1.0f));
        shake_yaw += amp * 0.0030f * my_sin(pv[2] + 2.0f);
        roll_out += amp * (0.0060f * my_sin(pv[1] + 0.5f) + 0.0035f * my_sin(pv[2]));
        bob += amp * 0.0022f * my_sin(pv[2] + 0.7f);
        lateral += amp * 0.0016f * my_sin(pv[0] + 1.0f);
        /* Kurven: Kopf kippt und rutscht nach aussen (Fliehkraft), staerker bei schneller Kurve */
        lateral -= lat_s * 0.0100f * rf;
        roll_out -= lat_s * 0.0040f * rf;
        shake_yaw -= lat_s * 0.0008f * rf;
        /* Huckel / Bodenwellen: Kopf federt auf und ab, Kamera nickt leicht mit */
        float btgt = clampf(-vacc_s * 0.0003f * rf, -0.015f, 0.015f);
        bump_v += ((btgt - bump_p) * 160.0f - bump_v * 14.0f) * dt;
        bump_p = clampf(bump_p + bump_v * dt, -0.02f, 0.02f);
        bob += bump_p;
        shake_pitch += bump_p * 0.5f;
    } else { bump_p = 0.0f; bump_v = 0.0f; }
    if (!shake_on && !walk_mode) { shake_yaw = shake_pitch = bob = lateral = 0.0f; }
    cur_roll = roll_out + cin_rl;
    if (!walk_mode) bank_roll = 0.0f;
    float sc = my_cos(shake_yaw), ss = my_sin(shake_yaw);
    float hx = sc * drone_hx + ss * drone_hz;
    float hz = sc * drone_hz - ss * drone_hx;
    if (!passenger_on) { head_p = 0.0f; head_v = 0.0f; }
    float pitch = clampf(drone_pitch + shake_pitch + (passenger_on ? car_pitch_s : 0.0f) + (passenger_on ? head_p * ((cinema_on && passenger_on) ? 0.4f : 1.0f) : 0.0f), -1.5f, 1.5f);
    float out_forward[3];
    float cp2 = my_cos(pitch);
    out_forward[0] = cp2 * hx;
    out_forward[1] = my_sin(pitch);
    out_forward[2] = cp2 * hz;

    double out_pos[3] = {drone_pos[0] + (double)(drone_hz * (lateral + cin_lat) + car_hx * cin_fw), drone_pos[1] + (double)bob, drone_pos[2] - (double)(drone_hx * (lateral + cin_lat)) + (double)(car_hz * cin_fw)};
    if (memory_is_writable(base + pos_off, 0x18) && memory_is_writable(base + dir_off, 0xc)) {
        memory_copy(base + pos_off, &out_pos[0], sizeof(double));
        memory_copy(base + pos_off + 8, &out_pos[1], sizeof(double));
        memory_copy(base + pos_off + 16, &out_pos[2], sizeof(double));
        memory_copy(base + dir_off, out_forward, sizeof(out_forward));
        last_written[0] = out_pos[0]; last_written[1] = out_pos[1]; last_written[2] = out_pos[2];
        last_out_fwd[0] = out_forward[0]; last_out_fwd[1] = out_forward[1]; last_out_fwd[2] = out_forward[2];
        have_written = 1;
        drone_camera_ptr = camera;
    }
    {
        static double pp[3]; static float ph[3]; static int have_prev, big_logs;
        double ex = drone_pos[0] - pp[0], ey = drone_pos[1] - pp[1], ez = drone_pos[2] - pp[2];
        float dh = drone_hx * ph[0] + drone_hz * ph[1];
        if (have_prev && big_logs < 80 && (ex * ex + ey * ey + ez * ez > 4.0 || dh < 0.97f || drone_pitch - ph[2] > 0.15f || ph[2] - drone_pitch > 0.15f)) {
            ++big_logs;
            put("DRONE-OWN-JUMP t="); put_number((unsigned)GetTickCount64());
            put(" move(dm)="); put_signed((int)(ex * 10)); put(","); put_signed((int)(ey * 10)); put(","); put_signed((int)(ez * 10));
            put(" headcos(x100)="); put_signed((int)(dh * 100.0f)); put(" dt_us="); put_number((unsigned)(dt * 1000000.0f));
            put(" yawrate(mrad/s)="); put_signed((int)(mouse_yaw_rate * 1000.0f)); put(" zoom="); put_signed((int)(zoom_current * 100.0f));
            put("\r\n"); flush();
        }
        pp[0] = drone_pos[0]; pp[1] = drone_pos[1]; pp[2] = drone_pos[2]; ph[0] = drone_hx; ph[1] = drone_hz; ph[2] = drone_pitch; have_prev = 1;
    }
    if (state_logs_left) {
        --state_logs_left;
        put("DRONE-STATE t="); put_number((unsigned)GetTickCount64());
        put(" view="); put_number(view);
        put(" cam="); put_hex((uint64_t)(uintptr_t)camera & 0xffffffffu, 8);
        put(" drone="); put_signed((int)drone_pos[0]); put(","); put_signed((int)drone_pos[1]); put(","); put_signed((int)drone_pos[2]);
        put(" game_g="); put_signed((int)state_g[0]); put(","); put_signed((int)state_g[1]); put(","); put_signed((int)state_g[2]);
        put(" vel="); put_signed((int)drone_vel[0]); put(","); put_signed((int)drone_vel[1]); put(","); put_signed((int)drone_vel[2]);
        put(" dt_us="); put_number((unsigned)(dt * 1000000.0f));
        put("\r\n");
        flush();
    }
}


void drone_probe_note_caller(int kind, uintptr_t caller, uintptr_t image_base) { (void)kind; (void)caller; (void)image_base; }

static void *dir_last_inner[2];
static void *dir_last_pending[2];
static unsigned dir_logs;

void drone_probe_director(void *director, uintptr_t image_base) {
    if (dir_logs >= 200) return;
    unsigned char *d = (unsigned char *)director;
    void *inner = 0, *pending = 0, *vtable = 0, *dvt = 0;
    memory_read(d + 0x208, &inner, sizeof(inner));
    memory_read(d + 0x1f0, &pending, sizeof(pending));
    memory_read(d, &dvt, sizeof(dvt));
    unsigned slot = ((uintptr_t)dvt - image_base == 0x2e70080u) ? 0u : 1u;
    if (inner == dir_last_inner[slot] && pending == dir_last_pending[slot]) return;
    dir_last_inner[slot] = inner;
    dir_last_pending[slot] = pending;
    if (inner) memory_read(inner, &vtable, sizeof(vtable));
    ++dir_logs;
    put("DRONE-DIRECTOR t="); put_number((unsigned)GetTickCount64());
    put(" dir_vt=0x"); put_hex((uint64_t)((uintptr_t)dvt - image_base), 8);
    put(" inner_obj=0x"); put_hex((uint64_t)(uintptr_t)inner & 0xffffffffu, 8);
    put(" inner_vt=0x"); put_hex((uint64_t)((uintptr_t)vtable - image_base), 8);
    put(" pending_obj=0x"); put_hex((uint64_t)(uintptr_t)pending & 0xffffffffu, 8);
    put("\r\n"); flush();
    if (inner && vtable) {
        /* unbekannte Kameraklasse (Replay, Foto-Modus ...): einmalig Tabelle und Objekt auflisten */
        static const unsigned known[] = {0x2e78120u, 0x2e77ea8u, 0x2e703b0u, 0x2e783a0u, 0x2e78610u, 0x2e78880u, 0x2e70628u, 0x2e77c30u};
        static unsigned dumped[12]; static int ndumped;
        unsigned rva = (unsigned)((uintptr_t)vtable - image_base);
        int skip = 0;
        for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); ++i) if (known[i] == rva) skip = 1;
        for (int i = 0; i < ndumped; ++i) if (dumped[i] == rva) skip = 1;
        if (!skip && ndumped < 12) {
            dumped[ndumped++] = rva;
            put("CAMCLASS vt=0x"); put_hex(rva, 8); put(" slots:");
            for (int i = 0; i < 20; ++i) {
                unsigned long long sl = 0;
                if (!memory_read((unsigned char *)vtable + i * 8, &sl, 8)) break;
                put(" "); put_hex((uint64_t)(sl - (unsigned long long)image_base), 8);
            }
            put("\r\n");
            for (int off = 0; off < 0x240; off += 32) {
                unsigned w[8];
                if (!memory_read((unsigned char *)inner + off, w, 32)) break;
                put("CAMOBJ+"); put_hex((uint64_t)off, 4); put(":");
                for (int i = 0; i < 8; ++i) { put(" "); put_hex((uint64_t)w[i], 8); }
                put("\r\n");
            }
            flush();
        }
    }
}

static unsigned before_logs;

void drone_probe_getter_diff(int kind, const double native[3], const double ours[3]) { (void)kind; (void)native; (void)ours; }

void drone_probe_before_update(void *camera) {
    if (!drone_on || !have_written || camera != drone_camera_ptr || before_logs >= 60) return;
    double now[3];
    unsigned char *b = (unsigned char *)camera;
    if (memory_read(b + 0x190, &now[0], 8) && memory_read(b + 0x198, &now[1], 8) && memory_read(b + 0x1a0, &now[2], 8)) {
        double dx = now[0] - last_written[0], dy = now[1] - last_written[1], dz = now[2] - last_written[2];
        if (dx * dx + dy * dy + dz * dz > 0.04) {
            ++before_logs;
            if (before_logs == 1 && !state_logs_left) state_logs_left = 50;
            put("DRONE-MOVED-BEFORE-UPDATE t="); put_number((unsigned)GetTickCount64());
            put(" delta(m)="); put_signed((int)dx); put(","); put_signed((int)dy); put(","); put_signed((int)dz);
            put("\r\n"); flush();
        }
    }
}

int drone_probe_override_position(void *camera, double out[3]) {
    if (!drone_on || !have_written || camera != drone_camera_ptr) {
        return 0;
    }
    out[0] = last_written[0];
    out[1] = last_written[1];
    out[2] = last_written[2];
    return 1;
}

void drone_probe_reset_seen(unsigned view) {
    if (drone_on) {
        put("DRONE-CAMERA-RESET view="); put_number(view); put("\r\n"); flush();
    }
}

static void log_foreground(void) {
    HMODULE u = LoadLibraryExW(L"user32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!u) return;
    HWND (WINAPI *fg)(void) = (HWND (WINAPI *)(void))(uintptr_t)GetProcAddress(u, "GetForegroundWindow");
    int (WINAPI *cls)(HWND, char *, int) = (int (WINAPI *)(HWND, char *, int))(uintptr_t)GetProcAddress(u, "GetClassNameA");
    int (WINAPI *txt)(HWND, char *, int) = (int (WINAPI *)(HWND, char *, int))(uintptr_t)GetProcAddress(u, "GetWindowTextA");
    DWORD (WINAPI *pidf)(HWND, LPDWORD) = (DWORD (WINAPI *)(HWND, LPDWORD))(uintptr_t)GetProcAddress(u, "GetWindowThreadProcessId");
    if (!fg) return;
    HWND w = fg();
    char c[64] = {0}, x[64] = {0}; DWORD pid = 0;
    if (w) { if (cls) cls(w, c, 63); if (txt) txt(w, x, 63); if (pidf) pidf(w, &pid); }
    put("DRONE-FOREGROUND hwnd="); put_hex((uint64_t)(uintptr_t)w, 8); put(" pid="); put_number((unsigned)pid);
    put(" class="); put(c); put(" title="); put(x); put("\r\n"); flush();
}

/* Messung: wie viel Zeit der Mod pro Frame braucht (Log alle 600 Frames im Drohnen-Modus) */
typedef BOOL (WINAPI *FnQPerf)(LARGE_INTEGER *);
static unsigned long long perf_now_us(void) {
    static FnQPerf pc, pf; static LARGE_INTEGER freq; static int init;
    if (!init) {
        init = 1;
        HMODULE k = LoadLibraryExW(L"kernel32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (k) { pc = (FnQPerf)(uintptr_t)GetProcAddress(k, "QueryPerformanceCounter"); pf = (FnQPerf)(uintptr_t)GetProcAddress(k, "QueryPerformanceFrequency"); }
        if (pf) pf(&freq);
    }
    LARGE_INTEGER n; n.QuadPart = 0;
    if (!pc || freq.QuadPart <= 0) return 0;
    pc(&n);
    return (unsigned long long)n.QuadPart * 1000000ull / (unsigned long long)freq.QuadPart;
}

void drone_probe_frame(void *camera, unsigned view, const unsigned char *image, int after_update, float dt) {
    {
        static int slots_logged;
        if (!slots_logged) { slots_logged = 1; patch_engine_log_slots(); }
    }
    (void)image;
    overlay_start();
    int focused = autodrive_game_window_focused();
    if (focused != last_focus) {
        put(focused ? "DRONE-FOCUS gained t=" : "DRONE-FOCUS LOST t="); put(drone_on ? "(drone on) " : "(drone OFF) ");
        put_number((unsigned)GetTickCount64());
        put("\r\n"); flush();
        if (!focused) log_foreground();
    }
    last_focus = focused;
    if (!after_update) {
        if (drone_on && have_written && view <= 3 && camera == drone_camera_ptr && moved_logs < 60) {
            double now[3];
            unsigned char *b = (unsigned char *)camera;
            if (memory_read(b + 0x190, &now[0], 8) && memory_read(b + 0x198, &now[1], 8) &&
                memory_read(b + 0x1a0, &now[2], 8)) {
                double dx = now[0] - last_written[0], dy = now[1] - last_written[1], dz = now[2] - last_written[2];
                if (dx * dx + dy * dy + dz * dz > 0.04) {
                    ++moved_logs;
                    put("DRONE-MOVED-BY-GAME(at heading hook) t="); put_number((unsigned)GetTickCount64()); put(" delta="); put_signed((int)(dx * 10.0)); put(","); put_signed((int)(dy * 10.0)); put(","); put_signed((int)(dz * 10.0));
                    put(" (zehntel meter)\r\n"); flush();
                }
            }
        }
        return;
    }
    int toggle = game_input_key_down(KEY_TOGGLE);
    if (drone_on && view <= 3) {
        drone_stop(); /* Aussenkamera nicht unterstuetzt: nur Ego-Ansicht */
    }
    if (toggle && !toggle_was_down && focused) {
        if (drone_on) {
            drone_stop();
        } else if (view > 3 && view < 7) {
            drone_start(camera, view);
        }
    }
    toggle_was_down = toggle;
    static int req_m, req_x, req_h;
    static unsigned long long pad_arm;
    if (pad_arm) {
        if (drone_on || GetTickCount64() - pad_arm > 3000ull) pad_arm = 0;
        else if (view == 4) { /* nur aus der Fahrer-Ansicht (Haube/Stossstange sitzen versetzt) */
            drone_start(camera, view);
            if (drone_on) { req_m = 1; pad_sign_pending = 1; }
            pad_arm = 0;
        }
    }
    {
        /* Controller: LB (allein) schaltet der Reihe nach Beifahrer -> 360-Kamera -> aus */
        static int combo_was;
        void *ctl; unsigned short btn = 0;
        int combo = autodrive_game_buttons(&ctl, &btn) >= 0 && (btn & 0x0100u) != 0u && (btn & 0x0200u) == 0u;
        if (combo && !combo_was) {
            if (!drone_on) {
                if (view >= 1 && view <= 3) { chase_on = !chase_on; put(chase_on ? "CHASE-CAM on\r\n" : "CHASE-CAM off\r\n"); flush(); } /* in der Verfolger-Ansicht: Effekt an/aus */
                else pad_arm = GetTickCount64();
            } else if (rear_on) { drone_stop(); chase_on = 1; put("CHASE-CAM on\r\n"); flush(); }
            else if (passenger_on && !drv_on) req_h = 1;
            else if (passenger_on) req_x = 1;
            else req_m = 1;
            put("PAD-LB view="); put_number(view); put("\r\n"); flush();
        }
        combo_was = combo;
        static int yk, l3_was;
        int l3 = autodrive_game_buttons(&ctl, &btn) >= 0 && (btn & 0x0040u) != 0u; /* L3 (linker Stick druecken) */
        int yd = (focused && game_input_key_down(0x59)) || l3;                      /* Y bzw. L3: Blick wieder nach vorn */
        if (yd && !yk && !(l3 && l3_was) && drone_on && passenger_on && !cinema_on) {
            drone_hx = car_hx; drone_hz = car_hz; drone_pitch = 0.0f; pend_yaw = pend_pitch = 0.0f;
            look_yaw_rate = look_pitch_rate = 0.0f; mouse_yaw_rate = mouse_pitch_rate = 0.0f;
        }
        yk = yd; l3_was = l3;
    }
    if (drone_on && camera != last_seen_camera) {
        last_seen_camera = camera;
        put("DRONE-CAMERA-SEEN view="); put_number(view); put(" cam="); put_hex((uint64_t)(uintptr_t)camera & 0xffffffffu, 8);
        put(" t="); put_number((unsigned)GetTickCount64()); put("\r\n"); flush();
    }
    {
        static int pk;
        int pd = focused && game_input_key_down(0x50);
        if (pd && !pk && drone_on) {
            walk_mode = !walk_mode;
            if (walk_mode) {
                ground_ref = drone_anchor[1] - 1.15;
                trail_n = 0; trail_head = 0;
                eye_height = 1.6f;
            }
            put(walk_mode ? "DRONE-MODE walk\r\n" : "DRONE-MODE drone\r\n"); flush();
        }
        pk = pd;
        static int mk, ck2;
        int md = (focused && game_input_key_down(0x4D)) || req_m; /* M = Beifahrer-Modus */
        req_m = 0;
        if (md && !mk && drone_on) {
            passenger_on = !passenger_on;
            seat_ok = 0; cinema_on = 0;
            if (rear_on) { rear_on = 0; zoom_target = 1.0f; }
            drv_on = 0;
            if (passenger_on) {
                if (!walk_mode) { walk_mode = 1; ground_ref = drone_anchor[1] - 1.15; trail_n = 0; trail_head = 0; eye_height = 1.6f; }
                if (gfh_fresh && cs_seen) {
                    /* Karosserie-Achse steht manchmal verkehrt herum: mit der Spiel-Kamera abgleichen */
                    float dd = car_hx * gfh_x + car_hz * gfh_z;
                    if (dd < -0.2f) { cs_sign = -cs_sign; car_hx = -car_hx; car_hz = -car_hz; }
                    put("PASSENGER-ENABLE dot*100="); put_signed((int)(dd * 100.0f)); put(" sign="); put_signed((int)cs_sign); put(" cs_ok="); put_number((unsigned)cs_ok); put("\r\n"); flush();
                }
                pch_x = car_hx; pch_z = car_hz;
                drone_hx = car_hx; drone_hz = car_hz; drone_pitch = 0.0f; pend_yaw = pend_pitch = 0.0f;
                focus_on = 0; follow_on = 0;
                seat_mode(0);
            } else seat_mode(-1);
            put(passenger_on ? "DRONE-PASSENGER on\r\n" : "DRONE-PASSENGER off\r\n"); flush();
        }
        mk = md;
        static int xk;
        int xd = (focused && game_input_key_down(0x58)) || req_x;
        req_x = 0; /* X = 360-Kamera hinten oben am Auto (immer 0,5x) */
        if (xd && !xk && drone_on) {
            if (!rear_on) {
                if (!passenger_on) {
                    passenger_on = 1; seat_ok = 0; cinema_on = 0;
                    if (!walk_mode) { walk_mode = 1; ground_ref = drone_anchor[1] - 1.15; trail_n = 0; trail_head = 0; }
                    if (gfh_fresh && cs_seen) {
                        float dd = car_hx * gfh_x + car_hz * gfh_z;
                        if (dd < -0.2f) { cs_sign = -cs_sign; car_hx = -car_hx; car_hz = -car_hz; }
                    }
                    pch_x = car_hx; pch_z = car_hz;
                    drone_hx = car_hx; drone_hz = car_hz; drone_pitch = 0.0f; pend_yaw = pend_pitch = 0.0f;
                    focus_on = 0; follow_on = 0;
                } else drv_on = 0;
                cinema_on = 0; rear_on = 1; seat_mode(2); zoom_target = 0.5f;
            } else {
                rear_on = 0; passenger_on = 0; seat_ok = 0; seat_mode(-1); zoom_target = 1.0f;
            }
            put(rear_on ? "DRONE-REAR360 on\r\n" : "DRONE-REAR360 off\r\n"); flush();
        }
        xk = xd;
        static int hk;
        int hd = (focused && game_input_key_down(0x48)) || req_h; /* H = Fahrer-Sitz (wie Beifahrer, aber auf dem Fahrerplatz) */
        req_h = 0;
        if (hd && !hk && drone_on && passenger_on && !rear_on) {
            if (!drv_on) { drv_on = 1; cinema_on = 0; seat_mode(1); }
            else { drv_on = 0; seat_mode(0); }
            put(drv_on ? "DRONE-DRIVERSEAT on\r\n" : "DRONE-DRIVERSEAT off\r\n"); flush();
        }
        hk = hd;
        int cd2 = focused && game_input_key_down(0x43); /* C = Seite wechseln */
        if (cd2 && !ck2 && drone_on && passenger_on && !rear_on && !drv_on) { passenger_side = -passenger_side; seat_r = -seat_r; put("DRONE-PASSENGER side\r\n"); flush(); }
        ck2 = cd2;
        static int tk;
        int td = focused && game_input_key_down(0x55); /* U = Zittern an/aus */
        if (td && !tk && drone_on) {
            shake_on = !shake_on;
            put(shake_on ? "DRONE-SHAKE on\r\n" : "DRONE-SHAKE off\r\n"); flush();
        }
        tk = td;
        static int vk;
        int vd = focused && game_input_key_down(0x56); /* V = dem Auto hinterherlaufen */
        if (vd && !vk && drone_on && walk_mode) { follow_on = !follow_on; put(follow_on ? "DRONE-FOLLOW on\r\n" : "DRONE-FOLLOW off\r\n"); flush(); }
        vk = vd;
        static int lk;
        int ld = focused && game_input_key_down(0x4C); /* L = Cinema-Modus (nur Beifahrer) */
        if (ld && !lk && drone_on && passenger_on && !rear_on) { cinema_on = !cinema_on; cin_dur = 0.0f; cin_seed = qpc_lo * 2654435761u; cin_n = 0; cin_shot = -1; cin_prev = -1; cin_yv = cin_pv = 0.0f; put(cinema_on ? "DRONE-CINEMA on\r\n" : "DRONE-CINEMA off\r\n"); flush(); }
        lk = ld;
        static int gk;
        int gd = focused && game_input_key_down(0x47); /* G = Auto im Blick halten */
        if (gd && !gk && drone_on) { focus_on = !focus_on; put(focus_on ? "DRONE-FOCUS-CAR on\r\n" : "DRONE-FOCUS-CAR off\r\n"); flush(); }
        gk = gd;
        static int kk, f7k, f8k;
        int kd = focused && game_input_key_down(0x4B); /* K = verbesserte Ego-Kamera an/aus */
        static int jk;
        int jd = focused && game_input_key_down(0x4A); /* J = verbesserte Verfolger-Kamera an/aus */
        if (jd && !jk) { chase_on = !chase_on; put(chase_on ? "CHASE-CAM on\r\n" : "CHASE-CAM off\r\n"); flush(); }
        jk = jd;
        if (kd && !kk) { ego_on = !ego_on; put(ego_on ? "EGO-CAM on\r\n" : "EGO-CAM off\r\n"); flush(); }
        kk = kd;
        int f7 = focused && game_input_key_down(0x76), f8 = focused && game_input_key_down(0x77); /* F7 weniger, F8 mehr */
        if (f7 && !f7k && ego_level_pct > 10) ego_level_pct -= 10;
        if (f8 && !f8k && ego_level_pct < 200) ego_level_pct += 10;
        if ((f7 && !f7k) || (f8 && !f8k)) { put("EGO-CAM level "); put_number((unsigned)ego_level_pct); put("\r\n"); flush(); }
        f7k = f7; f8k = f8;
        static int f5k, f6k;
        int f5 = focused && game_input_key_down(0x74), f6 = focused && game_input_key_down(0x75); /* F5 weniger, F6 mehr Wippen */
        if (f5 && !f5k && drone_on && walk_level_pct > 10) walk_level_pct -= 10;
        if (f6 && !f6k && drone_on && walk_level_pct < 200) walk_level_pct += 10;
        if ((f5 && !f5k) || (f6 && !f6k)) { put("DRONE-WALKLEVEL "); put_number((unsigned)walk_level_pct); put("\r\n"); flush(); }
        f5k = f5; f6k = f6;
    }
    {
        static void *last_obs; static int obs_logs;
        if (after_update && camera != last_obs && obs_logs < 60) {
            last_obs = camera; ++obs_logs;
            void *vt = 0; memory_read(camera, &vt, sizeof(vt));
            put("CAM-OBSERVED view="); put_number(view); put(" vt=0x"); put_hex((uint64_t)((uintptr_t)vt - (uintptr_t)image), 8);
            put(" t="); put_number((unsigned)GetTickCount64()); put("\r\n"); flush();
        }
    }
    drone_input_enabled = focused;
    if (drone_on && view < 7) {
        static unsigned long long perf_sum, perf_max; static unsigned perf_n; static float perf_dt;
        unsigned long long t0 = perf_now_us();
        drone_frame(camera, dt, view);
        unsigned long long used_us = perf_now_us() - t0;
        perf_sum += used_us; if (used_us > perf_max) perf_max = used_us; perf_dt += dt;
        if (++perf_n >= 600) {
            put("PERF view="); put_number(view);
            put(" fps="); put_number(perf_dt > 0.0f ? (unsigned)(600.0f / perf_dt) : 0u);
            put(" mod_avg_us="); put_number((unsigned)(perf_sum / 600ull));
            put(" mod_max_us="); put_number((unsigned)perf_max);
            put("\r\n"); flush();
            perf_sum = 0; perf_max = 0; perf_n = 0; perf_dt = 0.0f;
        }
    }
}

int drone_probe_owns_camera(void *camera) {
    return drone_on && camera == drone_camera_ptr && have_written;
}

int drone_probe_car_flag(int which) {
    if (which == 2) return drone_on; /* Auto in Ego-Ansicht komplett zeichnen */
    return 0;
}

int drone_probe_is_on(void) { return drone_on; }
float drone_probe_zoom(void) { return zoom_current; }
void drone_probe_apply_roll(void *out) {
    if (!drone_on || !out || (cur_roll < 0.0002f && cur_roll > -0.0002f)) return;
    float rows[3][4];
    for (int i = 0; i < 3; ++i) {
        if (!memory_read((unsigned char *)out + i * 16, rows[i], 16)) return;
        for (int k = 0; k < 4; ++k) if (!float_is_finite(rows[i][k])) return;
    }
    float d02 = rows[0][0] * rows[2][0] + rows[0][1] * rows[2][1] + rows[0][2] * rows[2][2];
    float d12 = rows[1][0] * rows[2][0] + rows[1][1] * rows[2][1] + rows[1][2] * rows[2][2];
    float l2 = rows[2][0] * rows[2][0] + rows[2][1] * rows[2][1] + rows[2][2] * rows[2][2];
    int ok = d02 < 0.05f && d02 > -0.05f && d12 < 0.05f && d12 > -0.05f && l2 > 0.9f && l2 < 1.1f;
    if (!ok) return;
    float c = my_cos(cur_roll), s = my_sin(cur_roll);
    float n0[4], n1[4];
    for (int k = 0; k < 4; ++k) {
        n0[k] = c * rows[0][k] + s * rows[1][k];
        n1[k] = -s * rows[0][k] + c * rows[1][k];
    }
    if (memory_is_writable(out, 32)) {
        memory_copy((unsigned char *)out, n0, 16);
        memory_copy((unsigned char *)out + 16, n1, 16);
    }
}

/* ---- Verbesserte Ego-Kamera: Bewegung nur im Bild (View-Matrix), die Spiel-Kamera selbst bleibt unveraendert ---- */
static float eg_sp_prev, eg_pdx, eg_pdz = 1.0f, eg_a, eg_yr, eg_lat, eg_spd, eg_vacc, eg_pvy, eg_hp, eg_hv, eg_bp, eg_bv, eg_vp[3], eg_t, eg_dirx = 1.0f, eg_dirz;
static int eg_have;
static float eg_sp_, eg_sy_, eg_sr_, eg_sx_, eg_sz_, eg_sh_;
static float eg_sv_[6];
/* kritisch gedaempfte Feder: weicher Einstieg (Beschleunigung steigt langsam an), kein harter Start */
static float ego_crit(float cur, float *vel, float tgt, float d, float w) {
    float dd = clampf(d, 0.0f, 0.05f);
    *vel += ((tgt - cur) * w * w - 2.0f * w * (*vel)) * dd;
    return cur + *vel * dd;
}
static float eg_pvx, eg_pvz, eg_hold, eg_lost;
static unsigned long long eg_last_upd;

/* Bewegung wird pro gezeichnetem Bild (echte Zeit) berechnet, nicht pro Spiel-Update: sonst ruckelt es, wenn Update- und Bildrate nicht zusammenpassen */
static void ego_step(float d) {
    if (drone_on || !(ego_view_chase ? chase_on : ego_on)) return;
    float lvl = (float)ego_level_pct * 0.01f * (ego_view_chase ? 0.8f : 1.0f);
    if (GetTickCount64() - eg_last_upd > 120ull) {
        /* Spiel pausiert (keine Kamera-Updates mehr): Effekt weich ausblenden, nichts wackelt weiter */
        float kf = clampf(d * 6.0f, 0.0f, 1.0f);
        eg_a = eg_yr = eg_lat = eg_vacc = 0.0f;
        eg_spd -= eg_spd * kf;
        eg_have = 0;
    }
    eg_t += d;
    /* Kopf: bei Gas nach hinten, beim Bremsen nach vorn (Feder-Daempfer) */
    float tgt = clampf(eg_a * 0.0035f, -0.05f, 0.05f) * lvl;
    eg_hv += ((tgt - eg_hp) * 14.0f - eg_hv * 5.5f) * d;
    eg_hp = clampf(eg_hp + eg_hv * d, -0.25f, 0.25f);
    /* Bodenwellen */
    float btgt = clampf(-eg_vacc * 0.0003f, -0.015f, 0.015f) * lvl;
    eg_bv += ((btgt - eg_bp) * 60.0f - eg_bv * 9.0f) * d;
    eg_bp = clampf(eg_bp + eg_bv * d, -0.02f, 0.02f);
    /* Vibrieren bei Tempo (ab ~110 km/h) */
    float vf = clampf((eg_spd - 30.0f) / 60.0f, 0.0f, 1.0f);
    float amp = (vf * vf * (0.6f + 0.8f * vnoise(5, eg_t * 1.3f)) + 0.05f * vf) * 0.4f * lvl;
    eg_vp[0] = wrap_pi(eg_vp[0] + d * 2.0f * PI_F * 13.0f);
    eg_vp[1] = wrap_pi(eg_vp[1] + d * 2.0f * PI_F * 19.0f);
    eg_vp[2] = wrap_pi(eg_vp[2] + d * 2.0f * PI_F * 27.0f);
    /* Grundbewegung weich (Tiefpass + Geschwindigkeitsbegrenzung), nur das feine Vibrieren darf schnell sein */
    float pb = eg_hp + eg_bp * 0.5f;
    float yb = clampf(eg_yr * 0.05f, -0.05f, 0.05f) * lvl - eg_lat * 0.0006f * lvl;
    float rb = -eg_lat * 0.0030f * lvl;
    float back = -eg_hp * 0.60f, side = -eg_lat * 0.006f * lvl;
    float dxb = eg_dirx * back + eg_dirz * side, dzb = eg_dirz * back - eg_dirx * side;
    eg_sp_ = ego_crit(eg_sp_, &eg_sv_[0], pb, d, 5.0f);
    eg_sy_ = ego_crit(eg_sy_, &eg_sv_[1], yb, d, 4.0f);
    eg_sr_ = ego_crit(eg_sr_, &eg_sv_[2], rb, d, 5.0f);
    eg_sx_ = ego_crit(eg_sx_, &eg_sv_[3], dxb, d, 5.0f);
    eg_sz_ = ego_crit(eg_sz_, &eg_sv_[4], dzb, d, 5.0f);
    eg_sh_ = ego_crit(eg_sh_, &eg_sv_[5], eg_bp, d, 6.0f);
    ego_pitch = eg_sp_ + amp * (0.0040f * my_sin(eg_vp[0]) + 0.0025f * my_sin(eg_vp[1] + 1.0f));
    ego_yaw = eg_sy_ + amp * 0.0030f * my_sin(eg_vp[2] + 2.0f);
    ego_roll = eg_sr_ + amp * (0.0060f * my_sin(eg_vp[1] + 0.5f) + 0.0035f * my_sin(eg_vp[2]));
    ego_dx = eg_sx_; ego_dz = eg_sz_;
    ego_dy = eg_sh_ + amp * 0.0022f * my_sin(eg_vp[2] + 0.7f);
}

void drone_probe_ego_view(int chase) { ego_view_chase = chase; }

void drone_probe_ego_update(void *camera, float dt) {
    if (drone_on || !(ego_view_chase ? chase_on : ego_on)) { ego_pitch = ego_yaw = ego_roll = ego_dx = ego_dy = ego_dz = 0.0f; eg_have = 0; return; }
    float fx, fz, vel[4];
    vel[0] = vel[1] = vel[2] = vel[3] = 0.0f;
    int ok = drift_camera_car_state(camera, &fx, &fz, vel);
    if (!(dt > 0.0005f && dt < 0.1f)) ok = 0;
    if (ok) eg_lost = 0.0f;
    else { eg_lost += (dt > 0.0f && dt < 0.5f) ? dt : 0.0f; eg_hold += (dt > 0.0f && dt < 0.5f) ? dt : 0.0f; if (eg_lost > 0.6f) eg_have = 0; } /* kurze Aussetzer der Auto-Daten werden ueberbrueckt */
    float lvl = (float)ego_level_pct * 0.01f;
    eg_last_upd = GetTickCount64();
    float vx = ok ? vel[0] : 0.0f, vz = ok ? vel[2] : 0.0f;
    float sp = my_sqrt(vx * vx + vz * vz);
    if (ok) {
        if (eg_have) {
            /* Das Auto meldet seine Geschwindigkeit nur schrittweise: Ableitung nur bei neuen Daten, ueber die ganze vergangene Zeit */
            if (vel[0] == eg_pvx && vel[1] == eg_pvy && vel[2] == eg_pvz) {
                eg_hold += dt;
            } else {
                float dtt = eg_hold + dt;
                eg_hold = 0.0f;
                if (dtt > 0.0005f && dtt < 0.12f) {
                    float a = clampf((sp - eg_sp_prev) / dtt, -20.0f, 20.0f);
                    if (dtt < 0.004f) a = eg_a; /* zu kurzer Abstand: Messwert unzuverlaessig */
                    eg_a += (a - eg_a) * clampf(dtt * 2.5f, 0.0f, 1.0f);
                    if (sp > 4.0f) {
                        float dx = vx / sp, dz = vz / sp;
                        float yr = -(eg_pdx * dz - eg_pdz * dx) / dtt; /* >0 = Rechtskurve */
                        eg_yr += (clampf(yr, -3.0f, 3.0f) - eg_yr) * clampf(dtt * 3.0f, 0.0f, 1.0f);
                        eg_pdx = dx; eg_pdz = dz; eg_dirx = dx; eg_dirz = dz;
                    } else eg_yr -= eg_yr * clampf(dtt * 4.0f, 0.0f, 1.0f);
                    float la = clampf(eg_yr * sp, -16.0f, 16.0f);
                    eg_lat += (la - eg_lat) * clampf(dtt * 3.0f, 0.0f, 1.0f);
                    float va = clampf((vel[1] - eg_pvy) / dtt, -40.0f, 40.0f);
                    eg_vacc += (va - eg_vacc) * clampf(dtt * 6.0f, 0.0f, 1.0f);
                }
                eg_sp_prev = sp; eg_pvx = vel[0]; eg_pvy = vel[1]; eg_pvz = vel[2];
            }
            eg_spd += (sp - eg_spd) * clampf(dt * 4.0f, 0.0f, 1.0f);
        } else {
            eg_a = eg_yr = eg_lat = eg_vacc = 0.0f; eg_spd = sp;
            if (sp > 4.0f) { eg_pdx = vx / sp; eg_pdz = vz / sp; eg_dirx = eg_pdx; eg_dirz = eg_pdz; }
        }
        if (!eg_have) { eg_sp_prev = sp; eg_pvx = vel[0]; eg_pvy = vel[1]; eg_pvz = vel[2]; eg_hold = 0.0f; }
        eg_have = 1;
    } else {
        float k = clampf(dt > 0.0f ? dt * (eg_lost > 0.6f ? 3.0f : 0.4f) : 0.01f, 0.0f, 1.0f);
        eg_a -= eg_a * k; eg_yr -= eg_yr * k; eg_lat -= eg_lat * k; eg_vacc -= eg_vacc * k; eg_spd -= eg_spd * k;
    }
    (void)lvl;
}

int drone_probe_ego_active(void) { return !drone_on && ego_on; }

void drone_probe_ego_apply(void *camera, void *out) {
    if (drone_on || !out) return;
    if (!(ego_view_chase ? chase_on : ego_on)) return;
    {
        /* echte Zeit seit dem letzten Schritt */
        typedef BOOL (WINAPI *FnQ)(LARGE_INTEGER *);
        static FnQ qpc, qpf;
        static LARGE_INTEGER qf, qlast;
        static int qinit;
        LARGE_INTEGER qn;
        qn.QuadPart = 0;
        if (!qinit) {
            qinit = 1;
            HMODULE kk = LoadLibraryExW(L"kernel32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (kk) { qpc = (FnQ)(uintptr_t)GetProcAddress(kk, "QueryPerformanceCounter"); qpf = (FnQ)(uintptr_t)GetProcAddress(kk, "QueryPerformanceFrequency"); }
            if (qpf) qpf(&qf);
        }
        if (qpc && qf.QuadPart) {
            qpc(&qn);
            if (qlast.QuadPart == 0) qlast = qn;
            else {
                double w = (double)(qn.QuadPart - qlast.QuadPart) / (double)qf.QuadPart;
                if (w >= 0.0015) { ego_step(w > 0.05 ? 0.05f : (float)w); qlast = qn; }
            }
        }
    }
    float rows[3][4];
    for (int i = 0; i < 3; ++i) {
        if (!memory_read((unsigned char *)out + i * 16, rows[i], 16)) return;
        for (int k = 0; k < 4; ++k) if (!float_is_finite(rows[i][k])) return;
    }
    float d02 = rows[0][0] * rows[2][0] + rows[0][1] * rows[2][1] + rows[0][2] * rows[2][2];
    float d12 = rows[1][0] * rows[2][0] + rows[1][1] * rows[2][1] + rows[1][2] * rows[2][2];
    float l2 = rows[2][0] * rows[2][0] + rows[2][1] * rows[2][1] + rows[2][2] * rows[2][2];
    static float sf_k = 1.0f, sr_k = 1.0f, su_k = 1.0f;
    static int axes_known;
    int ortho_ok = d02 < 0.05f && d02 > -0.05f && d12 < 0.05f && d12 > -0.05f && l2 > 0.9f && l2 < 1.1f;
    if (!ortho_ok) {
        /* Format ist nach dem ersten Erfolg bekannt: kleine Abweichungen duerfen den Effekt nicht kurz ausschalten (das waere ein Sprung) */
        if (!axes_known || l2 < 0.5f || l2 > 1.5f) return;
    }
    int sgn_ok = ortho_ok && eg_spd > 8.0f && (eg_dirx != 0.0f || eg_dirz != 0.0f);
    if (!axes_known && !sgn_ok) return;
    if (sgn_ok) {
        /* Referenz: Fahrtrichtung des Autos (stabil), nicht das Kamerafeld */
        float dl = my_sqrt(eg_dirx * eg_dirx + eg_dirz * eg_dirz);
        float gx = eg_dirx / (dl > 0.0001f ? dl : 1.0f), gz = eg_dirz / (dl > 0.0001f ? dl : 1.0f);
        float dfw = rows[2][0] * gx + rows[2][2] * gz;
        float drt = rows[0][0] * gz - rows[0][2] * gx;
        if ((dfw > 0.6f || dfw < -0.6f) && (drt > 0.6f || drt < -0.6f) && (rows[1][1] > 0.5f || rows[1][1] < -0.5f)) {
            float nsf = dfw < 0.0f ? -1.0f : 1.0f, nsr = drt < 0.0f ? -1.0f : 1.0f, nsu = rows[1][1] < 0.0f ? -1.0f : 1.0f;
            static unsigned agree_n; static float cf, cr, cu;
            if (nsf == cf && nsr == cr && nsu == cu) ++agree_n; else { cf = nsf; cr = nsr; cu = nsu; agree_n = 1; }
            if (!axes_known) { if (agree_n >= 15) { sf_k = cf; sr_k = cr; su_k = cu; axes_known = 1; } }
            else if (agree_n >= 300) { sf_k = cf; sr_k = cr; su_k = cu; }
        }
    }
    int axes_ok = axes_known; /* einmal erkannt, bleiben die Vorzeichen gueltig (kein Ein/Aus-Springen) */
    float sf = sf_k, sr = sr_k, su = su_k;
    if (axes_ok) {
        float c, sn, n2[4], n0[4], n1[4];
        /* Gieren (nach rechts +) */
        c = my_cos(ego_yaw); sn = my_sin(ego_yaw) * sf * sr;
        for (int k = 0; k < 4; ++k) { n2[k] = c * rows[2][k] + sn * rows[0][k]; n0[k] = c * rows[0][k] - sn * rows[2][k]; rows[2][k] = n2[k]; rows[0][k] = n0[k]; }
        /* Nicken (nach oben +) */
        c = my_cos(ego_pitch); sn = my_sin(ego_pitch) * sf * su;
        for (int k = 0; k < 4; ++k) { n2[k] = c * rows[2][k] + sn * rows[1][k]; n1[k] = c * rows[1][k] - sn * rows[2][k]; rows[2][k] = n2[k]; rows[1][k] = n1[k]; }
    }
    {
        /* Kippen (gleiche Richtung wie bei der Drohne) */
        float c = my_cos(ego_roll), sn = my_sin(ego_roll), n0[4], n1[4];
        for (int k = 0; k < 4; ++k) { n0[k] = c * rows[0][k] + sn * rows[1][k]; n1[k] = -sn * rows[0][k] + c * rows[1][k]; rows[0][k] = n0[k]; rows[1][k] = n1[k]; }
    }
    /* Kopfposition leicht verschieben */
    for (int i = 0; i < 3; ++i) rows[i][3] -= rows[i][0] * ego_dx + rows[i][1] * ego_dy + rows[i][2] * ego_dz;
    if (memory_is_writable(out, 48)) memory_copy((unsigned char *)out, rows, 48);
}

float drone_probe_fov_divisor(void) { return (drone_on && zoom_current > 0.01f) ? zoom_current : 1.0f; }
unsigned drone_probe_seconds(void) { return drone_on ? (unsigned)((GetTickCount64() - drone_start_tick) / 1000ull) : 0u; }

int drone_probe_walk_mode(void) { return walk_mode; }
int drone_probe_walk_level(void) { return walk_level_pct; }
int drone_probe_focus(void) { return focus_on; }
int drone_probe_passenger(void) { return passenger_on; }
int drone_probe_cinema(void) { return cinema_on && passenger_on; }
int drone_probe_follow(void) { return follow_on; }

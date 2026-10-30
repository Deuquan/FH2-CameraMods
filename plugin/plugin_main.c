// Camera Mods as a native Extra Options plugin (FH2 plugin API v1).
//
// Instead of patching Extra Options, this DLL installs its own camera hooks into
// the game's vtables when the plugin loader initialises it. Whatever pointer is
// already in a slot (the game's own function, or a hook of Extra Options /
// another mod) is saved and called first, so this plugin chains with others.
//
// Camera logic itself lives in features/drone_probe.c and features/overlay_window.c.
#include "extra_options.h"
#include "support.h"
#include "logging.h"
#include "features.h"
#include "fh2_plugin.h"
#include "features/camera_views.h"
#include "features/autodrive_game.h"
#include "features/game_input.h"
#include <xmmintrin.h>

#define PLUGIN_NAME "CameraMods"
#define PLUGIN_VERSION "1.2.0"
#define EXPECTED_IMAGE_SIZE 0x3a18000u

static unsigned char *image;
static const FH2PluginContext *host;

// ---------------------------------------------------------------------------
// Small runtime: logging and safe memory access (no C library available).
// ---------------------------------------------------------------------------
void log_append(const char *message) {
    if (host && host->log_utf8 && message) {
        host->log_utf8(message);
    }
}

void log_append_value(const char *label, int value) {
    char text[96];
    size_t length = 0;
    while (label && *label && length < 70) {
        text[length++] = *label++;
    }
    char digits[12];
    unsigned number = value < 0 ? 0u - (unsigned)value : (unsigned)value;
    int count = 0;
    do {
        digits[count++] = (char)('0' + number % 10);
        number /= 10;
    } while (number && count < 11);
    if (value < 0) {
        text[length++] = '-';
    }
    while (count) {
        text[length++] = digits[--count];
    }
    text[length++] = '\r';
    text[length++] = '\n';
    text[length] = 0;
    log_append(text);
}

void memory_copy(void *destination, const void *source, size_t size) {
    volatile unsigned char *destination_bytes = (volatile unsigned char *)destination;
    const unsigned char *source_bytes = (const unsigned char *)source;
    while (size--) {
        *destination_bytes++ = *source_bytes++;
    }
}

static int memory_read_fault(EXCEPTION_POINTERS *exception) {
    DWORD code = exception->ExceptionRecord->ExceptionCode;
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
                   code == EXCEPTION_GUARD_PAGE
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

int memory_read(const void *address, void *out, size_t size) {
    if (!address || !out || size > UINTPTR_MAX - (uintptr_t)address) {
        return 0;
    }
    __try {
        memory_copy(out, address, size);
        return 1;
    } __except (memory_read_fault(GetExceptionInformation())) {
        return 0;
    }
}

int memory_read_pointer(const void *address, void **value) {
    return memory_read(address, value, sizeof(*value)) && *value;
}

int float_is_finite(float value) {
    uint32_t bits;
    memory_copy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000) != 0x7f800000;
}

int memory_is_writable(const void *address, size_t size) {
    uintptr_t start = (uintptr_t)address, end = start + size;
    if (!start || end < start) {
        return 0;
    }
    while (start < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)start, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            return 0;
        }
        DWORD protection = info.Protect & 0xff;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) {
            return 0;
        }
        uintptr_t next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (next <= start) {
            return 0;
        }
        start = next;
    }
    return 1;
}

void patch_engine_log_slots(void) {}

// ---------------------------------------------------------------------------
// Input: keyboard state and the native controller (replaces Extra Options' game_input).
// ---------------------------------------------------------------------------
typedef SHORT(WINAPI *KeyStateFn)(int);
typedef HWND(WINAPI *ForegroundWindowFn)(void);
typedef DWORD(WINAPI *WindowProcessFn)(HWND, LPDWORD);
static KeyStateFn key_state_fn;
static ForegroundWindowFn foreground_window_fn;
static WindowProcessFn window_process_fn;
static int input_ready;

static int input_init(void) {
    if (!input_ready) {
        input_ready = 1;
        HMODULE user32 = LoadLibraryExW(L"user32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (user32) {
            key_state_fn = (KeyStateFn)(uintptr_t)GetProcAddress(user32, "GetAsyncKeyState");
            foreground_window_fn =
                (ForegroundWindowFn)(uintptr_t)GetProcAddress(user32, "GetForegroundWindow");
            window_process_fn =
                (WindowProcessFn)(uintptr_t)GetProcAddress(user32, "GetWindowThreadProcessId");
        }
        if (!key_state_fn || !foreground_window_fn || !window_process_fn) {
            log_append(PLUGIN_NAME ": keyboard API unavailable.\r\n");
        }
    }
    return key_state_fn && foreground_window_fn && window_process_fn;
}

int autodrive_game_window_focused(void) {
    if (!input_init()) {
        return 0;
    }
    HWND window = foreground_window_fn();
    DWORD process_id = 0;
    if (window) {
        window_process_fn(window, &process_id);
    }
    return process_id == GetCurrentProcessId();
}

int game_input_key_down(int virtual_key) {
    if (!input_init()) {
        return 0;
    }
    return (key_state_fn(virtual_key) & 0x8000) != 0;
}

static int has_vtable(void *object, unsigned rva) {
    void *vtable;
    return memory_read_pointer(object, &vtable) && vtable == image + rva;
}

int autodrive_game_buttons(void **controller, unsigned short *buttons) {
    void *manager;
    unsigned char connected;
    *buttons = 0;
    *controller = 0;
    if (!memory_read_pointer(image + 0x365d7c8, &manager) || !has_vtable(manager, 0x2e7a2e8) ||
        !memory_read_pointer((unsigned char *)manager + 0xab28, controller)) {
        return -1;
    }
    uintptr_t first = (uintptr_t)manager + 0x10;
    uintptr_t selected = (uintptr_t)*controller;
    if (selected < first || selected >= first + 8 * 0x1300 || (selected - first) % 0x1300 ||
        !memory_read((unsigned char *)*controller + 4, &connected, 1) || connected != 1 ||
        !memory_read((unsigned char *)*controller + 0xc0, buttons, sizeof(*buttons))) {
        *controller = 0;
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Game knowledge: camera classes and the player car (Forza Horizon 2 PC build).
// ---------------------------------------------------------------------------
enum {
    FOLLOW = 0x2e783a0,
    FOLLOW_NEAR = 0x2e78610,
    FOLLOW_FAR = 0x2e78880,
    DRIVER = 0x2e78120,
    HOOD = 0x2e77ea8,
    BUMPER = 0x2e703b0,
    EXTRA1 = 0x2e70628,
    EXTRA2 = 0x2e708a0,
    EXTRA3 = 0x2e70b18,
    EXTRA4 = 0x2e77c30,
    DIRECTOR_A = 0x2e70080,
    DIRECTOR_B = 0x2c6c840,
    FREE_CAMERA = 0x2e78af0,
    FREE_TC_CAMERA = 0x2e78d98,
    FREE_TRACK_CAMERA = 0x2e79040,
    ANIM_CAMERA = 0x2e779d8,
    ORTHO_CAMERA = 0x2e792e8,

    TARGET_PREDICTED_VTABLE = 0x2e79540,
    TARGET_CAR_VTABLE = 0x2c5ce28,
    TARGET_PLAYER_CAR_VTABLE = 0x2e6b348,
    CAR_BODY_OFFSET = 0x4be8,
    NATIVE_CAR_VELOCITY = 0xf646b0,
    NATIVE_CAR_AXIS = 0xf62220,

    NATIVE_ATTACHED_UPDATE = 0x103c590,
    NATIVE_FOLLOW_UPDATE = 0x103c820,
    NATIVE_POS_F = 0x1034a40,
    NATIVE_POS_D = 0x1034ac0,
    NATIVE_FOV_CHASE = 0x1034120,

    SLOT_UPDATE = 0x20,
    SLOT_VIEW_MATRIX = 0x50,
    SLOT_FOV = 0x70,
    SLOT_POS_F = 0x150,
    SLOT_POS_D = 0x158,
};

static const unsigned camera_vtables[6] = {FOLLOW, FOLLOW_NEAR, FOLLOW_FAR, DRIVER, HOOD, BUMPER};

typedef void *(*VectorGetter)(void *, float *);
typedef void *(*AxisGetter)(void *, float *, int);
typedef void (*CameraUpdate)(void *, float, unsigned char);
typedef void *(*ViewMatrixFn)(void *, void *);
typedef void *(*PosGetterF)(void *, float *);
typedef void *(*PosGetterD)(void *, double *);
typedef float (*GetFov)(void *);
typedef void (*DirectorUpdate)(void *, float, unsigned char, unsigned char);

static CameraView camera_type(void *camera) {
    void *vtable;
    if (!memory_read_pointer(camera, &vtable)) {
        return CAMERA_UNKNOWN;
    }
    for (unsigned index = 0; index < ARRAY_COUNT(camera_vtables); ++index) {
        if (vtable == image + camera_vtables[index]) {
            return (CameraView)(index + 1);
        }
    }
    return CAMERA_UNKNOWN;
}

static void *car_body(void *camera, float velocity[4], float axis[4]) {
    void *target, *vtable, *body, *getter;
    if (!memory_read_pointer((unsigned char *)camera + 8, &target)) {
        return 0;
    }
    // Predicted targets wrap the same car; use its actual world velocity.
    for (unsigned depth = 0; depth < 3; ++depth) {
        if (!memory_read_pointer(target, &vtable)) {
            return 0;
        }
        if (vtable == image + TARGET_PREDICTED_VTABLE) {
            if (!memory_read_pointer((unsigned char *)target + 8, &target)) {
                return 0;
            }
            continue;
        }
        if ((vtable != image + TARGET_CAR_VTABLE && vtable != image + TARGET_PLAYER_CAR_VTABLE) ||
            !memory_read_pointer((unsigned char *)target + CAR_BODY_OFFSET, &body) || !body ||
            !memory_read_pointer((unsigned char *)vtable + 0x10, &getter) ||
            getter != image + NATIVE_CAR_VELOCITY) {
            return 0;
        }
        ((VectorGetter)getter)(target, velocity);
        if (!float_is_finite(velocity[0]) || !float_is_finite(velocity[1]) ||
            !float_is_finite(velocity[2])) {
            return NULL;
        }
        if (axis) {
            if (!memory_read_pointer((unsigned char *)vtable + 0x20, &getter) ||
                getter != image + NATIVE_CAR_AXIS) {
                return NULL;
            }
            ((AxisGetter)getter)(target, axis, 2);
            if (!float_is_finite(axis[0]) || !float_is_finite(axis[1]) ||
                !float_is_finite(axis[2])) {
                return NULL;
            }
        }
        return body;
    }
    return 0;
}

// True heading and speed of the car body (used by passenger / driver seat mode).
int drift_camera_car_state(void *camera, float *hx, float *hz, float vel[3]) {
    _Alignas(16) float velocity[4], axis[4];
    if (!car_body(camera, velocity, axis)) return 0;
    float fx = -axis[0], fz = -axis[2];
    float len = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(fx * fx + fz * fz)));
    if (len < 0.1f) return 0;
    *hx = fx / len; *hz = fz / len;
    vel[0] = velocity[0]; vel[1] = velocity[1]; vel[2] = velocity[2];
    return 1;
}

int drift_camera_car_pitch(void *camera, float *fy, float *hlen) {
    _Alignas(16) float velocity[4], axis[4];
    if (!car_body(camera, velocity, axis)) return 0;
    *fy = axis[1];
    *hlen = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(axis[0] * axis[0] + axis[2] * axis[2])));
    return 1;
}

// ---------------------------------------------------------------------------
// Hook callbacks. Each one calls the previous function in its slot, then runs
// the camera logic after the game has computed its own camera for the frame.
// ---------------------------------------------------------------------------
static CameraUpdate orig_mounted[3];
static CameraUpdate orig_follow[3];
static ViewMatrixFn orig_vm[6];
static PosGetterF orig_pos_f[3];
static PosGetterD orig_pos_d[3];
static CameraUpdate orig_extra1, orig_extra2, orig_extra3, orig_extra4;
static CameraUpdate orig_free[5];
static DirectorUpdate orig_director[2];
static GetFov orig_fov[6];

static void h_mounted(void *camera, float dt, unsigned char reset) {
    CameraView view = camera_type(camera);
    int index = (view >= CAMERA_DRIVER && view <= CAMERA_BUMPER) ? (int)(view - CAMERA_DRIVER) : -1;
    CameraUpdate previous = (index >= 0 && orig_mounted[index]) ? orig_mounted[index]
                                                               : (CameraUpdate)(image + NATIVE_ATTACHED_UPDATE);
    previous(camera, dt, reset);
    drone_probe_frame(camera, (unsigned)view, image, 1, dt);
    if (index >= 0) {
        drone_probe_ego_view(0);
        drone_probe_ego_update(camera, dt);
    }
}

static void h_follow(void *camera, float dt, unsigned char reset) {
    CameraView view = camera_type(camera);
    int index = (view >= CAMERA_CHASE && view <= CAMERA_CHASE_FAR) ? (int)(view - CAMERA_CHASE) : -1;
    CameraUpdate previous = (index >= 0 && orig_follow[index]) ? orig_follow[index]
                                                              : (CameraUpdate)(image + NATIVE_FOLLOW_UPDATE);
    if (reset) {
        drone_probe_reset_seen((unsigned)view);
    }
    if (drone_probe_owns_camera(camera)) {
        reset = 0; // drone active: the game must not reset the camera
    }
    drone_probe_before_update(camera);
    previous(camera, dt, reset);
    drone_probe_frame(camera, (unsigned)view, image, 1, dt);
    if (index >= 0) {
        drone_probe_ego_view(1);
        drone_probe_ego_update(camera, dt);
    }
}

static void *h_view_matrix(void *camera, void *out) {
    CameraView view = camera_type(camera);
    int index = (view >= CAMERA_DRIVER && view <= CAMERA_BUMPER) ? (int)(view - CAMERA_DRIVER)
                : (view >= CAMERA_CHASE && view <= CAMERA_CHASE_FAR) ? 3 + (int)(view - CAMERA_CHASE) : -1;
    if (index < 0 || !orig_vm[index]) {
        return out;
    }
    void *result = orig_vm[index](camera, out);
    if (drone_probe_owns_camera(camera)) {
        drone_probe_apply_roll(out);
    } else {
        drone_probe_ego_view(index >= 3);
        drone_probe_ego_apply(camera, out);
    }
    return result;
}

static int follow_index(void *camera) {
    CameraView view = camera_type(camera);
    return (view >= CAMERA_CHASE && view <= CAMERA_CHASE_FAR) ? (int)(view - CAMERA_CHASE) : -1;
}

static void *h_pos_f(void *camera, float *out) {
    int index = follow_index(camera);
    PosGetterF previous = (index >= 0 && orig_pos_f[index]) ? orig_pos_f[index] : (PosGetterF)(image + NATIVE_POS_F);
    previous(camera, out);
    double position[3];
    if (drone_probe_override_position(camera, position)) {
        out[0] = (float)position[0];
        out[1] = (float)position[1];
        out[2] = (float)position[2];
    }
    return out;
}

static void *h_pos_d(void *camera, double *out) {
    int index = follow_index(camera);
    PosGetterD previous = (index >= 0 && orig_pos_d[index]) ? orig_pos_d[index] : (PosGetterD)(image + NATIVE_POS_D);
    previous(camera, out);
    double position[3];
    if (drone_probe_override_position(camera, position)) {
        out[0] = position[0];
        out[1] = position[1];
        out[2] = position[2];
    }
    return out;
}

#define UPDATE_CALLBACK(name, saved, view_id)                                                      \
    static void name(void *camera, float dt, unsigned char reset) {                                \
        saved(camera, dt, reset);                                                                  \
        drone_probe_frame(camera, view_id, image, 1, dt);                                          \
    }
UPDATE_CALLBACK(h_extra1, orig_extra1, 4)
UPDATE_CALLBACK(h_extra2, orig_extra2, 4)
UPDATE_CALLBACK(h_extra3, orig_extra3, 4)
UPDATE_CALLBACK(h_extra4, orig_extra4, 4)
UPDATE_CALLBACK(h_free0, orig_free[0], 7)
UPDATE_CALLBACK(h_free1, orig_free[1], 8)
UPDATE_CALLBACK(h_free2, orig_free[2], 9)
UPDATE_CALLBACK(h_free3, orig_free[3], 10)
UPDATE_CALLBACK(h_free4, orig_free[4], 11)

static void h_director(void *director, float dt, unsigned char a, unsigned char b) {
    void *vtable = 0;
    memory_read(director, &vtable, sizeof(vtable));
    int index = (vtable == image + DIRECTOR_B) ? 1 : 0;
    orig_director[index](director, dt, a, b);
    drone_probe_director(director, (uintptr_t)image);
}

static float h_fov(void *camera) {
    CameraView view = camera_type(camera);
    int index = view ? (int)view - 1 : 0;
    float fov = orig_fov[index](camera);
    float divisor = drone_probe_fov_divisor();
    if (divisor > 0.01f && float_is_finite(fov)) {
        fov /= divisor;
    }
    return fov;
}

// Car visibility tests of the mounted cameras: these natives are constant stubs,
// replaced so the car body / driver can be shown or hidden for the free cameras.
static unsigned char h_car_a(void *camera) {
    (void)camera;
    return drone_probe_car_flag(0) ? 1 : 0;
}

static unsigned char h_car_b(void *camera) {
    if (drone_probe_car_flag(1)) return 1;
    if (camera_type(camera) == CAMERA_BUMPER) return 0;
    unsigned char value = 0;
    memory_read((unsigned char *)camera + 0x219, &value, 1);
    return value;
}

static unsigned char h_car_c(void *camera) {
    if (drone_probe_car_flag(2)) return 0;
    return camera_type(camera) == CAMERA_DRIVER ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Hook installation.
// ---------------------------------------------------------------------------
static unsigned hooks_installed, hooks_skipped;

typedef struct {
    unsigned slot_rva;
    void *replacement;
    void **previous; // where the chained pointer is kept (0 for stub replacements)
} HookRecord;
static HookRecord hook_records[96];
static unsigned hook_record_count;

static int write_slot(void **slot, void *value) {
    DWORD protection;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection)) {
        return 0;
    }
    *slot = value;
    DWORD ignored;
    VirtualProtect(slot, sizeof(void *), protection, &ignored);
    return 1;
}

// Replaces the 8-byte pointer at image+slot_rva. The previous pointer is returned
// through *previous so callbacks can chain. Returns 0 when the slot is unusable.
static int hook_slot(unsigned slot_rva, void *replacement, void **previous) {
    void **slot = (void **)(image + slot_rva);
    void *current = 0;
    if (!memory_read_pointer(slot, &current) || hook_record_count >= ARRAY_COUNT(hook_records)) {
        ++hooks_skipped;
        return 0;
    }
    if (previous) {
        *previous = current;
    }
    if (!write_slot(slot, replacement)) {
        ++hooks_skipped;
        return 0;
    }
    hook_records[hook_record_count++] = (HookRecord){slot_rva, replacement, previous};
    ++hooks_installed;
    return 1;
}

// For stub-replacement hooks: only patch when the slot still holds the expected native.
static int hook_slot_expect(unsigned slot_rva, unsigned expected_rva, void *replacement) {
    void *current = 0;
    if (!memory_read_pointer(image + slot_rva, &current) || current != image + expected_rva) {
        ++hooks_skipped;
        log_append_value(PLUGIN_NAME ": car slot not stock, skipped rva=", (int)slot_rva);
        return 0;
    }
    return hook_slot(slot_rva, replacement, 0);
}

// Extra Options can rewrite a slot when the user changes a camera option in game.
// If a slot was put back to what we saved (or to a native function), hook it again.
// A slot now holding some other mod's hook (which chains through us) is left alone.
static void rehook_if_reset(void) {
    for (unsigned index = 0; index < hook_record_count; ++index) {
        HookRecord *record = &hook_records[index];
        void **slot = (void **)(image + record->slot_rva);
        void *current = 0;
        if (!memory_read_pointer(slot, &current) || current == record->replacement) {
            continue;
        }
        int is_native = (unsigned char *)current >= image &&
                        (unsigned char *)current < image + EXPECTED_IMAGE_SIZE;
        if (record->previous) {
            if (current == *record->previous || is_native) {
                *record->previous = current;
                if (write_slot(slot, record->replacement)) {
                    log_append_value(PLUGIN_NAME ": re-hooked slot rva=", (int)record->slot_rva);
                }
            }
        } else if (is_native && write_slot(slot, record->replacement)) {
            log_append_value(PLUGIN_NAME ": re-hooked stub slot rva=", (int)record->slot_rva);
        }
    }
}

static void install_hooks(void) {
    static const unsigned mounted[3] = {DRIVER, HOOD, BUMPER};
    static const unsigned follow[3] = {FOLLOW, FOLLOW_NEAR, FOLLOW_FAR};
    static const unsigned all_views[6] = {DRIVER, HOOD, BUMPER, FOLLOW, FOLLOW_NEAR, FOLLOW_FAR};
    static const unsigned free_cameras[5] = {FREE_CAMERA, FREE_TC_CAMERA, FREE_TRACK_CAMERA, ANIM_CAMERA,
                                             ORTHO_CAMERA};
    static CameraUpdate *const free_saved[5] = {&orig_free[0], &orig_free[1], &orig_free[2], &orig_free[3],
                                                &orig_free[4]};
    static void *const free_callbacks[5] = {(void *)h_free0, (void *)h_free1, (void *)h_free2,
                                            (void *)h_free3, (void *)h_free4};

    for (int i = 0; i < 3; ++i) {
        hook_slot(mounted[i] + SLOT_UPDATE, (void *)h_mounted, (void **)&orig_mounted[i]);
        hook_slot(follow[i] + SLOT_UPDATE, (void *)h_follow, (void **)&orig_follow[i]);
        hook_slot(follow[i] + SLOT_POS_F, (void *)h_pos_f, (void **)&orig_pos_f[i]);
        hook_slot(follow[i] + SLOT_POS_D, (void *)h_pos_d, (void **)&orig_pos_d[i]);
    }
    for (int i = 0; i < 6; ++i) {
        hook_slot(all_views[i] + SLOT_VIEW_MATRIX, (void *)h_view_matrix, (void **)&orig_vm[i]);
    }
    hook_slot(EXTRA1 + SLOT_UPDATE, (void *)h_extra1, (void **)&orig_extra1);
    hook_slot(EXTRA2 + SLOT_UPDATE, (void *)h_extra2, (void **)&orig_extra2);
    hook_slot(EXTRA3 + SLOT_UPDATE, (void *)h_extra3, (void **)&orig_extra3);
    hook_slot(EXTRA4 + SLOT_UPDATE, (void *)h_extra4, (void **)&orig_extra4);
    for (int i = 0; i < 5; ++i) {
        hook_slot(free_cameras[i] + SLOT_UPDATE, free_callbacks[i], (void **)free_saved[i]);
    }
    hook_slot(DIRECTOR_A + SLOT_UPDATE, (void *)h_director, (void **)&orig_director[0]);
    hook_slot(DIRECTOR_B + SLOT_UPDATE, (void *)h_director, (void **)&orig_director[1]);

    // Field of view getters (drone zoom): the camera_vtables order matches orig_fov.
    for (int i = 0; i < 6; ++i) {
        hook_slot(camera_vtables[i] + SLOT_FOV, (void *)h_fov, (void **)&orig_fov[i]);
    }

    // Car visibility stubs of the mounted cameras.
    hook_slot_expect(DRIVER + 0x238, 0xa41740, (void *)h_car_a);
    hook_slot_expect(HOOD + 0x238, 0xa41740, (void *)h_car_a);
    hook_slot_expect(BUMPER + 0x238, 0xa41740, (void *)h_car_a);
    hook_slot_expect(DRIVER + 0x1b8, 0x1031f30, (void *)h_car_b);
    hook_slot_expect(HOOD + 0x1b8, 0x1031f30, (void *)h_car_b);
    hook_slot_expect(BUMPER + 0x1b8, 0xa41740, (void *)h_car_b);
    hook_slot_expect(DRIVER + 0x1d0, 0xc47af0, (void *)h_car_c);
    hook_slot_expect(HOOD + 0x1d0, 0xa41740, (void *)h_car_c);
    hook_slot_expect(BUMPER + 0x1d0, 0xa41740, (void *)h_car_c);
}

__declspec(dllexport) BOOL WINAPI FH2PluginInitialize(const FH2PluginContext *context) {
    host = context;
    if (!context || context->size < sizeof(FH2PluginContext) || !context->game_image) {
        return FALSE;
    }
    log_append(PLUGIN_NAME " " PLUGIN_VERSION ": starting (Extra Options ");
    log_append(context->extra_options_version ? context->extra_options_version : "?");
    log_append(")\r\n");
    if (context->game_image_size != EXPECTED_IMAGE_SIZE) {
        log_append_value(PLUGIN_NAME ": unsupported game image size ", (int)context->game_image_size);
        return FALSE;
    }
    image = (unsigned char *)context->game_image;
    install_hooks();
    log_append_value(PLUGIN_NAME ": hooks installed=", (int)hooks_installed);
    log_append_value(PLUGIN_NAME ": hooks skipped=", (int)hooks_skipped);
    return hooks_installed != 0;
}

// Called every frame on the local car's update thread: a cheap periodic slot check.
__declspec(dllexport) void WINAPI FH2PluginVehicleUpdate(const FH2PluginVehicleFrame *frame) {
    static unsigned ticks;
    (void)frame;
    if (image && (++ticks & 127u) == 0) {
        rehook_if_reset();
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}

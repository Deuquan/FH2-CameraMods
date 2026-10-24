#include "camera_views.h"
#include "features.h"
#include "support.h"
#include "video_settings.h"
#include <xmmintrin.h>

#define HEADING_THUNK_RVA 0x103b261u
#define CHASE_THUNK_RVA 0x103b281u

enum {
    FOLLOW = 0x2e783a0,
    FOLLOW_NEAR = 0x2e78610,
    FOLLOW_FAR = 0x2e78880,
    DRIVER = 0x2e78120,
    HOOD = 0x2e77ea8,
    BUMPER = 0x2e703b0
};

enum {
    TARGET_PREDICTED_VTABLE = 0x2e79540,
    TARGET_CAR_VTABLE = 0x2c5ce28,
    TARGET_PLAYER_CAR_VTABLE = 0x2e6b348,
    CAR_BODY_OFFSET = 0x4be8,
    NATIVE_CAR_VELOCITY = 0xf646b0,
    NATIVE_CAR_AXIS = 0xf62220,
    WHEEL_SLIP_SLOT = 0x1d8,
    WHEEL_CONTACT_SLOT = 0x4e0,
    NATIVE_WHEEL_SLIP = 0x12c5cb0,
    NATIVE_WHEEL_CONTACT = 0x12e8040,
    CAMERA_SPRING_OFFSET = 0x180,
    CAMERA_HEADING_OFFSET = 0x1c0,
    CAMERA_VIEW_DIRTY_OFFSET = 0x21a,
    NATIVE_ATAN2 = 0xf17d52,
    NATIVE_HEADING_UPDATE = 0x1036920,
    NATIVE_ATTACHED_UPDATE = 0x103c590,
    NATIVE_SIN_COS = 0x1251ce0,
};

static const unsigned camera_vtables[] = {FOLLOW, FOLLOW_NEAR, FOLLOW_FAR, DRIVER, HOOD, BUMPER};

typedef float (*ArcTangent)(float, float);
typedef void *(*VectorGetter)(void *, float *);
typedef void *(*AxisGetter)(void *, float *, int);
typedef float (*WheelSlip)(void *, int);
typedef void *(*WheelContact)(void *, int);
typedef void (*CameraUpdate)(void *, float, unsigned char);
typedef void (*SinCos)(float, float *, float *);

typedef struct {
    void *vtable, *target;
    unsigned char unused[0x170];
    float *spring;
    unsigned char unused_after_spring[0x38];
    float yaw, yaw_speed;
} HeadingFrame;

typedef struct {
    void *camera, *target;
    float angle, angular_speed, body_heading, slide;
} DriftOffset;

typedef struct {
    float stiffness, damping;
} DriftSpring;

_Static_assert(offsetof(HeadingFrame, spring) == CAMERA_SPRING_OFFSET, "Native spring offset");
_Static_assert(offsetof(HeadingFrame, yaw) == CAMERA_HEADING_OFFSET, "Native yaw offset");

static unsigned char *image;
static ArcTangent native_atan2;
static CameraUpdate native_heading_update, native_attached_update;
static SinCos sin_cos;
static DriftOffset mounted_offsets[3];
static void *chase_cameras[3];
static float chase_slide_blend[3];
static unsigned char heading_thunk[15];
static const unsigned char unused_thunk[15] = {0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
                                               0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
static const unsigned char *const accepted_thunk[] = {unused_thunk, heading_thunk};
static unsigned char chase_thunk[12], original_update[8], attached_callback[8];
static const unsigned char unused_chase_thunk[12] = {0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
                                                     0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
static const unsigned char *const accepted_chase_thunk[] = {unused_chase_thunk, chase_thunk};
static const unsigned char *const accepted_update[] = {original_update, attached_callback};
/* Fahrer/Haube/Stossstange: je Kamera eigene Original-Funktion (z.B. wenn ForzaInfinity die Stelle schon belegt, wird dahinter eingehaengt) */
static CameraUpdate native_mounted[3];
typedef void *(*ViewMatrixFn)(void *, void *);
static ViewMatrixFn native_vm[6];
static unsigned char orig_vm[6][8], vm_callback[8];
static const unsigned char *const acc_vm0[] = {orig_vm[0], vm_callback};
static const unsigned char *const acc_vm1[] = {orig_vm[1], vm_callback};
static const unsigned char *const acc_vm2[] = {orig_vm[2], vm_callback};
static const unsigned char *const acc_vm3[] = {orig_vm[3], vm_callback};
static const unsigned char *const acc_vm4[] = {orig_vm[4], vm_callback};
static const unsigned char *const acc_vm5[] = {orig_vm[5], vm_callback};
static unsigned char orig_mounted[3][8];
static const unsigned char *const acc_mounted0[] = {orig_mounted[0], attached_callback};
static const unsigned char *const acc_mounted1[] = {orig_mounted[1], attached_callback};
static const unsigned char *const acc_mounted2[] = {orig_mounted[2], attached_callback};
static unsigned char original_follow[8], follow_callback[8];
static unsigned char original_dir[8], dir_callback[8];
static const unsigned char *const accepted_dir[] = {original_dir, dir_callback};
static unsigned char original_x2[8], x2_callback[8], original_x3[8], x3_callback[8], extra_callback[8];
static const unsigned char *const accepted_extra[] = {original_update, extra_callback};
static const unsigned char *const accepted_x2[] = {original_x2, x2_callback};
static const unsigned char *const accepted_x3[] = {original_x3, x3_callback};
static unsigned char original_posf[8], posf_callback[8], original_posd[8], posd_callback[8];
static const unsigned char *const accepted_posf[] = {original_posf, posf_callback};
static const unsigned char *const accepted_posd[] = {original_posd, posd_callback};
static const unsigned char *const accepted_follow[] = {original_follow, follow_callback};

static CameraView camera_type(void *camera) {
    void *vtable;
    if (!memory_read_pointer(camera, &vtable)) {
        return 0;
    }
    for (unsigned index = 0; index < ARRAY_COUNT(camera_vtables); ++index) {
        if (vtable == image + camera_vtables[index]) {
            return (CameraView)(index + 1);
        }
    }
    return 0;
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

/* Fuer den Beifahrer-Modus: echte Fahrtrichtung (Karosserie) und Geschwindigkeit des Autos */
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

static float slide_weight(void *body) {
    void *vtable, *slip_getter, *contact_getter;
    if (!memory_read_pointer(body, &vtable) ||
        !memory_read_pointer((unsigned char *)vtable + WHEEL_SLIP_SLOT, &slip_getter) ||
        !memory_read_pointer((unsigned char *)vtable + WHEEL_CONTACT_SLOT, &contact_getter) ||
        slip_getter != image + NATIVE_WHEEL_SLIP ||
        contact_getter != image + NATIVE_WHEEL_CONTACT) {
        return 0;
    }
    float slip = 0;
    for (int axle = 0; axle < 2; ++axle) {
        float total = 0;
        unsigned grounded = 0;
        for (int wheel = axle * 2; wheel < axle * 2 + 2; ++wheel) {
            if (!((WheelContact)contact_getter)(body, wheel)) {
                continue;
            }
            float lateral = ((WheelSlip)slip_getter)(body, wheel);
            if (!float_is_finite(lateral)) {
                return 0;
            }
            total += lateral < 0 ? -lateral : lateral;
            ++grounded;
        }
        if (grounded && total / grounded > slip) {
            slip = total / grounded;
        }
    }
    // Normalized lateral slip reaches the tire's grip limit at one.
    if (slip <= 1) {
        return 0;
    }
    if (slip >= 1.35f) {
        return 1;
    }
    float blend = (slip - 1) / 0.35f;
    return blend * blend * (3 - 2 * blend);
}

static float heading_difference(float heading, float reference) {
    float difference = heading - reference;
    if (difference > 3.141592653589793f) {
        difference -= 6.283185307179586f;
    } else if (difference < -3.141592653589793f) {
        difference += 6.283185307179586f;
    }
    return difference;
}

static float momentum_weight(float body_z, float body_x, const float velocity[4],
                             float direction[2]) {
    float speed_squared = velocity[0] * velocity[0] + velocity[2] * velocity[2];
    float body_squared = body_x * body_x + body_z * body_z;
    if (!(speed_squared > 4 && speed_squared < 1000000 && body_squared > 0.5f &&
          body_squared < 1.5f)) {
        return 0;
    }
    float inverse_speed = 1 / _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(speed_squared)));
    float x = -velocity[0] * inverse_speed;
    float z = -velocity[2] * inverse_speed;
    float alignment = x * body_x + z * body_z;
    if (alignment <= -0.5f) {
        return 0;
    }
    float blend = speed_squared >= 16 ? 1 : (speed_squared - 4) / 12;
    blend = blend * blend * (3 - 2 * blend);
    if (alignment < 0) {
        blend *= (alignment + 0.5f) * 2;
    }
    direction[0] = x;
    direction[1] = z;
    return blend;
}

static float momentum_heading(float body_z, float body_x, void *camera) {
    _Alignas(16) float velocity[4] = {0};
    void *body;
    CameraView view = camera_type(camera);
    float body_heading = native_atan2(body_z, body_x);
    float fallback = view >= CAMERA_DRIVER ? 0 : body_heading;
    if (!float_is_finite(body_x) || !float_is_finite(body_z) || !view ||
        !drift_view_enabled(view) || !(body = car_body(camera, velocity, NULL))) {
        return fallback;
    }
    float direction[2];
    float blend = slide_weight(body) * momentum_weight(body_z, body_x, velocity, direction);
    if (!blend) {
        return fallback;
    }
    float heading = native_atan2(body_z + (direction[1] - body_z) * blend,
                                 body_x + (direction[0] - body_x) * blend);
    float offset = drift_angle(view, heading_difference(heading, body_heading));
    return view >= CAMERA_DRIVER ? offset : body_heading + offset;
}

static DriftSpring drift_spring_tuning(CameraView view) {
    float look_speed = drift_look_speed(view);
    if (view >= CAMERA_DRIVER) {
        return (DriftSpring){look_speed * look_speed / 4.5f, 2 * look_speed / 4.4f};
    }
    float damping_scale = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(look_speed)));
    return (DriftSpring){0.55f * look_speed, 0.5f * damping_scale};
}

static void tune_drift_spring(float spring[12], float slide, const DriftSpring *tuning) {
    spring[8] *= 1 + (tuning->stiffness - 1) * slide;
    spring[9] *= 1 + (tuning->damping - 1) * slide;
    spring[10] *= 1 - slide;
}

static float recover_slide(float previous, float slide, float dt) {
    if (slide < previous && dt > 0 && dt < 0.25f) {
        float decay = dt * 4 / (1 + dt * 4);
        slide = previous + (slide - previous) * decay;
        if (slide < 0.0001f) {
            slide = 0;
        }
    }
    return slide;
}

static void update_chase_heading(void *camera, float dt, unsigned char reset) {
    CameraView view = camera_type(camera);
    drone_probe_frame(camera, (unsigned)view, image, 0, dt);
    HeadingFrame frame;
    float spring[12];
    _Alignas(16) float velocity[4];
    float slide = drift_view_enabled(view) ? slide_weight(car_body(camera, velocity, NULL)) : 0;
    if (view && view <= CAMERA_CHASE_FAR && float_is_finite(dt) && dt > 0 && dt < 0.25f) {
        unsigned index = view - CAMERA_CHASE;
        if (chase_cameras[index] != camera || reset) {
            chase_cameras[index] = camera;
            chase_slide_blend[index] = slide;
        }
        slide = recover_slide(chase_slide_blend[index], slide, dt);
        chase_slide_blend[index] = slide;
    }
    void *settings;
    if (!view || view > CAMERA_CHASE_FAR || !slide || !float_is_finite(dt) ||
        !memory_read_pointer((unsigned char *)camera + CAMERA_SPRING_OFFSET, &settings) ||
        !memory_read(settings, spring, sizeof(spring)) ||
        !memory_read(camera, &frame.vtable, 2 * sizeof(void *)) ||
        !memory_read((unsigned char *)camera + CAMERA_HEADING_OFFSET, &frame.yaw,
                     2 * sizeof(float)) ||
        !float_is_finite(spring[8]) || !float_is_finite(spring[9]) ||
        !float_is_finite(spring[11])) {
        native_heading_update(camera, dt, reset);
        return;
    }
    DriftSpring tuning = drift_spring_tuning(view);
    tune_drift_spring(spring, slide, &tuning);
    frame.spring = spring;
    native_heading_update(&frame, dt, reset);
    memory_copy((unsigned char *)camera + CAMERA_HEADING_OFFSET, &frame.yaw, 2 * sizeof(float));
}

static void update_mounted_camera_inner(void *camera, float dt, unsigned char reset);
static CameraUpdate native_follow_update;
static CameraUpdate native_x2_update, native_x3_update;
static CameraUpdate native_director_update;
static void update_director(void *director, float dt, unsigned char a, unsigned char b) {
    ((void (*)(void *, float, unsigned char, unsigned char))native_director_update)(director, dt, a, b);
    drone_probe_director(director, (uintptr_t)image);
}
static void update_extra_attached(void *camera, float dt, unsigned char reset) {
    native_attached_update(camera, dt, reset);
    drone_probe_frame(camera, 4, image, 1, dt);
}
static void update_extra_x2(void *camera, float dt, unsigned char reset) {
    native_x2_update(camera, dt, reset);
    drone_probe_frame(camera, 4, image, 1, dt);
}
static void update_extra_x3(void *camera, float dt, unsigned char reset) {
    native_x3_update(camera, dt, reset);
    drone_probe_frame(camera, 4, image, 1, dt);
}
typedef void *(*PosGetterF)(void *, float *);
typedef void *(*PosGetterD)(void *, double *);
static PosGetterF native_pos_f;
static PosGetterD native_pos_d;
static void *follow_pos_f(void *camera, float *out) {
    drone_probe_note_caller(1, (uintptr_t)__builtin_return_address(0), (uintptr_t)image);
    native_pos_f(camera, out);
    double position[3];
    if (drone_probe_override_position(camera, position)) {
        double native_pos[3] = {out[0], out[1], out[2]};
        drone_probe_getter_diff(1, native_pos, position);
        out[0] = (float)position[0];
        out[1] = (float)position[1];
        out[2] = (float)position[2];
    }
    return out;
}
static void *follow_pos_d(void *camera, double *out) {
    drone_probe_note_caller(2, (uintptr_t)__builtin_return_address(0), (uintptr_t)image);
    native_pos_d(camera, out);
    double position[3];
    if (drone_probe_override_position(camera, position)) {
        drone_probe_getter_diff(2, out, position);
        out[0] = position[0];
        out[1] = position[1];
        out[2] = position[2];
    }
    return out;
}
static void update_follow_camera(void *camera, float dt, unsigned char reset) {
    if (reset) {
        drone_probe_reset_seen((unsigned)camera_type(camera));
    }
    if (drone_probe_owns_camera(camera)) {
        reset = 0; /* Drohne aktiv: kein Kamera-Reset durch das Spiel */
    }
    drone_probe_before_update(camera);
    native_follow_update(camera, dt, reset);
    drone_probe_frame(camera, (unsigned)camera_type(camera), image, 1, dt);
    { CameraView cv = camera_type(camera); if (cv >= CAMERA_CHASE && cv <= CAMERA_CHASE_FAR) { drone_probe_ego_view(1); drone_probe_ego_update(camera, dt); } }
}
static void *view_matrix_hook(void *camera, void *out) {
    CameraView view = camera_type(camera);
    int vi = (view >= CAMERA_DRIVER && view <= CAMERA_BUMPER) ? (int)(view - CAMERA_DRIVER)
             : (view >= CAMERA_CHASE && view <= CAMERA_CHASE_FAR) ? 3 + (int)(view - CAMERA_CHASE) : -1;
    if (vi < 0 || !native_vm[vi]) return out;
    void *result = native_vm[vi](camera, out);
    if (drone_probe_owns_camera(camera)) drone_probe_apply_roll(out);
    else { drone_probe_ego_view(vi >= 3); drone_probe_ego_apply(camera, out); }
    return result;
}
PatchDefinition drift_camera_vm_hook_patches[6] = {
    {DRIVER + 0x50, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_vm_driver", orig_vm[0], vm_callback, vm_callback, acc_vm0, 2},
    {HOOD + 0x50, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_vm_hood", orig_vm[1], vm_callback, vm_callback, acc_vm1, 2},
    {BUMPER + 0x50, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_vm_bumper", orig_vm[2], vm_callback, vm_callback, acc_vm2, 2},
    {FOLLOW + 0x50, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_vm_follow", orig_vm[3], vm_callback, vm_callback, acc_vm3, 2},
    {FOLLOW_NEAR + 0x50, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_vm_follow_near", orig_vm[4], vm_callback, vm_callback, acc_vm4, 2},
    {FOLLOW_FAR + 0x50, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_vm_follow_far", orig_vm[5], vm_callback, vm_callback, acc_vm5, 2}};
static void update_mounted_camera(void *camera, float dt, unsigned char reset) {
    update_mounted_camera_inner(camera, dt, reset);
    drone_probe_frame(camera, (unsigned)camera_type(camera), image, 1, dt);
    { CameraView ev = camera_type(camera); if (ev >= CAMERA_DRIVER && ev <= CAMERA_BUMPER) { drone_probe_ego_view(0); drone_probe_ego_update(camera, dt); } }
}

static void update_mounted_camera_inner(void *camera, float dt, unsigned char reset) {
    CameraView view = camera_type(camera);
    if (view >= CAMERA_DRIVER && view <= CAMERA_BUMPER && native_mounted[view - CAMERA_DRIVER]) {
        native_mounted[view - CAMERA_DRIVER](camera, dt, reset);
    } else {
        native_attached_update(camera, dt, reset);
    }
    _Alignas(16) float forward[4], velocity[4], axis[4];
    void *target;
    if (view < CAMERA_DRIVER || view > CAMERA_BUMPER) {
        return;
    }
    DriftOffset *state = &mounted_offsets[view - CAMERA_DRIVER];
    void *body = car_body(camera, velocity, axis);
    float direction[2];
    float slide =
        body && drift_view_enabled(view)
            ? slide_weight(body) * momentum_weight(-axis[2], -axis[0], velocity, direction)
            : 0;
    if (!float_is_finite(dt) || dt < 0 || dt >= 0.25f || !body ||
        !memory_read((unsigned char *)camera + CAMERA_HEADING_OFFSET, forward, sizeof(forward)) ||
        !memory_read_pointer((unsigned char *)camera + 8, &target)) {
        *state = (DriftOffset){0};
        return;
    }
    if (!float_is_finite(forward[0]) || !float_is_finite(forward[1]) ||
        !float_is_finite(forward[2])) {
        return;
    }
    if (!slide &&
        (!state->camera || (state->angle > -0.00001f && state->angle < 0.00001f &&
                            state->angular_speed > -0.0001f && state->angular_speed < 0.0001f))) {
        *state = (DriftOffset){0};
        return;
    }
    int initialize = state->camera != camera || state->target != target;
    float body_heading = native_atan2(-axis[2], -axis[0]);
    if (initialize) {
        *state = (DriftOffset){
            .camera = camera, .target = target, .body_heading = body_heading, .slide = slide};
        return;
    }
    state->slide = recover_slide(state->slide, slide, dt);
    if (drift_view_enabled(view) && clamp_drift_angle(view, 1) > 0) {
        float body_turn = heading_difference(body_heading, state->body_heading);
        float correction = body_turn * state->slide * drift_sensitivity(view);
        state->angle = heading_difference(state->angle - correction, 0);
    }
    state->body_heading = body_heading;
    float spring[12] = {0, 0, 0, 0, 0, 0, 0, 0, 4.5f, 4.4f, 0.67f, 2};
    DriftSpring tuning = drift_spring_tuning(view);
    tune_drift_spring(spring, 1, &tuning);
    HeadingFrame frame;
    frame.vtable = image + camera_vtables[view - CAMERA_CHASE];
    frame.target = target;
    frame.spring = spring;
    frame.yaw = state->angle;
    frame.yaw_speed = state->angular_speed;
    // Rebase the offset as the body turns so the spring smooths world heading.
    native_heading_update(&frame, dt, 0);
    if (!float_is_finite(frame.yaw) || !float_is_finite(frame.yaw_speed)) {
        *state = (DriftOffset){0};
        return;
    }
    state->angle = clamp_drift_angle(view, frame.yaw);
    if ((frame.yaw - state->angle) * frame.yaw_speed > 0) {
        frame.yaw_speed = 0;
    }
    state->angular_speed = frame.yaw_speed;
    float sine, cosine;
    sin_cos(-state->angle, &sine, &cosine);
    float x = forward[0], z = forward[2];
    forward[0] = cosine * x + sine * z;
    forward[2] = cosine * z - sine * x;
    memory_copy((unsigned char *)camera + CAMERA_HEADING_OFFSET, forward, sizeof(forward));
    *((unsigned char *)camera + CAMERA_VIEW_DIRTY_OFFSET) = 1;
}

PatchDefinition drift_camera_hook_patch = {HEADING_THUNK_RVA,
                                           0,
                                           sizeof(heading_thunk),
                                           GROUP_DRIFT_CAMERA,
                                           "drift_camera_heading_thunk",
                                           unused_thunk,
                                           heading_thunk,
                                           heading_thunk,
                                           accepted_thunk,
                                           2};

#define ATTACHED_HOOK(table, name, idx)                                                            \
    {table + 0x20,                                                                                 \
     0,                                                                                            \
     8,                                                                                            \
     GROUP_DRIFT_CAMERA,                                                                           \
     name,                                                                                         \
     orig_mounted[idx],                                                                            \
     attached_callback,                                                                            \
     attached_callback,                                                                            \
     acc_mounted##idx,                                                                             \
     2}

#define FOLLOW_HOOK(table, name)                                                                   \
    {table + 0x20, 0, 8, GROUP_DRIFT_CAMERA, name, original_follow, follow_callback, follow_callback, \
     accepted_follow, 2}

#define POS_HOOK(table, slot, name, orig, cb, accepted)                                           \
    {table + slot, 0, 8, GROUP_DRIFT_CAMERA, name, orig, cb, cb, accepted, 2}

#define EXTRA_HOOK(table, name, orig, cb, accepted)                                                \
    {table + 0x20, 0, 8, GROUP_DRIFT_CAMERA, name, orig, cb, cb, accepted, 2}

static unsigned char car_orig[9][8], car_cb[3][8];
static const unsigned char *const car_acc0[] = {car_orig[0], car_cb[0]};
static const unsigned char *const car_acc1[] = {car_orig[1], car_cb[0]};
static const unsigned char *const car_acc2[] = {car_orig[2], car_cb[0]};
static const unsigned char *const car_acc3[] = {car_orig[3], car_cb[1]};
static const unsigned char *const car_acc4[] = {car_orig[4], car_cb[1]};
static const unsigned char *const car_acc5[] = {car_orig[5], car_cb[1]};
static const unsigned char *const car_acc6[] = {car_orig[6], car_cb[2]};
static const unsigned char *const car_acc7[] = {car_orig[7], car_cb[2]};
static const unsigned char *const car_acc8[] = {car_orig[8], car_cb[2]};

static unsigned char car_hide_a(void *camera) {
    (void)camera;
    return drone_probe_car_flag(0) ? 1 : 0;
}
static unsigned char car_hide_b(void *camera) {
    if (drone_probe_car_flag(1)) return 1;
    if (camera_type(camera) == CAMERA_BUMPER) return 0;
    unsigned char v = 0;
    memory_read((unsigned char *)camera + 0x219, &v, 1);
    return v;
}
static unsigned char car_hide_c(void *camera) {
    if (drone_probe_car_flag(2)) return 0;
    return camera_type(camera) == CAMERA_DRIVER ? 1 : 0;
}

PatchDefinition drift_camera_car_hook_patches[9] = {
    {DRIVER + 0x238, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_car_a_driver", car_orig[0], car_cb[0], car_cb[0], car_acc0, 2},
    {HOOD + 0x238, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_car_a_hood", car_orig[1], car_cb[0], car_cb[0], car_acc1, 2},
    {BUMPER + 0x238, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_car_a_bumper", car_orig[2], car_cb[0], car_cb[0], car_acc2, 2},
    {DRIVER + 0x1b8, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_car_b_driver", car_orig[3], car_cb[1], car_cb[1], car_acc3, 2},
    {HOOD + 0x1b8, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_car_b_hood", car_orig[4], car_cb[1], car_cb[1], car_acc4, 2},
    {BUMPER + 0x1b8, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_car_b_bumper", car_orig[5], car_cb[1], car_cb[1], car_acc5, 2},
    {DRIVER + 0x1d0, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_car_c_driver", car_orig[6], car_cb[2], car_cb[2], car_acc6, 2},
    {HOOD + 0x1d0, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_car_c_hood", car_orig[7], car_cb[2], car_cb[2], car_acc7, 2},
    {BUMPER + 0x1d0, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_car_c_bumper", car_orig[8], car_cb[2], car_cb[2], car_acc8, 2}};

static CameraUpdate native_fc[5];
static unsigned char fc_orig[5][8], fc_cb[5][8];
static const unsigned char *const fc_acc0[] = {fc_orig[0], fc_cb[0]};
static void update_fc0(void *camera, float dt, unsigned char reset) {
    native_fc[0](camera, dt, reset);
    drone_probe_frame(camera, 7, image, 1, dt);
}
static const unsigned char *const fc_acc1[] = {fc_orig[1], fc_cb[1]};
static void update_fc1(void *camera, float dt, unsigned char reset) {
    native_fc[1](camera, dt, reset);
    drone_probe_frame(camera, 8, image, 1, dt);
}
static const unsigned char *const fc_acc2[] = {fc_orig[2], fc_cb[2]};
static void update_fc2(void *camera, float dt, unsigned char reset) {
    native_fc[2](camera, dt, reset);
    drone_probe_frame(camera, 9, image, 1, dt);
}
static const unsigned char *const fc_acc3[] = {fc_orig[3], fc_cb[3]};
static void update_fc3(void *camera, float dt, unsigned char reset) {
    native_fc[3](camera, dt, reset);
    drone_probe_frame(camera, 10, image, 1, dt);
}
static const unsigned char *const fc_acc4[] = {fc_orig[4], fc_cb[4]};
static void update_fc4(void *camera, float dt, unsigned char reset) {
    native_fc[4](camera, dt, reset);
    drone_probe_frame(camera, 11, image, 1, dt);
}
PatchDefinition drift_camera_free_hook_patches[5] = {
    {0x2e78af0 + 0x20, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_free_update", fc_orig[0], fc_cb[0], fc_cb[0], fc_acc0, 2},
    {0x2e78d98 + 0x20, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_freetc_update", fc_orig[1], fc_cb[1], fc_cb[1], fc_acc1, 2},
    {0x2e79040 + 0x20, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_freetrack_update", fc_orig[2], fc_cb[2], fc_cb[2], fc_acc2, 2},
    {0x2e779d8 + 0x20, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_anim_update", fc_orig[3], fc_cb[3], fc_cb[3], fc_acc3, 2},
    {0x2e792e8 + 0x20, 0, 8, GROUP_DRIFT_CAMERA, "drift_camera_ortho_update", fc_orig[4], fc_cb[4], fc_cb[4], fc_acc4, 2}};

PatchDefinition drift_camera_view_hook_patches[19] = {
    {CHASE_THUNK_RVA, 0, sizeof(chase_thunk), GROUP_DRIFT_CAMERA, "drift_camera_chase_spring_thunk",
     unused_chase_thunk, chase_thunk, chase_thunk, accepted_chase_thunk, 2},
    ATTACHED_HOOK(DRIVER, "drift_camera_driver_update", 0),
    ATTACHED_HOOK(HOOD, "drift_camera_hood_update", 1),
    ATTACHED_HOOK(BUMPER, "drift_camera_bumper_update", 2),
    FOLLOW_HOOK(FOLLOW, "drift_camera_follow_update"),
    FOLLOW_HOOK(FOLLOW_NEAR, "drift_camera_follow_near_update"),
    FOLLOW_HOOK(FOLLOW_FAR, "drift_camera_follow_far_update"),
    POS_HOOK(FOLLOW, 0x150, "drift_camera_follow_pos_f", original_posf, posf_callback, accepted_posf),
    POS_HOOK(FOLLOW_NEAR, 0x150, "drift_camera_follow_near_pos_f", original_posf, posf_callback, accepted_posf),
    POS_HOOK(FOLLOW_FAR, 0x150, "drift_camera_follow_far_pos_f", original_posf, posf_callback, accepted_posf),
    POS_HOOK(FOLLOW, 0x158, "drift_camera_follow_pos_d", original_posd, posd_callback, accepted_posd),
    POS_HOOK(FOLLOW_NEAR, 0x158, "drift_camera_follow_near_pos_d", original_posd, posd_callback, accepted_posd),
    POS_HOOK(FOLLOW_FAR, 0x158, "drift_camera_follow_far_pos_d", original_posd, posd_callback, accepted_posd),
    EXTRA_HOOK(0x2e70628u, "drift_camera_extra1_update", original_update, extra_callback, accepted_extra),
    EXTRA_HOOK(0x2e77c30u, "drift_camera_extra4_update", original_update, extra_callback, accepted_extra),
    EXTRA_HOOK(0x2e708a0u, "drift_camera_extra2_update", original_x2, x2_callback, accepted_x2),
    EXTRA_HOOK(0x2e70b18u, "drift_camera_extra3_update", original_x3, x3_callback, accepted_x3),
    EXTRA_HOOK(0x2e70080u, "drift_camera_director_a_update", original_dir, dir_callback, accepted_dir),
    EXTRA_HOOK(0x2c6c840u, "drift_camera_director_b_update", original_dir, dir_callback, accepted_dir)};

void drift_camera_prepare_hook(unsigned char *game_image) {
    image = game_image;
    native_atan2 = (ArcTangent)(image + NATIVE_ATAN2);
    native_heading_update = (CameraUpdate)(image + NATIVE_HEADING_UPDATE);
    native_attached_update = (CameraUpdate)(image + NATIVE_ATTACHED_UPDATE);
    sin_cos = (SinCos)(image + NATIVE_SIN_COS);
    // Forward the native heading call's RSI camera as its third argument.
    static const unsigned char code[15] = {0x49, 0x89, 0xf0, 0x48, 0xb8, 0,    0,   0,
                                           0,    0,    0,    0,    0,    0xff, 0xe0};
    memory_copy(heading_thunk, code, sizeof(code));
    uintptr_t callback = (uintptr_t)momentum_heading;
    memory_copy(heading_thunk + 5, &callback, sizeof(callback));
    static const unsigned char chase_code[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    memory_copy(chase_thunk, chase_code, sizeof(chase_code));
    callback = (uintptr_t)update_chase_heading;
    memory_copy(chase_thunk + 2, &callback, sizeof(callback));
    callback = (uintptr_t)native_attached_update;
    memory_copy(original_update, &callback, sizeof(callback));
    {
        static const uintptr_t mounted_tables[6] = {DRIVER, HOOD, BUMPER, FOLLOW, FOLLOW_NEAR, FOLLOW_FAR};
        for (int mi = 0; mi < 6; ++mi) {
            uintptr_t cur = 0;
            if (mi < 3) {
                memory_copy(&cur, image + mounted_tables[mi] + 0x20, sizeof(cur));
                if (cur == 0) cur = (uintptr_t)native_attached_update;
                memory_copy(orig_mounted[mi], &cur, sizeof(cur));
                native_mounted[mi] = (CameraUpdate)cur;
            }
            uintptr_t vm = 0;
            memory_copy(&vm, image + mounted_tables[mi] + 0x50, sizeof(vm));
            memory_copy(orig_vm[mi], &vm, sizeof(vm));
            native_vm[mi] = (ViewMatrixFn)vm;
        }
        uintptr_t vcb = (uintptr_t)view_matrix_hook;
        memory_copy(vm_callback, &vcb, sizeof(vcb));
    }
    callback = (uintptr_t)update_mounted_camera;
    memory_copy(attached_callback, &callback, sizeof(callback));
    native_follow_update = (CameraUpdate)(image + 0x103c820);
    callback = (uintptr_t)native_follow_update;
    memory_copy(original_follow, &callback, sizeof(callback));
    callback = (uintptr_t)update_follow_camera;
    memory_copy(follow_callback, &callback, sizeof(callback));
    native_director_update = (CameraUpdate)(image + 0xfe6020);
    callback = (uintptr_t)native_director_update;
    memory_copy(original_dir, &callback, sizeof(callback));
    callback = (uintptr_t)update_director;
    memory_copy(dir_callback, &callback, sizeof(callback));
    native_x2_update = (CameraUpdate)(image + 0x103e190);
    native_x3_update = (CameraUpdate)(image + 0x103e0e0);
    callback = (uintptr_t)update_extra_attached;
    memory_copy(extra_callback, &callback, sizeof(callback));
    callback = (uintptr_t)native_x2_update;
    memory_copy(original_x2, &callback, sizeof(callback));
    callback = (uintptr_t)update_extra_x2;
    memory_copy(x2_callback, &callback, sizeof(callback));
    callback = (uintptr_t)native_x3_update;
    memory_copy(original_x3, &callback, sizeof(callback));
    callback = (uintptr_t)update_extra_x3;
    memory_copy(x3_callback, &callback, sizeof(callback));
    {
        uintptr_t cb2;
        cb2 = (uintptr_t)(image + 0xa41740); memory_copy(car_orig[0], &cb2, sizeof(cb2));
        cb2 = (uintptr_t)(image + 0xa41740); memory_copy(car_orig[1], &cb2, sizeof(cb2));
        cb2 = (uintptr_t)(image + 0xa41740); memory_copy(car_orig[2], &cb2, sizeof(cb2));
        cb2 = (uintptr_t)(image + 0x1031f30); memory_copy(car_orig[3], &cb2, sizeof(cb2));
        cb2 = (uintptr_t)(image + 0x1031f30); memory_copy(car_orig[4], &cb2, sizeof(cb2));
        cb2 = (uintptr_t)(image + 0xa41740); memory_copy(car_orig[5], &cb2, sizeof(cb2));
        cb2 = (uintptr_t)(image + 0xc47af0); memory_copy(car_orig[6], &cb2, sizeof(cb2));
        cb2 = (uintptr_t)(image + 0xa41740); memory_copy(car_orig[7], &cb2, sizeof(cb2));
        cb2 = (uintptr_t)(image + 0xa41740); memory_copy(car_orig[8], &cb2, sizeof(cb2));
        cb2 = (uintptr_t)car_hide_a; memory_copy(car_cb[0], &cb2, sizeof(cb2));
        cb2 = (uintptr_t)car_hide_b; memory_copy(car_cb[1], &cb2, sizeof(cb2));
        cb2 = (uintptr_t)car_hide_c; memory_copy(car_cb[2], &cb2, sizeof(cb2));
    }
    {
        uintptr_t cb3;
        native_fc[0] = (CameraUpdate)(image + 0x103cda0);
        cb3 = (uintptr_t)native_fc[0]; memory_copy(fc_orig[0], &cb3, sizeof(cb3));
        cb3 = (uintptr_t)update_fc0; memory_copy(fc_cb[0], &cb3, sizeof(cb3));
        native_fc[1] = (CameraUpdate)(image + 0x103d7b0);
        cb3 = (uintptr_t)native_fc[1]; memory_copy(fc_orig[1], &cb3, sizeof(cb3));
        cb3 = (uintptr_t)update_fc1; memory_copy(fc_cb[1], &cb3, sizeof(cb3));
        native_fc[2] = (CameraUpdate)(image + 0x103d8a0);
        cb3 = (uintptr_t)native_fc[2]; memory_copy(fc_orig[2], &cb3, sizeof(cb3));
        cb3 = (uintptr_t)update_fc2; memory_copy(fc_cb[2], &cb3, sizeof(cb3));
        native_fc[3] = (CameraUpdate)(image + 0x103bf20);
        cb3 = (uintptr_t)native_fc[3]; memory_copy(fc_orig[3], &cb3, sizeof(cb3));
        cb3 = (uintptr_t)update_fc3; memory_copy(fc_cb[3], &cb3, sizeof(cb3));
        native_fc[4] = (CameraUpdate)(image + 0x103dd60);
        cb3 = (uintptr_t)native_fc[4]; memory_copy(fc_orig[4], &cb3, sizeof(cb3));
        cb3 = (uintptr_t)update_fc4; memory_copy(fc_cb[4], &cb3, sizeof(cb3));
    }
    native_pos_f = (PosGetterF)(image + 0x1034a40);
    native_pos_d = (PosGetterD)(image + 0x1034ac0);
    callback = (uintptr_t)native_pos_f;
    memory_copy(original_posf, &callback, sizeof(callback));
    callback = (uintptr_t)follow_pos_f;
    memory_copy(posf_callback, &callback, sizeof(callback));
    callback = (uintptr_t)native_pos_d;
    memory_copy(original_posd, &callback, sizeof(callback));
    callback = (uintptr_t)follow_pos_d;
    memory_copy(posd_callback, &callback, sizeof(callback));
}

void drift_camera_commit_options(const ModOptions *options) {
    (void)options;
    for (unsigned view = 0; view < ARRAY_COUNT(mounted_offsets); ++view) {
        mounted_offsets[view] = (DriftOffset){0};
        chase_cameras[view] = NULL;
        chase_slide_blend[view] = 0;
    }
}

#ifdef FH2_TWEAKS_TEST
__declspec(dllexport) void *Fh2TestDriftCameraHookAddress(void) {
    return (void *)momentum_heading;
}

__declspec(dllexport) void *Fh2TestDriftViewHookAddress(unsigned index) {
    return index ? (void *)update_mounted_camera : (void *)update_chase_heading;
}
#endif

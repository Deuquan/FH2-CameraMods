#include "camera_views.h"
#include "features.h"
#include "support.h"
#include "video_settings.h"

enum { FOV_SLOT = 0x70, NATIVE_CHASE_FOV = 0x1034120, NATIVE_MOUNTED_FOV = 0x10340f0 };

static const unsigned camera_vtables[] = {0x2e783a0, 0x2e78610, 0x2e78880,
                                          0x2e78120, 0x2e77ea8, 0x2e703b0};
typedef float (*GetFov)(void *);
static unsigned char *image;
static GetFov native_chase, native_mounted_n[3];
static unsigned char original_chase[8], orig_m[3][8], replacement[8];
static const unsigned char *const accepted_chase[] = {original_chase, replacement};
static const unsigned char *const accepted_m0[] = {orig_m[0], replacement};
static const unsigned char *const accepted_m1[] = {orig_m[1], replacement};
static const unsigned char *const accepted_m2[] = {orig_m[2], replacement};

static float get_fov_base(void *camera) {
    void *vtable;
    if (!memory_read_pointer(camera, &vtable)) {
        return 0;
    }
    unsigned view = CAMERA_UNKNOWN;
    for (unsigned index = 0; index < ARRAY_COUNT(camera_vtables); ++index) {
        if (vtable == image + camera_vtables[index]) {
            view = index + CAMERA_CHASE;
            break;
        }
    }
    if (!view) {
        return 0;
    }
    int chase = view <= CAMERA_CHASE_FAR;
    float result = chase ? native_chase(camera) : native_mounted_n[view - CAMERA_DRIVER](camera);
    float degrees = camera_fov_degrees(view);
    if (degrees == camera_fov_default(view) || !float_is_finite(result)) {
        return result;
    }
    float base;
    if (chase) {
        void *settings;
        if (!memory_read_pointer((unsigned char *)camera + 0x180, &settings) ||
            !memory_read((unsigned char *)settings + 0x1c, &base, sizeof(base))) {
            return result;
        }
        if (!float_is_finite(base) || base <= 0) {
            return result;
        }
        return result * (degrees / base);
    }
    if (!memory_read((unsigned char *)camera + 0x1d0, &base, sizeof(base)) ||
        !float_is_finite(base) || base <= 0) {
        return result;
    }
    return result * (degrees * 0.017453292519943295f / base);
}

/* Drohnen-Zoom: der Sichtwinkel, den das Spiel fuers Bild abfragt, wird hier durch den Zoom geteilt */
static float get_fov(void *camera) {
    float fov = get_fov_base(camera);
    float divisor = drone_probe_fov_divisor();
    if (divisor > 0.01f && float_is_finite(fov)) fov /= divisor;
    return fov;
}

#define FOV_PATCH(table, name, original, accepted)                                                 \
    {table + FOV_SLOT, 0,           8,           GROUP_CAMERA_FOV, name,                           \
     original,         replacement, replacement, accepted,         2}
PatchDefinition camera_fov_hook_patches[6] = {
    FOV_PATCH(0x2e783a0u, "fov_follow", original_chase, accepted_chase),
    FOV_PATCH(0x2e78610u, "fov_chase", original_chase, accepted_chase),
    FOV_PATCH(0x2e78880u, "fov_far_chase", original_chase, accepted_chase),
    FOV_PATCH(0x2e78120u, "fov_driver", orig_m[0], accepted_m0),
    FOV_PATCH(0x2e77ea8u, "fov_hood", orig_m[1], accepted_m1),
    FOV_PATCH(0x2e703b0u, "fov_bumper", orig_m[2], accepted_m2),
};

void camera_fov_prepare_hooks(unsigned char *game_image) {
    image = game_image;
    native_chase = (GetFov)(image + NATIVE_CHASE_FOV);
    uintptr_t pointer = (uintptr_t)native_chase;
    memory_copy(original_chase, &pointer, sizeof(pointer));
    {
        /* Fahrer/Haube/Stossstange: was an der Stelle schon steht (z.B. ForzaInfinity) wird dahinter weiter aufgerufen */
        static const unsigned mounted_tables[3] = {0x2e78120u, 0x2e77ea8u, 0x2e703b0u};
        for (int mi = 0; mi < 3; ++mi) {
            uintptr_t cur = 0;
            memory_copy(&cur, image + mounted_tables[mi] + FOV_SLOT, sizeof(cur));
            if (cur == 0) cur = (uintptr_t)(image + NATIVE_MOUNTED_FOV);
            memory_copy(orig_m[mi], &cur, sizeof(cur));
            native_mounted_n[mi] = (GetFov)cur;
        }
    }
    pointer = (uintptr_t)get_fov;
    memory_copy(replacement, &pointer, sizeof(pointer));
}

#ifdef FH2_TWEAKS_TEST
__declspec(dllexport) void *Fh2TestFovHookAddress(void) {
    return (void *)get_fov;
}
#endif

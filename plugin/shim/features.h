#ifndef FH2_FEATURES_H
#define FH2_FEATURES_H
#include "extra_options.h"
int drone_probe_owns_camera(void *camera);
void overlay_start(void);
int drone_probe_is_on(void);
int drone_probe_walk_mode(void);
int drone_probe_walk_level(void);
int drone_probe_focus(void);
int drone_probe_passenger(void);
int drone_probe_cinema(void);
int drift_camera_car_state(void *camera, float *hx, float *hz, float vel[3]);
int drift_camera_car_pitch(void *camera, float *fy, float *hlen);
void patch_engine_log_slots(void);
float drone_probe_fov_divisor(void);
void drone_probe_apply_roll(void *out);
void drone_probe_neutralize_look(void *camera);
void drone_probe_ego_view(int chase);
void drone_probe_ego_update(void *camera, float dt);
void drone_probe_ego_apply(void *camera, void *out);
int drone_probe_ego_active(void);
int drone_probe_follow(void);
float drone_probe_zoom(void);
unsigned drone_probe_seconds(void);
int drone_probe_car_flag(int which);
void drone_probe_reset_seen(unsigned view);
int drone_probe_override_position(void *camera, double out[3]);
void drone_probe_getter_diff(int kind, const double native[3], const double ours[3]);
void drone_probe_before_update(void *camera);
void drone_probe_director(void *director, uintptr_t image_base);
void drone_probe_note_caller(int kind, uintptr_t caller, uintptr_t image_base);
void drone_probe_frame(void *camera, unsigned view, const unsigned char *image, int after_update, float dt);
#endif

#ifndef CM_ACTIONS_H
#define CM_ACTIONS_H
/* Rebindable actions of Camera Mods. Shared by the plugin (CameraMods.dll) and the
   platform bridge (CameraModsMenu.dll). X(id, ini-name, menu label, default virtual key) */
#define CM_ACTIONS(X) \
    X(TOGGLE,     "Toggle",       "Drone mode on/off",            0x4F) /* O */ \
    X(WALK,       "Walk",         "Walk mode",                    0x50) /* P */ \
    X(PASSENGER,  "Passenger",    "Passenger mode",               0x4D) /* M */ \
    X(REAR,       "Rear360",      "360 camera behind the car",    0x58) /* X */ \
    X(DRIVERSEAT, "DriverSeat",   "Driver seat view",             0x48) /* H */ \
    X(CINEMA,     "Cinema",       "Cinema mode",                  0x4C) /* L */ \
    X(HIDEDRIVER, "HideDriver",   "Hide / show driver",           0x78) /* F9 */ \
    X(DRIVELOCK,  "DriveLock",    "Lock / allow driving",         0x4E) /* N */ \
    X(INPUTMODE,  "InputMode",    "Keyboard / controller input",  0x49) /* I */ \
    X(SIDE,       "Side",         "Passenger side",               0x43) /* C */ \
    X(SHAKE,      "Shake",        "Camera shake",                 0x55) /* U */ \
    X(FOLLOWCAR,  "FollowCar",    "Walk after the car",           0x56) /* V */ \
    X(FOCUSCAR,   "FocusCar",     "Keep car in view",             0x47) /* G */ \
    X(EGO,        "EgoCam",       "Improved cockpit camera",      0x4B) /* K */ \
    X(CHASE,      "ChaseCam",     "Improved chase camera",        0x4A) /* J */ \
    X(FIX1,       "RoofCam",      "Fixed cam: roof",              0x31) /* 1 */ \
    X(FIX2,       "DashCam",      "Fixed cam: dashcam",           0x32) /* 2 */ \
    X(FIX3,       "DashboardCam", "Fixed cam: dashboard",         0x33) /* 3 */ \
    X(FIX4,       "FrontChase",   "Fixed cam: front chase",       0x34) /* 4 */ \
    X(LOOKFWD,    "LookForward",  "Look forward again",           0x59) /* Y */ \
    X(WALKLESS,   "WalkBobLess",  "Walk bobbing less",            0x74) /* F5 */ \
    X(WALKMORE,   "WalkBobMore",  "Walk bobbing more",            0x75) /* F6 */ \
    X(EGOLESS,    "EgoLess",      "Cockpit effect less",          0x76) /* F7 */ \
    X(EGOMORE,    "EgoMore",      "Cockpit effect more",          0x77) /* F8 */ \
    X(FOLLOWDRONE,"FollowDrone",  "Follow drone on/off",          0x54) /* T */ \
    X(NEXTSHOT,   "NextShot",     "Follow drone: next shot",      0x5A) /* Z */

enum {
#define CM_X(id, name, label, vk) CM_A_##id,
    CM_ACTIONS(CM_X)
#undef CM_X
    CM_ACT_COUNT
};
static const char *const cm_action_name[CM_ACT_COUNT] = {
#define CM_X(id, name, label, vk) name,
    CM_ACTIONS(CM_X)
#undef CM_X
};
static const char *const cm_action_label[CM_ACT_COUNT] = {
#define CM_X(id, name, label, vk) label,
    CM_ACTIONS(CM_X)
#undef CM_X
};
static const unsigned cm_action_vk[CM_ACT_COUNT] = {
#define CM_X(id, name, label, vk) vk,
    CM_ACTIONS(CM_X)
#undef CM_X
};
#endif

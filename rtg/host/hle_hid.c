// Controller and touchscreen SDK bindings for the Android input backend.
#include "hle.h"
#include "../android/input_backend.h"
#include "../android/vibration_backend.h"
// This Android branch contains only SDK service bindings and the actual host
// input backend. No game-version UI pumps, scripted taps, or fixed entry calls
// from the Apple branch are compiled into it.
static void input_require(int error, const char *operation) {
    if (error) die("Android input %s failed error=%d (real host binding required)", operation, error);
}
HLE_FORCE("_ZN2nn3hid14InitializeNpadEv", "Android source binding/state initialization", void, h_hid_InitNpad, void) {
    input_require(dm_android_input_initialize_npad(), "InitializeNpad");
    LOG("ANDROID_HID InitializeNpad bound=true");
}
HLE_FORCE("_ZN2nn3hid21InitializeTouchScreenEv", "Android MotionEvent source binding/state initialization", void, h_hid_InitTouch, void) {
    input_require(dm_android_input_initialize_touch(), "InitializeTouchScreen");
    LOG("ANDROID_HID InitializeTouchScreen bound=true");
}
HLE_FORCE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", "Store/validate IDs and filter logical input connection", void, h_hid_SetIds, const uint32_t *ids, uint64_t n) {
    input_require(dm_android_input_set_ids(ids, (size_t)n), "SetSupportedNpadIdType");
}
HLE_FORCE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "Store requested styles and filter actual logical input source", void, h_hid_SetStyles, uint32_t styles) {
    input_require(dm_android_input_set_styles(styles), "SetSupportedNpadStyleSet");
}
HLE_FORCE("_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", "Store actual handheld attachment requirement", void, h_hid_SetHhMode, int mode) {
    input_require(dm_android_input_set_handheld_mode(mode), "SetNpadHandheldActivationMode");
}
HLE_FORCE("_ZN2nn3hid15GetNpadStyleSetERKj", "Return only a bound/allowed logical input style", uint32_t, h_hid_GetStyle, const uint32_t *id) {
    uint32_t style = 0; if (!id) die("GetNpadStyleSet null ID");
    input_require(dm_android_input_get_style(*id, &style), "GetNpadStyleSet"); return style;
}
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", "Sample bound Android logical actions with press-edge latch", void, h_hid_GetFullKey, dm_android_npad_state *out, const uint32_t *id) {
    if (!id) die("GetNpadState null ID");
    input_require(dm_android_input_read_npad(out, *id, 1), "GetNpadFullKeyState");
}
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", "Sample bound Android logical actions with attachment/style filters", void, h_hid_GetHandheld, dm_android_npad_state *out, const uint32_t *id) {
    if (!id) die("GetNpadState null ID");
    input_require(dm_android_input_read_npad(out, *id, 2), "GetNpadHandheldState");
}
HLE_FORCE("_ZN2nn3hid20IsVibrationPermittedEv", "Android physical vibrator availability", unsigned, h_hid_VibPermit, void) {
    return dm_android_vibration_permitted();
}
HLE_FORCE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "Connected logical pad rumble channels mapped to Android motor", int, h_hid_GetVibHandles, uint32_t *out, int capacity, const uint32_t *id, uint32_t style) {
    if (!id) die("Invalid vibration-device enumeration");
    uint32_t connected=0;input_require(dm_android_input_get_style(*id,&connected), "vibration pad style");
    int n=dm_android_vibration_handles(out,capacity,*id,style&connected);
    if(n<0)die("Invalid vibration-device enumeration error=%d",-n);
    return n;
}
HLE_FORCE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", "Initialize real Android rumble route", void, h_hid_InitVib, const uint32_t *handle) {
    if(!handle||dm_android_vibration_initialize(*handle))LOG("WARN invalid vibration initialization");
}
HLE_FORCE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", "Queue game rumble without Android Binder work on game thread", void, h_hid_SendVib, const uint32_t *handle, const dm_vibration_value *value) {
    if(!handle||dm_android_vibration_send(*handle,value))LOG("WARN invalid vibration value/handle");
}
HLE_FORCE("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", "Read real Android multi-pointer input with Start/End latch", int, h_hid_GetTouch, void *out, int capacity) {
    int received = dm_android_input_read_touch(out, capacity);
    if (received < 0) input_require(-received, "GetTouchScreenStates");
    return received;
}

// These bindings were shared by both platform branches before the Android-only
// source cleanup. Keep them here: otherwise the original SDK reaches Horizon
// HID IPC, including SetNpadJoyHoldType during the game's startup.
HLE_FORCE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", "Fixed host controller orientation", void, h_hid_SetJoyHold, int type) { (void)type; }
HLE_FORCE("_ZN2nn3hid14DisconnectNpadERKj", "Logical touch controller remains attached", void, h_hid_Disconnect, const uint32_t *id) { (void)id; }
HLE_FORCE("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj", "Default logical controller color", uint64_t, h_hid_Color, void *out, const uint32_t *id) {
    (void)id; if (out) memset(out, 0, 16); return 0;
}
HLE_FORCE("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj", "Default dual controller colors", uint64_t, h_hid_Color2, void *out, void *out2, const uint32_t *id) {
    (void)id; if (out) memset(out, 0, 16); if (out2) memset(out2, 0, 16); return 0;
}
HLE_FORCE("_ZN2nn3hid20ControllerSupportArg10SetDefaultEv", "Initialize controller support arguments", void, h_hid_CtrlArgDflt, void *self) {
    if (self) memset(self, 0, 0x48);
}
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", "Single logical touch controller support", uint64_t, h_hid_ShowCtrlSupport, void *out, const void *arg) {
    (void)arg;
    if (out) { memset(out, 0, 0x14); *(uint8_t *)out = 1; }
    return 0;
}
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", "Single logical touch controller support", void, h_hid_ShowCtrlSupport2, const void *arg) { (void)arg; }

static int android_states_fill(dm_android_npad_state *states, int count, const uint32_t *id, uint32_t style) {
    if (!id || count <= 0) return 0;
    if (!states) die("GetNpadStates null output");
    for (int i = 0; i < count; i++)
        input_require(dm_android_input_read_npad(&states[i], *id, style), "GetNpadStates");
    return count;
}
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", "Batch samples from Android FullKey input", int, h_hid_StatesFullKey, dm_android_npad_state *states, int count, const uint32_t *id) {
    return android_states_fill(states, count, id, 1);
}
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", "Batch samples from Android Handheld input", int, h_hid_StatesHandheld, dm_android_npad_state *states, int count, const uint32_t *id) {
    return android_states_fill(states, count, id, 2);
}

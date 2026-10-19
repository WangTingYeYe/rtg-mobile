#include "vibration_backend.h"
#include <jni.h>
JNIEXPORT jlong JNICALL Java_org_rhythmtengoku_mobile_GameHaptics_nativeBind(JNIEnv *env,jclass cls,jboolean available) {
    (void)env;(void)cls;return (jlong)dm_android_vibration_bind(available);
}
JNIEXPORT jlong JNICALL Java_org_rhythmtengoku_mobile_GameHaptics_nativeActive(JNIEnv *env,jclass cls,jlong session,jboolean active) {
    (void)env;(void)cls;return dm_android_vibration_active((uint64_t)session,active);
}
JNIEXPORT jlong JNICALL Java_org_rhythmtengoku_mobile_GameHaptics_nativeWait(JNIEnv *env,jclass cls,jlong session) {
    (void)env;(void)cls;return dm_android_vibration_wait((uint64_t)session);
}
JNIEXPORT void JNICALL Java_org_rhythmtengoku_mobile_GameHaptics_nativeClose(JNIEnv *env,jclass cls,jlong session) {
    (void)env;(void)cls;dm_android_vibration_unbind((uint64_t)session);
}

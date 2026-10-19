// 节奏天国移植：Mac 侧触控/诊断符号的安卓桩。
// Mac 版这些实现在 hle_hid.c 的 Apple 分支 / front_mac.m；安卓输入走 input_backend.c
// （dm_android_input_*），这里的桩只保证链接与安全缺省行为，后续真机联调时换成
// 安卓触控路由。
// RTG port: Android stubs for the Mac-side touch / diagnostics symbols. On macOS these live
// in hle_hid.c's Apple branch / front_mac.m; Android input goes through input_backend.c
// (dm_android_input_*). These stubs only keep the link whole and behave safely; swap in the
// real Android touch routing during device bring-up.
#include "front.h"
#include "dm_input.h"
#include <string.h>
#include <stddef.h>

// 触点集合：安卓暂走空实现（真机阶段接 input_backend 的触控路由）
void dm_touch_set(const dm_touch *t, int n) { (void)t; (void)n; }
void dm_touch_cancel(void) {}

// hle_hid 的触屏状态填充（Mac 实现在 Apple 分支）：安卓先报「无触点」
int touch_fill(void *out, uint64_t now_ns) { memset(out, 0, 0x38); (void)now_ns; return 0; }

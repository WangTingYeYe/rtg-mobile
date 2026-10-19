#ifdef __ANDROID__
#include "../android/audio_renderer_hle.inc"
#else
// 节奏天国移植新增：REV AudioRenderer 全套空桩（游戏导入 37 个 nn::audio 符号）。
// 目标是引导通过：渲染器「打开成功、永不更新」——音频线程等事件空转，无声。
// 真实实现（CoreAudio/AAudio 后端的 REV 仿真）是独立里程碑。
// Added for this port: no-op REV AudioRenderer stubs (boot-through only; silent).
// A real implementation (REV emulation on CoreAudio/AAudio) is a separate milestone.
#include "hle.h"
#include <string.h>
#include <stdatomic.h>
#include <stddef.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

static void ar_count(const char *f) { static _Atomic int n; if ((atomic_fetch_add(&n, 1) + 1) % 300 == 1) LOG("[audio] %s 第 %d 次", f, n); }

static uint64_t now_ns_(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

// per-voice 假播放状态：入队时刻 + 波形总样本数（WaveBuffer[2] 实测为样本数）
static struct { const void *v; uint64_t t0, samples; uint32_t rate; int have; } g_v[8];
static int vidx(const void *v) { for (int i = 0; i < 8; i++) if (g_v[i].have && g_v[i].v == v) return i; for (int i = 0; i < 8; i++) if (!g_v[i].have) { g_v[i].v = v; g_v[i].have = 1; return i; } return -1; }
static void voice_note(void *voice, const void *wb) {
    if (!wb) return;
    int i = vidx(voice); if (i < 0) return;
    g_v[i].t0 = now_ns_(); g_v[i].samples = ((const uint64_t *)wb)[1] / 2;   // fmt=2：16-bit 单声道（实测 AcquireVoiceSlot rate=32000 fmt=2 b=1）
    if (!g_v[i].rate) g_v[i].rate = 32000;
}


void hle_bind_system_event(void *ev, int autoclear);   // hle_thread.c：在客体线程上调 SDK 造内核事件 | hle_thread.c: creates a kernel event via the SDK, must run on a guest thread
void hle_signal_system_event(void *ev);                // 任意线程可调 | callable from any thread

// REV 更新事件：桩渲染器每 5 ms 触发一次（真机节拍）。游戏的音频线程等它醒来调 RequestUpdate；
// 从不触发的话，等音频就绪的装载流水线会永远停着。
// REV update event: the stub renderer fires it every 5 ms (the real cadence). The game's audio thread
// waits on it to call RequestUpdate; without it, any load step that waits for audio readiness stalls forever.
static void *ar_tick(void *p) {
    (void)p;
    for (;;) {
        usleep(5000);
        void hle_signal_system_event(void *);
        hle_signal_system_event(p);
    }
    return NULL;
}

// GetAudioRendererWorkBufferSize：保留桩（固定值）——sdk 原生实现会连 audren:u 服务（QueryWorkBufferSize
// 的 hipc），我们无服务端，连接失败直接 abort（Break 栈 sdk+0x22c020 "audren:u"）
// Keep this one stubbed: the native implementation queries the audren:u service over hipc
HLE_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "REV 工作区大小；给固定值（原生版要连服务）", size_t, h_ar_WorkBufSz, const void *p) { (void)p; return 0x100000; }
// GetAudioRendererConfigWorkBufferSize：原生实现（0x21ea60）是纯计算（各 Manager 尺寸的求和），原生跑
// The config variant (0x21ea60) is pure arithmetic; run it natively
HLE_FORCE("_ZN2nn5audio36GetAudioRendererConfigWorkBufferSizeERKNS0_22AudioRendererParameterE", "同上（config 变体）", size_t, h_ar_CfgBufSz, const void *p) { (void)p; return 0x100000; }
// InitializeAudioRendererParameter / GetAudioRenderer*WorkBufferSize：不再接管——sdk 原生实现
// （0x21ea00 / 0x227910 / 0x21ea60）都是纯内存计算。原先空桩把参数留成全零，
// 原生跑的 InitializeAudioRendererConfig 分配失败直接 abort（Break#1 reason=0 sdk+0x21efc4）。
// These three are no longer intercepted: the native implementations are pure memory computation.
// The old no-op stubs left the parameters all-zero, which made the native config init abort.
// 3 参版 Open（无事件输出）：真实现会连 audren:u 服务（我们无 hipc 服务端，连接失败 abort）。
// 与 5 参版同口径：直接给假 handle（{1,0} 16 字节）——cfg/work 由原生 InitializeAudioRendererConfig 建好
// The 3-arg Open overload (no event out): the real one connects to audren:u (no hipc server here, aborts).
// Same treatment as the 5-arg version: fabricate the handle; cfg/work are built by the native config init
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm", "REV 3 参 Open；假 handle 不连服务", uint64_t, h_ar_Open3, void *handle, const void *param, void *work, size_t wsz) {
    (void)param; (void)work; (void)wsz;
    uint64_t *h = handle; if (h) { h[0] = 1; h[1] = 0; }
    return 0;
}
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "打开 REV；桩句柄 1、成功，更新事件按 5 ms 节拍触发", uint64_t, h_ar_Open, uint64_t *handle, void *event, const void *param, void *work, size_t wsz) {
    (void)param; (void)work; (void)wsz;
    if (handle) *handle = 1;
    if (event) {
        hle_bind_system_event(event, 1);   // 自动清除：每次 Wait 醒来即清，跟真 REV 一致 | auto-clear: matches the real REV
        pthread_t th; if (pthread_create(&th, NULL, ar_tick, event)) die("REV 节拍线程起不来");
        pthread_detach(th);
        LOG("[audio] REV 桩：更新事件已绑、5ms 节拍线程已起");
    }
    return 1;
}
// InitializeAudioRendererConfig：不再接管——sdk 原生实现（0x21eb80）纯内存（WorkBufferAllocator +
// 各 Manager 构造），原生跑即正确建好 cfg 各槽；我们的空桩曾让它全零，RequestUpdate 无从下手
// InitializeAudioRendererConfig: no longer intercepted -- the native sdk implementation (0x21eb80) is pure
// memory work (WorkBufferAllocator + Manager constructors) and builds cfg correctly

HLE_FORCE("_ZN2nn5audio15AcquireFinalMixEPNS0_19AudioRendererConfigEPNS0_12FinalMixTypeEi", "REV；空桩成功", uint64_t, h_ar_AcqFinal, void *cfg, void *mix, int n) { (void)cfg; (void)mix; (void)n; return 1; }
HLE_FORCE("_ZN2nn5audio13AcquireSubMixEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeEii", "REV；空桩成功", uint64_t, h_ar_AcqSub, void *cfg, void *mix, int a, int b) { (void)cfg; (void)mix; (void)a; (void)b; return 1; }
HLE_FORCE("_ZN2nn5audio20SetSubMixDestinationEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeEPNS0_12FinalMixTypeE", "REV；空桩", void, h_ar_SubDest, void *cfg, void *sub, void *fin) { (void)cfg; (void)sub; (void)fin; }
HLE_FORCE("_ZN2nn5audio18SetSubMixMixVolumeEPNS0_10SubMixTypeEPNS0_12FinalMixTypeEfii", "REV；空桩", void, h_ar_SubVol, void *sub, void *fin, float v, int a, int b) { (void)sub; (void)fin; (void)v; (void)a; (void)b; }
HLE_FORCE("_ZN2nn5audio35GetRequiredBufferSizeForI3dl2ReverbEii", "REV；给 0", size_t, h_ar_RevSz, int a, int b) { (void)a; (void)b; return 0; }
HLE_FORCE("_ZN2nn5audio17AcquireMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeEPvm", "REV；空桩成功", uint64_t, h_ar_AcqPool, void *cfg, void *pool, void *mem, size_t sz) { (void)cfg; (void)pool; (void)mem; (void)sz; return 1; }
HLE_FORCE("_ZN2nn5audio23RequestAttachMemoryPoolEPNS0_14MemoryPoolTypeE", "REV；空桩", void, h_ar_AttachPool, void *pool) { (void)pool; }
HLE_FORCE("_ZN2nn5audio14AddI3dl2ReverbEPNS0_19AudioRendererConfigEPNS0_15I3dl2ReverbTypeEPvmPNS0_10SubMixTypeEi", "REV；空桩成功", uint64_t, h_ar_AddRev, void *cfg, void *rev, void *mem, size_t sz, void *sub, int n) { (void)cfg; (void)rev; (void)mem; (void)sz; (void)sub; (void)n; return 1; }
HLE_FORCE("_ZN2nn5audio25SetI3dl2ReverbInputOutputEPNS0_15I3dl2ReverbTypeEPKaS4_i", "REV；空桩", void, h_ar_RevIO, void *rev, const void *in, const void *out, int n) { (void)rev; (void)in; (void)out; (void)n; }
HLE_FORCE("_ZN2nn5audio21SetI3dl2ReverbEnabledEPNS0_15I3dl2ReverbTypeEb", "REV；空桩", void, h_ar_RevOn, void *rev, unsigned on) { (void)rev; (void)on; }
HLE_FORCE("_ZN2nn5audio21LoadI3dl2ReverbPresetEPNS0_23I3dl2ReverbParameterSetENS0_15I3dl2ReverbType6PresetE", "REV；空桩", void, h_ar_RevPreset, void *out, int preset) { (void)out; (void)preset; }
HLE_FORCE("_ZN2nn5audio24SetI3dl2ReverbParametersEPNS0_15I3dl2ReverbTypeEPKNS0_23I3dl2ReverbParameterSetE", "REV；空桩", void, h_ar_RevParam, void *rev, const void *set) { (void)rev; (void)set; }
HLE_FORCE("_ZN2nn5audio13AddDeviceSinkEPNS0_19AudioRendererConfigEPNS0_14DeviceSinkTypeEPNS0_12FinalMixTypeEPKaiPKc", "REV；空桩成功", uint64_t, h_ar_AddSink, void *cfg, void *sink, void *fin, const int *ids, int n, const char *dev) { (void)cfg; (void)sink; (void)fin; (void)ids; (void)n; (void)dev; return 1; }
// REV 更新（真实协议，逆向自 sdk+0x227f80 RequestUpdateAudioRenderer）：
//   cfg 布局：+0x30 BehaviorManager、+0x28 MemoryPoolManager、+0x00 VoiceInfoManager、+0x10 EffectManager、
//   +0x18 SinkManager、+0x20 (Splitter/Mix/Performance)、+0x40 行为结果对象指针、+0x48 请求头(0x40B)、
//   +0x50 缓冲总大小、+0x58 结果头(0x40B) 指针
//   真实现把请求段序列化→hipc→服务端写结果→UpdateOutStatus 系列把结果解析进各 Manager 对象。
//   HLE：结果头拷请求头（各段大小一致，过 0x228150 起的逐段校验），结果数据按假播放时钟伪造，
//   然后调 sdk 原生的 6 个 UpdateOutStatus（PLT 已重定位、按名 guest_sym）回写游戏对象。
// REV update (real protocol, reverse-engineered from sdk+0x227f80):
//   HLE: copy the request header to the result header (segment sizes match, passing the per-segment checks),
//   fabricate the result data from the fake-play clock, then call the six native UpdateOutStatus entry points
//   (relocated PLT, resolved by name via guest_sym) to write the results back into the game objects.
HLE_FORCE("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE", "REV 更新；sdk 原生序列化 + 结果伪造 + 原生 UpdateOutStatus 回写", uint64_t, h_ar_Update, uint64_t h0, uint64_t h1, const void *cfg) {
    (void)h0; (void)h1;
    static int inited = -1;
    // 请求序列化器（sdk 原生导出；调用序与符号抄自 sdk+0x227f80 的 0x227ff4-0x228094 段）
    static uint32_t (*p_get_rev)(void *);
    static uint32_t (*p_in_behavior)(void *, void *), (*p_in_pool)(void *, void *), (*p_in_voice)(void *, void *, uint64_t *, uint64_t *),
                   (*p_in_effect)(void *, void *), (*p_in_splitter)(void *, void *), (*p_in_mix)(void *, void *), (*p_in_sink)(void *, void *), (*p_in_perf)(void *, void *);
    // 结果解析器
    static void (*p_pool_out)(void *, const void *), (*p_voice_out)(void *, const void *), (*p_effect_out)(void *, const void *), (*p_sink_out)(void *, const void *), (*p_perf_out)(void *, const void *), (*p_behavior_out)(void *, const void *);
    if (inited < 0) {
        inited = 1;
        p_get_rev = guest_sym("_ZNK2nn5audio15BehaviorManager11GetRevisionEv");
        p_in_behavior = guest_sym("_ZNK2nn5audio15BehaviorManager17UpdateInParameterEPNS0_8renderer3ipc8Behavior10UpdateDataILi0EEE");   // 注意 K（const 成员）——名字不带 K 时 guest_sym 解析不到、调用即崩 | note the K (const member): without it guest_sym resolves nothing and the call crashes
        p_in_pool = guest_sym("_ZN2nn5audio17MemoryPoolManager17UpdateInParameterEPNS0_8renderer3ipc10MemoryPool10UpdateDataILi0EEE");
        p_in_voice = guest_sym("_ZN2nn5audio16VoiceInfoManager28UpdateVoiceRelatedParametersEPvPmS3_");
        p_in_effect = guest_sym("_ZN2nn5audio13EffectManager24UpdateEffectsInParameterEPNS0_8renderer3ipc6Effect10UpdateDataILi2EEE");
        p_in_splitter = guest_sym("_ZN2nn5audio19SplitterInfoManager17UpdateInParameterEPv");
        p_in_mix = guest_sym("_ZN2nn5audio10MixManager20UpdateMixInParameterEPv");
        p_in_sink = guest_sym("_ZN2nn5audio11SinkManager21UpdateSinkInParameterEPNS0_8renderer3ipc4Sink10UpdateDataILi0EEE");
        p_in_perf = guest_sym("_ZN2nn5audio24PerformanceBufferManager17UpdateInParameterEPNS0_8renderer3ipc18PerformanceMetrics10UpdateDataILi0EEE");
        p_pool_out = guest_sym("_ZN2nn5audio17MemoryPoolManager15UpdateOutStatusEPKNS0_8renderer3ipc10MemoryPool10StatusDataILi0EEE");
        p_voice_out = guest_sym("_ZN2nn5audio16VoiceInfoManager20UpdateVoiceOutStatusEPKNS0_8renderer3ipc5Voice10StatusDataILi0EEE");
        p_effect_out = guest_sym("_ZN2nn5audio13EffectManager21UpdateEffectOutStatusEPKNS0_8renderer3ipc6Effect10StatusDataILi1EEE");
        p_sink_out = guest_sym("_ZN2nn5audio11SinkManager19UpdateSinkOutStatusEPKNS0_8renderer3ipc4Sink10StatusDataILi0EEE");
        p_perf_out = guest_sym("_ZN2nn5audio24PerformanceBufferManager15UpdateOutStatusEPKNS0_8renderer3ipc18PerformanceMetrics10StatusDataILi0EEE");
        p_behavior_out = guest_sym("_ZN2nn5audio15BehaviorManager15UpdateOutStatusEPKNS0_8renderer3ipc8Behavior10StatusDataILi0EEE");
        LOG("[audio] REV 序列化+回写就绪：in_voice=%p out_voice=%p", p_in_voice, p_voice_out);
    }
    // cfg 布局（sdk+0x227f80 逆向）：+0x00 VoiceInfoManager +0x08 MixManager +0x10 EffectManager +0x18 SinkManager
    // +0x20 PerformanceBufferManager +0x28 MemoryPoolManager +0x30 BehaviorManager +0x38 SplitterInfoManager
    // +0x40 行为结果对象 +0x48 请求头 +0x50 缓冲大小 +0x58 结果头
    const uint64_t *c = cfg;
    { static int nd; if (nd++ < 4) { char b[500]; int q = 0; for (int i = 0; i < 12; i++) q += snprintf(b + q, sizeof b - q, "[%#x]=%#llx ", i * 8, (unsigned long long)c[i]); LOG("[audio] cfg dump %s", b); } }
    uint8_t *reqh = (uint8_t *)c[9], *resh = (uint8_t *)c[11];
    if (!resh || !reqh || !c[6] || !c[0]) return 0;
    // —— 请求序列化（sdk 原生，照 0x227fbc-0x228094 的顺序）——
    int st = getenv("DM_REV_STAGES") ? atoi(getenv("DM_REV_STAGES")) : 99;   // 诊断：执行到第几步（1=仅GetRevision 2=+behavior 3=+pool 4=+voice 5=+effect 6=+splitter 7=+mix 8=+sink 9=+perf 10=结果+回写）
    uint32_t rev = (st >= 1 && p_get_rev) ? p_get_rev((void *)c[6]) : 0;
    static int nstep; nstep++; if (nstep < 2 || nstep % 600 == 0) LOG("[audio] REV 步进 #%d st=%d rev=%u", nstep, st, rev);
    memset(reqh, 0, 0x40);
    *(uint32_t *)reqh = rev;
    uint8_t *rd = reqh + 0x40;   // 请求数据区 | request data area
    // 未建的 Manager（槽 NULL，如 splitter 数为 0）跳过序列化，段大小留 0 | A manager never built (NULL slot) is skipped; its segment size stays 0
    *(uint32_t *)(reqh + 4)  = (st >= 2 && c[6]) ? p_in_behavior((void *)c[6], rd) : 0;
    *(uint32_t *)(reqh + 8)  = (st >= 3 && c[5]) ? p_in_pool((void *)c[5], rd + *(uint32_t *)(reqh + 4)) : 0;
    uint64_t v1 = 0, v2 = 0;
    uint32_t o = *(uint32_t *)(reqh + 4) + *(uint32_t *)(reqh + 8);
    if (st >= 4 && c[0]) p_in_voice((void *)c[0], rd + o, &v1, &v2);
    *(uint32_t *)(reqh + 0xc) = (uint32_t)v1; *(uint32_t *)(reqh + 0x10) = (uint32_t)v2;
    o += (uint32_t)v1 + (uint32_t)v2;
    *(uint32_t *)(reqh + 0x14) = (st >= 5 && c[2]) ? p_in_effect((void *)c[2], rd + o) : 0;
    *(uint32_t *)(reqh + 0x24) = (st >= 6 && c[7]) ? p_in_splitter((void *)c[7], rd + o + *(uint32_t *)(reqh + 0x14)) : 0;
    *(uint32_t *)(reqh + 0x18) = (st >= 7 && c[1]) ? p_in_mix((void *)c[1], rd + o + *(uint32_t *)(reqh + 0x14) + *(uint32_t *)(reqh + 0x24)) : 0;
    *(uint32_t *)(reqh + 0x1c) = (st >= 8 && c[3]) ? p_in_sink((void *)c[3], rd + o + *(uint32_t *)(reqh + 0x14) + *(uint32_t *)(reqh + 0x24) + *(uint32_t *)(reqh + 0x18)) : 0;
    *(uint32_t *)(reqh + 0x20) = (st >= 9 && c[4]) ? p_in_perf((void *)c[4], rd + o + *(uint32_t *)(reqh + 0x14) + *(uint32_t *)(reqh + 0x24) + *(uint32_t *)(reqh + 0x18) + *(uint32_t *)(reqh + 0x1c)) : 0;
    // —— 结果伪造（结果头=请求头段大小；结果区按解析序 pool→voice→effect→sink→perf→behavior）——
    memcpy(resh, reqh, 0x40);
    uint8_t *d = resh + 0x40;
    uint32_t s_pool = *(uint32_t *)(resh + 8), s_voice = *(uint32_t *)(resh + 0xc), s_effect = *(uint32_t *)(resh + 0x14),
             s_sink = *(uint32_t *)(resh + 0x1c), s_perf = *(uint32_t *)(resh + 0x20);
    if (c[10] && (uint64_t)(s_pool + s_voice + s_effect + s_sink + s_perf) + 0x48 > c[10]) { LOG("【红】REV 结果区超界 %#x > %#llx", s_pool + s_voice + s_effect + s_sink + s_perf, (unsigned long long)c[10]); return 0x5299; }
    memset(d, 0, (size_t)(s_pool + s_voice + s_effect + s_sink + s_perf) + 8);
    // voice 段：每 voice 0x10B{+0x00 played(u64)、+0x08 状态(u32)、+0x0c 状态字节}（sdk 0x22bc60：x10=x20+8、ldur x12,[x10,#-8]）
    { static int nz; if (nz++ < 6) LOG("[audio] REV voice 段：s=%#x nvoice=%d", s_voice, *(const int32_t *)c[0]); }
    if (s_voice) {
        int nvoice = *(const int32_t *)c[0];
        const uint8_t *vbase = *(const uint8_t **)(c[0] + 8);
        if (nvoice > 0 && nvoice < 64 && s_voice >= (uint32_t)nvoice * 0x10) {
            for (int i = 0; i < nvoice; i++) {
                uint8_t *rec = d + s_pool + (size_t)i * 0x10;
                uint64_t played = 0, totsamples = 0;
                for (int k = 0; k < 8; k++) if (g_v[k].have && g_v[k].v == (const void *)(vbase + (size_t)i * 0x208)) {
                    uint64_t el = now_ns_() - g_v[k].t0;
                    played = (uint64_t)(el / 1000000000.0 * (g_v[k].rate ? g_v[k].rate : 32000));
                    totsamples = g_v[k].samples;
                    if (g_v[k].samples && played > g_v[k].samples) played = g_v[k].samples;
                }
                // DM_VSTATE 模式：0=完成态（played 封顶）；1=缺省播放态；2=played 超大（阈值比较实验）
                { static int vs = -1; if (vs < 0) { const char *e = getenv("DM_VSTATE"); vs = e ? atoi(e) : 1; }
                  if (vs == 2) played = 0x7fffffff00000000ull;
                  if (vs == 0) played = totsamples ? totsamples : 0x10000000ull;
                }
                *(uint64_t *)rec = played;
                // 状态字节：真机 audren 的"播放中"=1、"完成/空闲"=0
                *(uint32_t *)(rec + 8) = 1;
                { static int vs = -1; if (vs < 0) { const char *e = getenv("DM_VSTATE"); vs = e ? atoi(e) : 1; } rec[0xc] = vs ? 1 : 0; }
            }
        }
    }
    // —— 结果解析（sdk 原生 UpdateOutStatus，照 0x228144-0x2281e8 的顺序与偏移）——
    if (st >= 10 && c[5]) p_pool_out((void *)c[5], d);
    if (c[0]) p_voice_out((void *)c[0], d + s_pool);
    if (c[2]) p_effect_out((void *)c[2], d + s_pool + s_voice);
    if (c[3]) p_sink_out((void *)c[3], d + s_pool + s_voice + s_effect);
    if (c[4]) p_perf_out((void *)c[4], d + s_pool + s_voice + s_effect + s_sink);
    // sdk+2284e0/228580: Behavior StatusData contains renderer errors, not timeline events.
    // A fabricated {result=1,count=1} makes CheckErrors return 1. Supply no errors;
    // keep this fixed-size status separate from the response area's legacy size calculation.
    // Revision (this+8) is a constructor/protocol parameter, not a frame counter.
    uint8_t behavior_status[0xb0] = {0};
    if (c[6]) p_behavior_out((void *)c[6], behavior_status);
    void *behavior_obj = (void *)c[8];
    if (behavior_obj) *(uint64_t *)behavior_obj = 0;   // 0x2281ec：行为结果=behavior 段首 8 字节 | behavior result = first 8 bytes of the behavior segment
    return 0;
}

HLE_FORCE("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE", "REV；空桩成功", uint64_t, h_ar_Start, uint64_t handle) { (void)handle; return 1; }
HLE_FORCE("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE", "REV；空桩成功", uint64_t, h_ar_Stop, uint64_t handle) { (void)handle; return 1; }
HLE_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "REV；空桩成功", uint64_t, h_ar_Close, uint64_t handle) { (void)handle; return 1; }
HLE_FORCE("_ZN2nn5audio23RequestDetachMemoryPoolEPNS0_14MemoryPoolTypeE", "REV；空桩", void, h_ar_DetachPool, void *pool) { (void)pool; }
HLE_FORCE("_ZN2nn5audio20IsMemoryPoolAttachedEPKNS0_14MemoryPoolTypeE", "REV；回报已挂", unsigned, h_ar_PoolOn, const void *pool) { (void)pool; return 1; }
HLE_FORCE("_ZN2nn5audio16AcquireVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEiiNS0_12SampleFormatEiPKvm", "REV；空桩成功", uint64_t, h_ar_AcqVoice, void *cfg, void *voice, int a, int b, int fmt, int c, const void *p, size_t sz) { LOG("[audio] AcquireVoiceSlot fmt=%d rate=%d ch=%d c=%d", fmt, a, b, c);
    { int i = vidx(voice); if (i >= 0) g_v[i].rate = (uint32_t)a; } (void)cfg; (void)voice; (void)a; (void)b; (void)fmt; (void)c; (void)p; (void)sz; return 1; }
HLE_FORCE("_ZN2nn5audio19SetVoiceDestinationEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEPNS0_10SubMixTypeE", "REV；空桩", void, h_ar_VoiceDest, void *cfg, void *voice, void *sub) { (void)cfg; (void)voice; (void)sub; }
HLE_FORCE("_ZN2nn5audio17SetVoiceMixVolumeEPNS0_9VoiceTypeEPNS0_10SubMixTypeEfii", "REV；空桩", void, h_ar_VoiceVol, void *voice, void *sub, float v, int a, int b) { (void)voice; (void)sub; (void)v; (void)a; (void)b; }
HLE_FORCE("_ZN2nn5audio19SetVoiceDestinationEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEPNS0_12FinalMixTypeE", "REV；空桩", void, h_ar_VoiceDestF, void *cfg, void *voice, void *fin) { (void)cfg; (void)voice; (void)fin; }
HLE_FORCE("_ZN2nn5audio17SetVoiceMixVolumeEPNS0_9VoiceTypeEPNS0_12FinalMixTypeEfii", "REV；空桩", void, h_ar_VoiceVolF, void *voice, void *fin, float v, int a, int b) { (void)voice; (void)fin; (void)v; (void)a; (void)b; }
HLE_FORCE("_ZN2nn5audio17SetVoicePlayStateEPNS0_9VoiceTypeENS1_9PlayStateE", "REV；空桩", void, h_ar_VoicePlay, void *voice, int st) { (void)voice; (void)st; }
HLE_FORCE("_ZN2nn5audio16AppendWaveBufferEPNS0_9VoiceTypeEPKNS0_10WaveBufferE", "REV；记录入队（假播放时钟用）", void, h_ar_AppendWB, void *voice, const void *wb) {
    const uint64_t *w = (const uint64_t *)wb;
    if (wb) LOG("[audio] AppendWaveBuffer voice=%p wb=%p：%#llx %#llx %#llx %#llx", voice, wb, (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3]);
    voice_note(voice, wb);
}
HLE_FORCE("_ZN2nn5audio16ReleaseVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeE", "REV；空桩", void, h_ar_RelVoice, void *cfg, void *voice) { (void)cfg; (void)voice; }
HLE_FORCE("_ZN2nn5audio13SetVoicePitchEPNS0_9VoiceTypeEf", "REV；空桩", void, h_ar_VoicePitch, void *voice, float p) { (void)voice; (void)p; }
HLE_FORCE("_ZN2nn5audio25GetVoicePlayedSampleCountEPKNS0_9VoiceTypeE", "REV；per-voice 假播放：入队起按该声道采样率走、封顶在波形总样本数", uint64_t, h_ar_PlayedN, const void *voice) {
    for (int i = 0; i < 8; i++) if (g_v[i].have && g_v[i].v == voice) {
        { static int zap = -1; if (zap < 0) zap = !!getenv("DM_TTS_ZAP"); if (zap) return g_v[i].samples; }   // 诊断开关：旁白视为立即播完 | Diagnostic switch: treat narration as instantly finished
        uint64_t el = now_ns_() - g_v[i].t0;
        uint64_t p = (uint64_t)(el / 1000000000.0 * (g_v[i].rate ? g_v[i].rate : 32000));
        return g_v[i].samples && p > g_v[i].samples ? g_v[i].samples : p;
    }
    return 0;
}

HLE_FORCE("_ZN2nn5audio18GetWaveBufferCountEPKNS0_9VoiceTypeE", "REV；波形播完前 1、播完 0（游戏等它归零推进状态机）", int, h_ar_WBCount, const void *voice) {
    for (int i = 0; i < 8; i++) if (g_v[i].have && g_v[i].v == voice) {
        { static int zap = -1; if (zap < 0) zap = !!getenv("DM_TTS_ZAP"); if (zap) return 0; }
        uint64_t el = now_ns_() - g_v[i].t0;
        uint64_t p = (uint64_t)(el / 1000000000.0 * (g_v[i].rate ? g_v[i].rate : 32000));
        return p < g_v[i].samples;
    }
    return 0;
}
HLE_FORCE("_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi", "设备名列表；0 台（桩无输出设备）", int, h_ar_ListDev, void *names, int n) { (void)names; (void)n; return 0; }
HLE_FORCE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", "设备音量；空桩", void, h_ar_SetVol, const void *name, float v) { (void)name; (void)v; }

#endif

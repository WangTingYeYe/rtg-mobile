// 引导：装模块后，照 SDK 里 nn::init::Start（sdk+0x2b0470）的真实顺序初始化 SDK 运行时，再进 nnMain。
// Bootstrap: after loading the modules, initialize the SDK runtime in the real order of nn::init::Start (sdk+0x2b0470) in the SDK, then enter nnMain.
#include "dm.h"
#include "front.h"
#include "dm_input.h"
#include <sys/mman.h>
#include <errno.h>
#include <stdatomic.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
void probe_install(void); void ticks_install(void); void hid_keys_init(void); void stubs_report(void); void crash_install(void); void guest_thread_init(void); uint32_t svc_thread_main_init(void);
typedef void (*fn0)(void); typedef void (*fn2)(uint64_t, uint64_t); typedef void (*fnp)(void *);
static void *req(const char *n){ void *p=guest_sym(n); if(!p) die("缺符号 %s", n); return p; }

// 游戏参数：argv[0] 固定为 "D2R"，"--" 之后的原样追加（真机由 nn::init 从进程参数区取，经 SetHostArgc/Argv 交给 SDK）
// Game arguments: argv[0] is fixed to "D2R", and whatever follows "--" is appended as is (on a real device nn::init takes them from the process argument area and hands them to the SDK through SetHostArgc/Argv)
static char *g_argv[64] __attribute__((unused)) = {"D2R"}; static int g_argc __attribute__((unused)) = 1;

void crash_sample_self(void);
static int rtg_observation_logs(void) {
#ifdef __ANDROID__
    return getenv("DM_RUNTIME_DIAGNOSTICS")!=NULL;
#else
    return 1;
#endif
}

// 节奏天国场景切换请求观测门（patch_nso OBS 表 23 号，main+0x509570 挂起请求 setter）：
// 每次有人往场景管理器写 (场景号, 描述) 就打一行——教程期→黑屏期的真实请求序列（DM_SCENEREQ=<文件> 可落盘）
// RTG scene request observation gate (patch_nso OBS entry 23, the pending-request setter at main+0x509570)
#define RTG_OBS(fn, tag) static void fn(const uint64_t *r) { \
    static int n; if (rtg_observation_logs() && (++n <= 3 || n % 3000 == 0)) fprintf(stderr, "[dm] [rtg-obs] " tag " #%d 场景=%#llx\n", n, (unsigned long long)r[0]); }
RTG_OBS(obs_transreq, "转场请求")   // 0x131f50 每帧转场请求频率
#undef RTG_OBS
// 活动门卫（0x131060）细分版：同时打印 [x0+0x9400] 活动位与 [x0+0x94d0] 活动对象——门卫判定的完整生命周期
// Activity gate (0x131060) detailed: also print [x0+0x9400] enable bit and [x0+0x94d0] activity object
uint64_t g_ag_confirm_q;   // DM_AUTOGAME:待执行的菜单确认(selector Q 地址);由 obs_gate6 在客体线程消费
uint64_t g_ag_confirm_base;
void obs_gate6(const uint64_t *r) {
    static int n;
    // DM_AUTOGAME 菜单确认:门回调运行在客体线程(与 lldb 注入同上下文)——直调
    // 135fe0(Q,1)(selector「确认单人游玩」)。蹦板线程/按键注入的线程上下文不足,
    // 这里与 DM_MSBTPRELOAD 同模式(先例已验证客体线程可直调游戏函数)。
    // DM_AUTOGAME menu confirm: the gate callback runs on the guest thread (the same
    // context as the lldb injection) — call 135fe0(Q, 1) (selector "confirm single
    // player") directly. Trampoline threads and key injection lack the context;
    // this follows the proven DM_MSBTPRELOAD pattern of calling game code on the
    // guest thread from the gate callback.
    if (g_ag_confirm_q) {
        uint64_t Q = g_ag_confirm_q; g_ag_confirm_q = 0;
        void (*confirm)(uint64_t, uint32_t) = (void (*)(uint64_t, uint32_t))(g_ag_confirm_base + 0x135fe0);
        confirm(Q, 1);
        LOG("[autogame] 菜单确认 135fe0(Q=%#llx,1) 已在客体线程执行", (unsigned long long)Q);
    }
    // DM_MSBTPRELOAD 预装：boot N 秒后（客体线程上）调 0x4efc80 预装 beatspell 剧本 msbt
    // （从 scenereq_obs 挪来——那个门黑屏后不触发；这个每帧触发）
    { static int cfg = -1; static uint64_t due; static void (*loader)(void *, const char *);
      if (cfg < 0) { const char *e = getenv("DM_MSBTPRELOAD"); if (e) { due = (now_ns() - g_t0_ns) / 1000000000ull + strtoull(e, 0, 0); cfg = 1; } else cfg = 0; }
      if (cfg == 1 && (now_ns() - g_t0_ns) / 1000000000ull >= due) {
        cfg = 2;
        module_t *mm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
        if (mm) {
          loader = (void (*)(void *, const char *))((uint64_t)mm->base + 0x4efc80);
          LOG("【红】DM_MSBTPRELOAD：预装 beatspell msbt");
          for (int wi = 1; wi <= 9; wi++) {
            char nm[64]; snprintf(nm, sizeof nm, "beatspell_v01_w%d", wi);
            static uint64_t slot[4];
            loader(&slot, nm);
            LOG("  预装 %s → 容器 %#llx", nm, (unsigned long long)slot[0]);
          }
        }
      } }
    if (rtg_observation_logs() && (++n <= 5 || n % 600 == 0)) {
        const uint64_t scene = r[0];
        // DM_SCENEDUMP：每 600 次 dump 场景对象 +0x9000..0x9600（状态字段区）——字段级快照对比用
        if (n % 600 == 0) { static int dc; if (dc++ < 60) {
            char fn[64]; snprintf(fn, sizeof fn, "run/scene_%03d.bin", dc);
            FILE *df = fopen(fn, "wb"); if (df) { fwrite((const void *)(scene + 0x9000), 1, 0x600, df); fclose(df); }
        } }
        // DM_FLAGFORCE=<秒>：boot N 秒后直接写转态对象 +0x119d+4*8 标志=1（消息处理确认位——
        // 78 号轮询 5139 次空转的数组元素）——伪造"消息已处理"看主循环是否推进（最后一击实验）
        // DM_FLAGFORCE=<sec>: after N s, directly set the +0x119d flag the main loop polls
        { static int cfg = -1; static uint64_t due;
          if (cfg < 0) { const char *e = getenv("DM_FLAGFORCE"); if (e) { due = (now_ns() - g_t0_ns) / 1000000000ull + strtoull(e, 0, 0); cfg = 1; } else cfg = 0; }
          if (cfg == 1 && (now_ns() - g_t0_ns) / 1000000000ull >= due) {
            cfg = 2;
            // 【第 8 种：组合注入】活动门卫字段（0x9400=1 + 0x94d0 活动对象）+ 消息投递——
            // 7 种单独注入失败的组合拳（门卫字段是 i_change_ok 查询的前置条件）
            {
                uint64_t s0 = scene;
                *(volatile uint32_t *)(s0 + 0x9400) = 1;
                // 0x94d0 活动对象指针——用一个非空值（之前的场景对象自身引用）
                uint64_t selfobj = s0;
                memcpy((void *)(s0 + 0x94d0), &selfobj, 8);
                LOG("【红】DM_FLAGFORCE：组合注入 0x9400=1 0x94d0=%#llx", (unsigned long long)selfobj);
            }
            // 真正的消息投递（协议级注入）：调 0x53360(总线对象, ID, 1)——
            // 之前 6 种标志伪造失败因为绕过了投递协议（0x53360 有完整存储/分发语义）
            // 总线对象 = [main+0xd96058]（0x250ba0 setter 用的同一全局）
            {
                module_t *mm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
                if (mm) {
                    uint64_t busobj; memcpy(&busobj, (const void *)((uint64_t)mm->base + 0xd96058), 8);
                    static void (*send)(void *, int, int);
                    if (!send) send = (void (*)(void *, int, int))((uint64_t)mm->base + 0x53360);
                    LOG("【红】DM_FLAGFORCE：消息投递（协议级）bus=%#llx", (unsigned long long)busobj);
                    // 关键消息 ID 批量投递（78 号真实轮询的 0x3e8 + 常规 0x4 + 场景推进候选）
                    static const int ids[] = {0x3e8, 0x4, 0x1, 0x2, 0x3, 0x5};
                    for (unsigned k = 0; k < sizeof ids / sizeof ids[0]; k++)
                        send((void *)(busobj + 8), ids[k], 1);
                }
            }
          } }
        uint32_t en = 0; uint64_t obj = 0;
        memcpy(&en, (const void *)(scene + 0x9400), 4);
        memcpy(&obj, (const void *)(scene + 0x94d0), 8);
        fprintf(stderr, "[dm] [gate6] #%d 场景=%#llx 活动0x9400=%#x 对象0x94d0=%#llx\n", n, (unsigned long long)scene, en, (unsigned long long)obj);
    }
}
void scenereq_obs(const uint64_t *r) {
    static FILE *f; static int n;
    if (!f) { const char *e = getenv("DM_SCENEREQ"); if (e && !(f = fopen(e, "w"))) LOG("DM_SCENEREQ 打不开"); }
    char line[400];
    // 调用者回溯：观测门恢复 x30 后执行原指令——这里拿不到现场 x30；改从 x0 对象+静态推导。
    // 简化：打印 (场景号, 描述) + 描述内容前 16 字节（描述是场景名/参数结构）
    uint64_t desc = r[2];
    char db[72] = ""; if (desc > 0x10000) { memcpy(db, (const void *)desc, 16); }
    // 调用者回溯：x29 链（dm_gate 保存的 regs[29] 是客体 x29）——打出本次请求的完整调用栈
    // Caller backtrace: the x29 chain (regs[29] saved by dm_gate is the guest's x29)
    char bt[300]; int q = 0;
    uint64_t fp = r[29];
    module_t *mm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
    for (int k2 = 0; k2 < 7 && fp && !(fp & 7) && q < 250; k2++) {
        uint64_t pair[2] = {0, 0};
        if (memcpy(pair, (const void *)fp, 16), !pair[1]) break;
        if (mm && pair[1] >= (uint64_t)mm->base && pair[1] < (uint64_t)mm->base + mm->size)
            q += snprintf(bt + q, sizeof bt - q, " main+%#llx", (unsigned long long)(pair[1] - (uint64_t)mm->base));
        else if (pair[1] > 0x100000000ull)
            q += snprintf(bt + q, sizeof bt - q, " heap:%#llx", (unsigned long long)pair[1]);
        fp = pair[0];
    }
    int k = snprintf(line, sizeof line, "%.3f\tscene=%llu\tdesc=%#llx\tbt:%s\n",
        (now_ns() - g_t0_ns) / 1e9, (unsigned long long)r[1], (unsigned long long)desc, bt);
    if (f) { fwrite(line, 1, k, f); fflush(f); }
    if (rtg_observation_logs() && ++n <= 50) fprintf(stderr, "[dm] [scenereq] %s", line);
}
void obs_mgrentry(const uint64_t *r);
void obs_mgrentry(const uint64_t *r);
static void obs_jobtramp(const uint64_t *r);
static void obs_progtick(const uint64_t *r);
void obs_spawn(const uint64_t *r);
// BLREDIR 回调（主循环中段调用点观测——put_branch 重定向的 bl）
// BLREDIR 记录器（带编号）：宏生成 90 个硬编码编号的包装——TSV 行「秒	ID	x0..x3」
static FILE *blredir_f(void) { static FILE *f; if (!f) { const char *e = getenv("DM_BLREDIR"); if (e) f = fopen(e, "a"); } return f; }
#define BLREC(id) static void obs_blr_##id(const uint64_t *r) {     FILE *f = blredir_f(); if (f) { fprintf(f, "%.3f\t%d\t%#llx\t%#llx\t%#llx\t%#llx\n",         (now_ns() - g_t0_ns) / 1e9, id, (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3]); fflush(f); } }
// DM_SCENEKICK 劫持：跳板写入后，在主循环门 33（每帧的转态查询）回调里劫持
// 当前线程执行跳板。执行后线程停在 park 自旋（不再回来）。一个线程只劫持一次。
// DM_SCENEKICK hijack: after the trampoline is armed, hijack the calling thread
// in gate 33 (per-frame transition query). The thread parks in a spin afterwards.
static uint64_t g_kick_entry, g_kick_park; static int g_kick_armed, g_kick_done;
void dm_scenekick_arm(uint64_t entry, uint64_t park) { g_kick_entry = entry; g_kick_park = park; g_kick_armed = 1; }
static void kick_thread(const uint64_t *r) {
    (void)r;
    if (!g_kick_armed || g_kick_done) return;
    g_kick_done = 1;
    // 保存 sp，设置 pc=跳板、lr=park
    // 注意：不能直接改回调里的寄存器——BLREDIR 门只回写 x0-x8/x30
    // 方案：通过修改 r 数组里的 x30（lr）——门回写时 lr 变成 park，
    // pc 无法直接改（BLREDIR 重放原 bl 后返回到下一条指令）
    // 所以我们只能间接：设 lr=park 让下次 blr 跳到 park（自旋）
    // 更好的方案：写跳板入口到一个全局函数指针，然后在 next frame 的 bl 处替换目标
    // 最简可行方案：直接在回调里从 C 侧调用跳板（C 函数可以间接调用 A64 代码）
    // 但 x9-x17 会被破坏——BLREDIR 不保存它们——而主循环的这些寄存器在 bl 后可能活跃
    // 所以还是用 lr 方案：让本函数返回后 blr 跳到 park
    // 实际上最安全的做法是像 lldb 那样直接设 pc——但 BLREDIR 不支持
    // 暂时放弃这个路线——保留 arm 函数供外部（lldb）使用
    LOG("[scenekick] 跳板已就绪 entry=%#llx park=%#llx（用 lldb 劫持执行）",
        (unsigned long long)g_kick_entry, (unsigned long long)g_kick_park);
}
BLREC(30) BLREC(31) BLREC(32) BLREC(33) BLREC(34) BLREC(35) BLREC(36) BLREC(37) BLREC(38) BLREC(39)
BLREC(40) BLREC(41) BLREC(42) BLREC(47) BLREC(48) BLREC(49)
// 44/45/46 = 旗标门置位（0x250710/72c/744 的 bl 0x506c50 前）：x0=旗标容器 {+0x10 字节数组指针}、
// w1=索引。真机上这三个旗标（bit0）在装载/情景前置流程里被置位，我们的运行里没人置——
// 检查失败 → [mgr+0x1ce4] 不置 1 → 情景类型注册全跳过。回调在检查执行前把该字节 bit0 置 1。
// 44/45/46 = flag-gate bootstrap (before the bl 0x506c50 at 0x250710/72c/744): x0 = flag container
// {+0x10 byte-array pointer}, w1 = index. On hardware these three flags are set by the
// pre-situation flow; nothing sets them in our run, the checks fail, [mgr+0x1ce4] never becomes 1
// and every situation registration is skipped. The callback sets the byte's bit0 right before the check.
static void dm_flagreg_set(const uint64_t *r) {
    static int logged;
    if (!getenv("DM_FLAGREG")) return;
    uint64_t x0 = r[0]; uint32_t idx = (uint32_t)r[1];
    if (!x0) return;
    uint64_t arr; memcpy(&arr, (void *)(x0 + 0x10), 8);
    if (arr && idx < 0x20) {
        volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)arr + idx;
        *b |= 1;
        if (!logged) { logged = 1; LOG("[flagreg] 旗标[%u] 检查前置位（容器 %#llx 数组 %#llx）", idx, (unsigned long long)x0, (unsigned long long)arr); }
    }
}
static void obs_blr_44(const uint64_t *r) { dm_flagreg_set(r); }
static void obs_blr_45(const uint64_t *r) { dm_flagreg_set(r); }
static void obs_blr_46(const uint64_t *r) { dm_flagreg_set(r); }
// 43 = 场景类型表补注册（0x636f0 门）：manager 构造刚把 32×0xdc 表填成 -1 空模式，
// 宿主立即把静态记录（main+0xda70c0，0x6a8×10 条）里 DM_TYPEREG 指定 situation 的注册元组
// 写进表槽 cell(e,s) = 表+e*0xdc+s*0x2c（0x2c 载荷）——真机上由消息处理器 0x588e0(situation)
// 完成，我们的运行里该处理器链从未被调（黑屏根因）；纯数据写入、无客体调用，安全。
// 43 = scene-type table bootstrap (gate at 0x636f0): the manager constructor just filled the
// 32x0xdc table with the -1 empty pattern; the host immediately copies the registration tuples
// from the static records (main+0xda70c0, 0x6a8 x 10) for the situation named by DM_TYPEREG into
// the cells cell(e,s) = table + e*0xdc + s*0x2c (0x2c payload). On hardware the message handler
// 0x588e0(situation) does this; in our run that handler chain never fires (the black-screen root
// cause). Pure data writes, no guest calls -- safe.
static void obs_blr_43(const uint64_t *r) {
    (void)r;
    const char *e = getenv("DM_TYPEREG");
    if (!e) return;
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) return;
    int sit = atoi(e);
    uint8_t *tab = (uint8_t *)m->base + 0x10b3d80;           // 表（32 项 × 0xdc）
    const uint8_t *rec = (const uint8_t *)m->base + 0xda70c0 + (size_t)sit * 0x6a8;
    const uint8_t *p = rec + 0x14;                            // 元组流 {w entry, w slot, 0x2c 载荷}，entry==-1 结尾
    int n = 0;
    for (;;) {
        int entry, slot; memcpy(&entry, p, 4); memcpy(&slot, p + 4, 4);
        if (entry == -1) break;
        if ((unsigned)entry < 0x20 && (unsigned)slot < 5) {
            memcpy(tab + (size_t)entry * 0xdc + (size_t)slot * 0x2c, p + 8, 0x2c);
            n++;
        }
        p += 0x34;
    }
    LOG("[typereg] situation %d：表初始化后已补写 %d 格（cell(e,s)=base+0x10b3d80+e*0xdc+s*0x2c）", sit, n);
}
BLREC(50) BLREC(51) BLREC(52) BLREC(53) BLREC(54) BLREC(55) BLREC(56) BLREC(57) BLREC(58) BLREC(59)
BLREC(60) BLREC(61) BLREC(62) BLREC(63) BLREC(64) BLREC(65) BLREC(66) BLREC(67) BLREC(68) BLREC(69)
BLREC(70) BLREC(71) BLREC(72) BLREC(73) BLREC(74) BLREC(75) BLREC(76) BLREC(77) BLREC(78) BLREC(79)
BLREC(80) BLREC(81) BLREC(82) BLREC(83) BLREC(84) BLREC(85) BLREC(86) BLREC(87) BLREC(88) BLREC(89)
BLREC(90) BLREC(91) BLREC(92) BLREC(93) BLREC(94) BLREC(95) BLREC(96) BLREC(97) BLREC(98) BLREC(99)
BLREC(100) BLREC(101) BLREC(102) BLREC(103) BLREC(104) BLREC(105) BLREC(106) BLREC(107) BLREC(108) BLREC(109)
BLREC(110) BLREC(111) BLREC(112) BLREC(113) BLREC(114) BLREC(115) BLREC(116) BLREC(117) BLREC(118) BLREC(119)
void (*const g_blr_fns[90])(const uint64_t *) = { obs_blr_30, obs_blr_31, obs_blr_32, obs_blr_33, obs_blr_34, obs_blr_35, obs_blr_36, obs_blr_37, obs_blr_38, obs_blr_39, obs_blr_40, obs_blr_41, obs_blr_42, obs_blr_43, obs_blr_44, obs_blr_45, obs_blr_46, obs_blr_47, obs_blr_48, obs_blr_49, obs_blr_50, obs_blr_51, obs_blr_52, obs_blr_53, obs_blr_54, obs_blr_55, obs_blr_56, obs_blr_57, obs_blr_58, obs_blr_59, obs_blr_60, obs_blr_61, obs_blr_62, obs_blr_63, obs_blr_64, obs_blr_65, obs_blr_66, obs_blr_67, obs_blr_68, obs_blr_69, obs_blr_70, obs_blr_71, obs_blr_72, obs_blr_73, obs_blr_74, obs_blr_75, obs_blr_76, obs_blr_77, obs_blr_78, obs_blr_79, obs_blr_80, obs_blr_81, obs_blr_82, obs_blr_83, obs_blr_84, obs_blr_85, obs_blr_86, obs_blr_87, obs_blr_88, obs_blr_89, obs_blr_90, obs_blr_91, obs_blr_92, obs_blr_93, obs_blr_94, obs_blr_95, obs_blr_96, obs_blr_97, obs_blr_98, obs_blr_99, obs_blr_100, obs_blr_101, obs_blr_102, obs_blr_103, obs_blr_104, obs_blr_105, obs_blr_106, obs_blr_107, obs_blr_108, obs_blr_109, obs_blr_110, obs_blr_111, obs_blr_112, obs_blr_113, obs_blr_114, obs_blr_115, obs_blr_116, obs_blr_117, obs_blr_118, obs_blr_119 };
static void obs_blrt0(const uint64_t *r) { static int n; if (rtg_observation_logs() && (++n <= 5 || n % 3000 == 0)) fprintf(stderr, "[blrt0] 主循环转态查询 #%d obj=%#llx\n", n, (unsigned long long)r[0]); }
static void obs_blrt1(const uint64_t *r) { static int n; if (rtg_observation_logs() && (++n <= 5 || n % 3000 == 0)) fprintf(stderr, "[blrt1] tick A #%d obj=%#llx\n", n, (unsigned long long)r[0]); }
static void obs_blrt2(const uint64_t *r) { (void)r; static int n; if (rtg_observation_logs() && (++n <= 5 || n % 3000 == 0)) fprintf(stderr, "[blrt2] 子系统循环 #%d\n", n); }
// std::thread spawn（0x4dfdf0）入口：x0 = 线程体函数指针——记录 boot 全部线程创建序列
// （初始化序列图：真机该 spawn 的线程 vs 我们 spawn 的——差集 = 缺失的初始化）
/* v22C（Mac 侧）：挂载链插入/移除观测——安卓首故障空表根因定位。
 * 安卓 0x510170 BinaryFileHeader ctor 走 4e18b0 空 walk → NULL → 0x14 崩；
 * 且安卓 OBS_MNTINS/REM 全程零命中=插入从未发生、Mac nnMain 早段有两个 [spawn#]
 * 安卓没有。此观测在 Mac 上打插入/移除时序+条目名+调用者，取地面真值。 */
/* v22C (Mac side): mount-list insert/remove observation -- root cause of Android's
 * empty-list first fault. On Android 0x510170 ctor walks an empty list -> NULL -> crash,
 * and OBS_MNTINS/REM never fire (no insert ever happens) while Mac's early nnMain has two
 * [spawn#] threads Android lacks. Log the insert/remove timeline with entry name and
 * caller on Mac as ground truth. */
#ifndef __ANDROID__
static void mnt_ev_obs(const uint64_t *r, int is_ins) {
    static int n;
    if (n >= 400) return;
    n++;
    module_t *mm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
    if (!mm) return;
    uint64_t entry = r[0], inner = 0, namep = 0;
    char nm[24] = "";
    /* 插入期条目未构造完：inner/name 可能是垃圾（实测 0x656d2f6870617267）——区间过滤 */
    /* At insert time the entry is half-built: inner/name can be garbage (observed
     * 0x656d2f6870617267) -- range-filter before dereferencing. */
    if (entry > 0x10000 && entry < 0x000100000000ull) {
        memcpy(&inner, (const void *)(uintptr_t)(entry + 0x10), 8);
        if (inner > 0x10000 && inner < 0x000100000000ull) { memcpy(&namep, (const void *)(uintptr_t)(inner + 0x18), 8);
            if (namep > 0x10000 && namep < 0x000100000000ull) { memcpy(nm, (const void *)(uintptr_t)namep, 23); nm[23] = 0; } }
    }
    uint64_t hb[3] = {0, 0, 0};
    memcpy(hb, (const void *)(uintptr_t)((char *)mm->base + 0x10bd738), 24);
    char caller[48] = "?";
    if (r[30] >= (uint64_t)mm->base && r[30] < (uint64_t)mm->base + mm->size)
        snprintf(caller, sizeof caller, "main+%#llx", (unsigned long long)(r[30] - (uint64_t)mm->base));
    LOG("[mntobs] %s#%d entry=%#llx name=%.23s 调用者 %s head tail=%#llx first=%#llx count=%#llx",
        is_ins ? "插入" : "移除", n, (unsigned long long)entry, nm, caller,
        (unsigned long long)hb[0], (unsigned long long)hb[1], (unsigned long long)hb[2]);
}
void dm_mntins_obs(const uint64_t *r) { mnt_ev_obs(r, 1); }
void dm_mntrem_obs(const uint64_t *r) { mnt_ev_obs(r, 0); }
#endif

void obs_spawn(const uint64_t *r) {
    static FILE *fs; static int n;
    uint64_t fn = 0;
    if (r[0] > 0x10000) memcpy(&fn, (const void *)r[0], 8);   // x0 = std::thread this：线程体 fn 在 [x0] | thread body fn at [x0]
    module_t *mm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
    char where[48];
    if (mm && fn >= (uint64_t)mm->base && fn < (uint64_t)mm->base + mm->size)
        snprintf(where, sizeof where, "main+%#llx", (unsigned long long)(fn - (uint64_t)mm->base));
    else snprintf(where, sizeof where, "heap:%#llx", (unsigned long long)fn);
    if (!fs) { const char *e = getenv("DM_SPAWNLOG"); if (e && !(fs = fopen(e, "a"))) LOG("DM_SPAWNLOG 打不开"); }
    if (fs) { fprintf(fs, "%.3f\tspawn\t%s\n", (now_ns() - g_t0_ns) / 1e9, where); fflush(fs); }
    /* v22C：安卓零客体线程定位——spawn 调用者（lr）加进日志 */
    {
        char caller[48] = "?";
        if (mm && r[30] >= (uint64_t)mm->base && r[30] < (uint64_t)mm->base + mm->size)
            snprintf(caller, sizeof caller, "main+%#llx", (unsigned long long)(r[30] - (uint64_t)mm->base));
        if (++n <= 30) LOG("[spawn#%d] 线程体 %s 调用者 %s", n, where, caller);
        else if (fs) { fprintf(fs, "%.3f\tcaller\t%s\n", (now_ns() - g_t0_ns) / 1e9, caller); fflush(fs); }
    }
}
#ifndef __ANDROID__
static void guest_main(void) {
    guest_thread_init();
    uint32_t hmain = svc_thread_main_init();   // 主线程句柄：nnosInitialize 的 x0（存入 sdk 全局，ThreadManagerImplByHorizon 取用）；x1 = 参数区指针 | Main thread handle: x0 of nnosInitialize (stored in an sdk global, used by ThreadManagerImplByHorizon); x1 = pointer to the argument area
    ((fn0)req("_init_libc0"))();
    LOG(">>> nnosInitialize"); ((fn2)req("nnosInitialize"))(hmain, 0);
    ((fn0)req("_init_libc1"))();
    LOG(">>> nninitInitializeSdkModule"); ((fn0)req("nninitInitializeSdkModule"))();
    LOG(">>> nninitStartup"); ((fn0)req("nninitStartup"))();
    ((fn0)req("_init_libc2"))();
    dm_prof_mark("② SDK 初始化（nnosInitialize → nninitStartup）");
    // DM_BOOTSTOP=1：模块已装、init_array 前 SIGSTOP 自己——lldb attach 后设 boot 期断点再 continue（boot 只在头两秒，事后 attach 抓不到）
    // DM_BOOTSTOP=1: SIGSTOP ourselves after modules load, before init_array -- lldb attaches, sets boot-period breakpoints, then continues
    if (getenv("DM_BOOTSTOP")) { for (int i = 0; i < g_nmod; i++) LOG("bootstop 模块 %s base=%#llx", g_mod[i].name, (unsigned long long)(uintptr_t)g_mod[i].base); raise(SIGSTOP); }
    // 90 点 BLREDIR 全量：30..119 号接各自硬编码编号的包装（TSV 行带 ID）
    // 必须在 run_module_inits 之前挂：43 号门（类型表补注册）要在 init_array 里的 manager 构造时触发
    // Must be attached before run_module_inits: gate 43 (type-table bootstrap) fires during the
    // manager construction inside init_array
    if (getenv("DM_BLREDIR"))
        for (int id = 30; id < 120; id++) if (gate_obs_present(id)) gate_obs_set(id, g_blr_fns[id - 30]);
    // DM_REC0FIX=1：boot 期游戏经函数指针调 0x588e0(情景 0)——记录 0 是空记录（无注册元组），
    // 类型表重置后不注册任何场景 → 场景 2 造基类空壳 → 黑屏。把记录 1（教程情景，含场景号
    // 0..3 的注册元组）的元组流拷进记录 0 的元组区（+0x14 起）——纯静态数据改写，boot 前生效。
    // DM_REC0FIX=1: at boot the game calls 0x588e0(situation 0) via a function pointer; record 0
    // is the empty record (no registration tuples), so the reset type table stays empty, scene 2
    // gets constructed as the base-class shell and the screen goes black. Copy record 1's tuple
    // stream (the tutorial situation, scene ids 0..3) into record 0's tuple area (+0x14 onward) --
    // a static-data rewrite that takes effect before boot.
    if (getenv("DM_REC0FIX")) {
        module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
        if (m) {
            uint8_t *recs = (uint8_t *)m->base + 0xda70c0;
            memcpy(recs + 0x14, recs + 0x6a8 + 0x14, 0x6a8 - 0x14);
            LOG("[rec0fix] 记录 0 的元组区已换成记录 1（教程情景）的内容");
        }
    }
    run_module_inits();   // rtld 回调 B：各模块 DT_INIT（全局构造函数） | rtld callback B: each module's DT_INIT (global constructors)
    dm_prof_mark("③ 各模块 init_array");
    ((fnp)req("_ZN2nn2os11SetHostArgcEi"))((void *)(uint64_t)g_argc);
    ((fnp)req("_ZN2nn2os11SetHostArgvEPPc"))(g_argv);
    { if (gate_obs_present(OBS_SCENEREQ)) { gate_obs_set(OBS_SCENEREQ, scenereq_obs); LOG("场景切换请求观测门已挂"); } }
    { if (gate_obs_present(OBS_MGRENTRY)) gate_obs_set(OBS_MGRENTRY, obs_mgrentry); }
    { if (gate_obs_present(OBS_JOBTRAMP)) { gate_obs_set(OBS_JOBTRAMP, obs_jobtramp); LOG("作业蹦床观测门已挂"); } }
    { if (gate_obs_present(OBS_SPAWN)) { gate_obs_set(OBS_SPAWN, obs_spawn); LOG("线程 spawn 观测门已挂"); } }
    { if (gate_obs_present(OBS_MNTINS)) { gate_obs_set(OBS_MNTINS, dm_mntins_obs); LOG("挂载链插入观测门已挂"); } }
    { if (gate_obs_present(OBS_MNTREM)) { gate_obs_set(OBS_MNTREM, dm_mntrem_obs); LOG("挂载链移除观测门已挂"); } }
    { (void)0;
      if (gate_obs_present(OBS_TRANSREQ)) gate_obs_set(OBS_TRANSREQ, obs_transreq);
      if (gate_obs_present(OBS_GATE6)) gate_obs_set(OBS_GATE6, obs_gate6);
      if (gate_obs_present(OBS_PROGTICK)) gate_obs_set(OBS_PROGTICK, obs_progtick); }
    { void dm_state_watcher_start(void); dm_state_watcher_start(); }
    { void dm_scenemgr_watcher_start(void); dm_scenemgr_watcher_start(); }
    { void dm_tablewatch_start(void); dm_tablewatch_start(); }
    { void dm_autogame_start(void); dm_autogame_start(); }
    LOG(">>> nnMain");
    {   // 21号修C:nnMain 入口 dump main 数据段(diff 找安卓/Mac 分叉字节)
        const char *e = getenv("DM_DATADUMP");
        if (e) for (int i=0;i<g_nmod;i++) if(!strcmp(g_mod[i].name,"main")){
            FILE *df = fopen(e,"wb");
            if(df){ uint64_t sz = g_mod[i].size - 0xcd9000;
                fwrite((void*)(g_mod[i].base+0xcd9000),1,sz,df); fclose(df);
                LOG("[21fix-C] main 数据段 dump %#llx 字节 → %s",(unsigned long long)sz,e); }
            break;
        }
    }
    ((fn0)req("nnMain"))();
    LOG("nnMain returned");
}
#endif   // !__ANDROID__


#include <pthread.h>
#include <unistd.h>
typedef struct { const volatile uint8_t *st, *ar; } stw_arg;
static void *stw_watcher(void *p) {
    stw_arg *a = p;
    uint8_t last = 0xff;
    for (;;) {
        uint8_t v = *a->st;
        if (v != last) { last = v; LOG("[state] app+0x18 状态字节 → %u（邻域 %02x %02x %02x %02x %02x）", v, a->ar[0x10], a->ar[0x14], a->ar[0x16], a->ar[0x19], a->ar[0x20]); }
        usleep(200000);
    }
    return NULL;
}
// 场景管理器监视（DM_SCENEMGR）：每 200ms 读 [管理器+0x10 当前场景 / +0x18 挂起号 / +0x20 描述]，变化时打印
// Scene manager watcher (DM_SCENEMGR): read current scene / pending scene / desc every 200 ms, print on change
static _Atomic uint64_t g_scenemgr;   // 场景管理器地址（OBS_MGRENTRY 观测门每帧刷新，DM_SCENEMGR 轮询用）
void obs_mgrentry(const uint64_t *r) { atomic_store(&g_scenemgr, r[0]); }
// 作业蹦床（0x4dfb20）：x0 = 作业包 {函数指针, 参数, pool}——统计每个 VM 作业的调用频率
// Job trampoline (0x4dfb20): x0 = job package {function, arg, pool} -- call frequency per VM job
#define JT_SLOTS 64
static struct { uint64_t fn; uint64_t n; uint64_t last_ns; } g_jobs[JT_SLOTS];
static void obs_jobtramp(const uint64_t *r) {
    uint64_t pkg = r[0]; if (pkg < 0x10000) return;
    uint64_t fn; memcpy(&fn, (const void *)pkg, 8);        // [pkg+0] = 作业函数
    int slot = -1;
    for (int i = 0; i < JT_SLOTS; i++) { if (g_jobs[i].n == 0) { slot = i; g_jobs[i].fn = fn; break; } if (g_jobs[i].fn == fn) { slot = i; break; } }
    if (slot < 0) return;
    g_jobs[slot].n++; g_jobs[slot].last_ns = now_ns();
}
void jobtramp_report(void) {   // SIGUSR1 时随现场导出打印（见 crash.c 挂钩处）
    fprintf(stderr, "[dm] VM 作业频率表（教程期+黑屏期）：\n");
    for (int i = 0; i < JT_SLOTS && g_jobs[i].n; i++)
        fprintf(stderr, "  作业 fn=%#llx 调用 %llu 次（最近 %.1fs 前）\n", (unsigned long long)g_jobs[i].fn, (unsigned long long)g_jobs[i].n, (now_ns() - g_jobs[i].last_ns) / 1e9);
}
static void *scenemgr_watcher(void *p) {
    (void)p;
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) return NULL;
    // 场景管理器对象：用 0x509744 观测门时见过 x0=0x3406f6960（每局稳定？boot 后从全局拿更稳）
    // 简化：从全局 0x10a75f0+0x3aa0 的对象（与门卫同一全局）——不对。直接扫：场景管理器 vtable 在 main+0xd2b730
    // 最稳来源：0x509744（场景管理器入口）观测门——但还没装。这里先用轮询 0x3406f6960（实测稳定地址）
    uint64_t mgr = 0;  // 由 OBS_MGRENTRY 观测门提供（每帧刷新） | provided by the OBS_MGRENTRY gate
    for (int spin = 0; !mgr && spin < 600; spin++) { mgr = atomic_load(&g_scenemgr); usleep(100000); }
    uint64_t last[3] = {1, 1, 1};
    for (;;) {
        uint64_t cur[3];
        memcpy(&cur[0], (const void *)(mgr + 0x10), 8);
        memcpy(&cur[1], (const void *)(mgr + 0x18), 8);
        memcpy(&cur[2], (const void *)(mgr + 0x20), 8);
        if (cur[0] != last[0] || cur[1] != last[1] || cur[2] != last[2]) {
            LOG("[scenemgr] 当前场景=%#llx 挂起=%#llx(%lld) 描述=%#llx", (unsigned long long)cur[0], (unsigned long long)cur[1], (long long)(int32_t)cur[1], (unsigned long long)cur[2]);
            last[0] = cur[0]; last[1] = cur[1]; last[2] = cur[2];
        }
        // DM_SCENEFORCE=<秒>:<场景号>：boot 后 N 秒直接把挂起请求写为指定场景号（绕过实验——
        // 伪造"教程已完成，切到场景 N"——如果新场景构造+全链跑通，黑屏根因确认在挂起请求的写入者）
        // DM_SCENEFORCE=<sec>:<scene>: after N seconds force the pending scene request (bypass experiment)
        { static int cfg = -1; static uint64_t due; static int32_t sceneno;
          if (cfg < 0) { const char *e = getenv("DM_SCENEFORCE"); if (e && sscanf(e, "%llu:%d", (unsigned long long *)&due, &sceneno) == 2) { cfg = 1; due = (now_ns() - g_t0_ns) / 1000000000ull + due; } else cfg = 0; }
          if (cfg == 1 && (now_ns() - g_t0_ns) / 1000000000ull >= due && (int32_t)cur[1] < 0) {
            LOG("【红】DM_SCENEFORCE：%.0f 秒到，强制挂起请求=场景 %d", (now_ns() - g_t0_ns) / 1e9, sceneno);
            uint64_t v = (uint32_t)sceneno;
            memcpy((void *)(mgr + 0x18), &v, 8);   // 挂起号 | pending scene number
            cfg = 2;
          } }
        usleep(200000);
    }
    return NULL;
}
void dm_scenemgr_watcher_start(void) { if (getenv("DM_SCENEMGR")) { pthread_t th; pthread_create(&th, NULL, scenemgr_watcher, NULL); pthread_detach(th); } }

void dm_state_watcher_start(void) {
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) return;
    stw_arg *a = malloc(sizeof *a);
    a->st = (const volatile uint8_t *)(m->base + 0x1106588 + 0x18);
    a->ar = (const volatile uint8_t *)(m->base + 0x1106588);
    pthread_t th; pthread_create(&th, NULL, stw_watcher, a); pthread_detach(th);
}

// DM_TABLEWATCH=1：紧密轮询场景类型表首格（main+0x10b3d80），从 0 变 -1/0xb 空模式的瞬间
// SIGUSR1 导出全部线程回溯——抓住 boot 期真正给表做初始化的调用链（静态分析各候选 BL 都不命中）
// DM_TABLEWATCH=1: tight-poll the first cell of the scene type table (main+0x10b3d80); the moment it
// flips from 0 to the -1/0xb empty pattern, SIGUSR1 dumps every thread's backtrace -- catching the
// real boot-time initializer call chain (none of the candidate BL sites ever fire).
static void *tblw_thread(void *p) {
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    volatile uint32_t *cell = p;
    int sit = -1; int sit_all = 0;
    { const char *e = getenv("DM_TYPEREG"); if (e) { if (!strcmp(e, "all")) sit_all = 1; else sit = atoi(e); } }
    for (;;) {
        uint32_t v = *cell;
        if (v == 0xffffffff) {
            LOG("[tablewatch] 类型表首格已初始化（-1 模式出现，%.4f s）", (now_ns() - g_t0_ns) / 1e9);
            // DM_TYPEREG=<sit>：初始化完成的瞬间补写记录 <sit> 的注册元组。真机上情景注册
            //（0x588e0 → 0x628a0）随后发生；我们的运行该链从不执行（记录 0 = 空情景）。
            // manager 构造在工作者线程上跑、表初始化后短时间内无读者——轮询窗口内补写是安全的
            // DM_TYPEREG=<sit>: the instant init completes, write record <sit>'s registration tuples.
            // On hardware the situation registration (0x588e0 -> 0x628a0) follows; in our run that
            // chain never executes (record 0 = the empty situation).
            if ((sit >= 0 || sit_all) && m) {
                uint8_t *tab = (uint8_t *)m->base + 0x10b3d80;
                int n = 0, lo = sit_all ? 1 : sit, hi = sit_all ? 8 : sit;
                for (int sr = lo; sr <= hi; sr++) {
                    const uint8_t *rec = (const uint8_t *)m->base + 0xda70c0 + (size_t)sr * 0x6a8;
                    const uint8_t *q = rec + 0x14;
                    for (;;) {
                        int entry, slot; memcpy(&entry, q, 4); memcpy(&slot, q + 4, 4);
                        if (entry == -1) break;
                        if ((unsigned)entry < 0x20 && (unsigned)slot < 5) {
                            memcpy(tab + (size_t)entry * 0xdc + (size_t)slot * 0x2c, q + 8, 0x2c);
                            n++;
                        }
                        q += 0x34;
                    }
                }
                LOG("[typereg] 表初始化后补写 %s：%d 格", sit_all ? "情景 1-8 全部" : "situation 1", n);
            }
            if (getenv("DM_TABLEWATCH")) raise(SIGUSR1);
            return NULL;
        }
        if (v != 0) { LOG("[tablewatch] 首格异常值 %#x，继续观察", v); }
        usleep(200);
    }
    return NULL;
}
// DM_SCENEKICK=<秒>：起局后指定秒数在补丁区零填充段（base+0xa66c00）自动写入
// C 方案两调用跳板（509570(S,R,0)+130500(S,1)），然后在下一个观测门回调里劫持执行。
// 跳板自动从 [base+0x1106580] 动态推导 A→T→S→C→R；执行后 worker 停在 park 自旋。
// 注意：不能写 base+0xa66000（那是 OS 导入 PLT！Codex 修正）。
// DM_SCENEKICK=<sec>: after the given seconds, write the C-plan two-call trampoline
// (509570(S,R,0)+130500(S,1)) into the zero-filled gap at base+0xa66c00, then hijack
// a thread to execute it on the next gate callback. The trampoline dynamically resolves
// A→T→S→C→R from [base+0x1106580]; after execution the worker parks in a spin.
// NOTE: base+0xa66000 is the OS import PLT (Codex finding) - must NOT overwrite it.
static void scene_kick_build(module_t *m) {
    uint64_t base = (uint64_t)m->base;
    uint8_t *scratch = (uint8_t *)base + 0xa66c00;
    // 校验零填充（已有代码/数据则放弃）
    for (int i = 0; i < 120; i += 4) {
        uint32_t v; memcpy(&v, scratch + i, 4);
        if (v) { LOG("[scenekick] 补丁区非零，放弃"); return; }
    }
    // 跳板 A64：
    //   stp x20,x30,[sp,#-16]!
    //   mov x0, B+0x1106580; ldr x0,[x0]; ldr x0,[x0,#0x10]; ldr x0,[x0,#0x10] → S
    //   mov x20,x0; ldr w1,[x0,#8] → R
    //   mov w2,#0; mov x0,x20; mov x16,B+0x509570; blr x16
    //   mov w1,#1; mov x0,x20; mov x16,B+0x130500; blr x16
    //   ldp x20,x30,[sp],#16; ret
    //   b .  (park)
    uint32_t words[] = {
        0xA9BF7BF4,                     // stp x20,x30
        0,0,0,0,                        // mov x0 (4 instr) - placeholder
        0xF9400000, 0xF9400800, 0xF9400800, // ldr A, ldr T, ldr S
        0xAA0003F4,                     // mov x20,x0
        0xB9400801,                     // ldr w1,[x0,#8]; B9400101 reads [x8]
        0x52800002,                     // mov w2,#0
        0xAA1403E0,                     // mov x0,x20
        0,0,0,0, 0xD63F0200,            // mov x16(4) + blr
        0x52800021,                     // mov w1,#1
        0xAA1403E0,                     // mov x0,x20
        0,0,0,0, 0xD63F0200,            // mov x16(4) + blr
        0xA8C17BF4, 0xD65F03C0,        // ldp x20,x30 / ret
        0x14000000,                     // park: b .
    };
    // 填 mov x0 和 mov x16（movz/movk 序列）
    uint64_t vals[] = { base + 0x1106580, base + 0x509570, base + 0x130500 };
    for (int i = 1; i < 5; i += 4) {
        uint64_t v = vals[0];
        words[i]   = 0xD2800000 | ((v & 0xFFFF) << 5);
        words[i+1] = 0xF2800000 | (1 << 21) | (((v >> 16) & 0xFFFF) << 5);
        words[i+2] = 0xF2800000 | (2 << 21) | (((v >> 32) & 0xFFFF) << 5);
        words[i+3] = 0xF2800000 | (3 << 21) | (((v >> 48) & 0xFFFF) << 5);
    }
    // mov x16 for 509570（在 words[12]..words[15]）
    {
        uint64_t v = vals[1];
        words[12] = 0xD2800010 | ((v & 0xFFFF) << 5);
        words[13] = 0xF2800010 | (1 << 21) | (((v >> 16) & 0xFFFF) << 5);
        words[14] = 0xF2800010 | (2 << 21) | (((v >> 32) & 0xFFFF) << 5);
        words[15] = 0xF2800010 | (3 << 21) | (((v >> 48) & 0xFFFF) << 5);
    }
    // mov x16 for 130500（在 words[19]..words[22]）
    {
        uint64_t v = vals[2];
        words[19] = 0xD2800010 | ((v & 0xFFFF) << 5);
        words[20] = 0xF2800010 | (1 << 21) | (((v >> 16) & 0xFFFF) << 5);
        words[21] = 0xF2800010 | (2 << 21) | (((v >> 32) & 0xFFFF) << 5);
        words[22] = 0xF2800010 | (3 << 21) | (((v >> 48) & 0xFFFF) << 5);
    }
    // 写入
    memcpy(scratch, words, sizeof(words));
    // 记录跳板入口/park 供门劫持用
    extern void dm_scenekick_arm(uint64_t entry, uint64_t park);
    dm_scenekick_arm((uint64_t)scratch, (uint64_t)scratch + sizeof(words) - 4);
    LOG("[scenekick] 跳板已写入 base+0xa66c00（%zu 字节）——等门劫持", sizeof(words));
}

void dm_tablewatch_start(void) {
    if (!getenv("DM_TABLEWATCH") && !getenv("DM_TYPEREG")) return;
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) return;
    pthread_t th; pthread_create(&th, NULL, tblw_thread, (void *)(m->base + 0x10b3d80)); pthread_detach(th);
}

// DM_AUTOGAME=1：把 lldb 注入链固化进宿主——
//   场景 2（教程/标题）：写 S+9420=1 完成教程确认，稳定后写入 R=3 转场蹦板并 arm
//   （门劫持执行 509570(S,3,0)+130500(S,1) → 主菜单）；
//   场景 3（主菜单）：写 U+3150=1 与 296b=1 触发 item 建立，然后模拟「下 + A」
//   确认「单人游玩」进入游戏。写内存均为小字段写；按键走 dm_input_set（按下沿
//   锁存，任意线程可调）；转场蹦板由既有的 scenekick 门劫持机制执行。
// DM_AUTOGAME=1: bake the lldb injection chain into the host —
//   scene 2 (tutorial/title): write S+9420=1 to confirm the tutorial, then arm the
//   R=3 transition trampoline (gate hijack runs 509570(S,3,0)+130500(S,1) → menu);
//   scene 3 (menu): write U+3150=1 and 296b=1 to build items, then simulate Down + A
//   to pick "single player" and enter the game.
static int g_ag_menu_kicked;
static int g_ag_boot_kicked;
// 通用转场蹦板线程：在 RWX 匿名页里写 509570(S,R,0)+130500(S,1) 两调用序列，
// 由专用线程执行一次后退出（与 lldb 的 worker 劫持等价，但不借用游戏线程，
// 因此不需要恢复寄存器；509570/130500 内部有自己的锁，跨线程安全——r152/r155 验证）。
// Generic transition trampoline: write the two-call sequence into an anonymous RWX page
// and run it once on a dedicated thread (equivalent to the lldb worker hijack, but
// borrowing no game thread, so no register restore is needed).
struct ag_tramp { uint64_t base, scene; int32_t dest; };
static void ag_fill_tramp(uint32_t *w, uint64_t base, int32_t dest) {
    // stp x20,x30,[sp,#-16]!; mov x0(4); ldr x0,[x0]; ldr x0,[x0,#0x10]; ldr x0,[x0,#0x10]
    // mov x20,x0; movz w1,#dest; mov w2,#0; mov x0,x20; mov x16(4); blr; mov w1,#1;
    // mov x0,x20; mov x16(4); blr; ldp x20,x30,[sp],#16; ret
    w[0] = 0xA9BF7BF4;
    uint64_t a = base + 0x1106580;
    w[1] = 0xD2800000 | ((a & 0xFFFF) << 5);
    w[2] = 0xF2800000 | (1 << 21) | (((a >> 16) & 0xFFFF) << 5);
    w[3] = 0xF2800000 | (2 << 21) | (((a >> 32) & 0xFFFF) << 5);
    w[4] = 0xF2800000 | (3 << 21) | (((a >> 48) & 0xFFFF) << 5);
    w[5] = 0xF9400000; w[6] = 0xF9400800; w[7] = 0xF9400800;
    w[8] = 0xAA0003F4;
    w[9] = 0x52800001 | (((uint32_t)dest & 0xFFFF) << 5);   // movz w1,#dest
    w[10] = 0x52800002;
    w[11] = 0xAA1403E0;
    uint64_t f1 = base + 0x509570;
    w[12] = 0xD2800010 | ((f1 & 0xFFFF) << 5);
    w[13] = 0xF2800010 | (1 << 21) | (((f1 >> 16) & 0xFFFF) << 5);
    w[14] = 0xF2800010 | (2 << 21) | (((f1 >> 32) & 0xFFFF) << 5);
    w[15] = 0xF2800010 | (3 << 21) | (((f1 >> 48) & 0xFFFF) << 5);
    w[16] = 0xD63F0200;
    w[17] = 0x52800021;
    w[18] = 0xAA1403E0;
    uint64_t f2 = base + 0x130500;
    w[19] = 0xD2800010 | ((f2 & 0xFFFF) << 5);
    w[20] = 0xF2800010 | (1 << 21) | (((f2 >> 16) & 0xFFFF) << 5);
    w[21] = 0xF2800010 | (2 << 21) | (((f2 >> 32) & 0xFFFF) << 5);
    w[22] = 0xF2800010 | (3 << 21) | (((f2 >> 48) & 0xFFFF) << 5);
    w[23] = 0xD63F0200;
    w[24] = 0xA8C17BF4; w[25] = 0xD65F03C0;
}
static void *ag_tramp_run(void *p) {
    struct ag_tramp *t = p;
    extern void crash_thread_init(void);
    extern void guest_thread_init(void);
    crash_thread_init();
    guest_thread_init();   // 装上客体线程 TSD（门/x18/TLS/TLR）——游戏函数的线程依赖 | install guest-thread TSD
    // （进程退出时 TSD 析构会释放这些槽，与游戏线程同路）
    // W^X：先 RW 写入蹦板，再 mprotect 成 RX 执行（macOS/安卓都禁 RWX 匿名映射）
    // W^X: write the trampoline with RW, then mprotect to RX to execute (macOS and
    // Android both refuse anonymous RWX mappings).
    void *page = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) { LOG("[autogame] 蹦板 mmap 失败 errno=%d", errno); return NULL; }
    uint32_t w[26];
    ag_fill_tramp(w, t->base, t->dest);
    memcpy(page, w, sizeof(w));
    __sync_synchronize();
    if (mprotect(page, 0x1000, PROT_READ | PROT_EXEC) != 0) { LOG("[autogame] 蹦板 mprotect 失败 errno=%d", errno); munmap(page, 0x1000); return NULL; }
    ((void (*)(void))page)();   // 执行两调用序列（S 从 A 链动态推导）| run the two-call sequence
    munmap(page, 0x1000);
    return NULL;
}
struct ag_call { uint64_t fn, this_; uint32_t w1; };
static void *ag_call_run(void *p) {
    struct ag_call *c = p;
    extern void crash_thread_init(void);
    extern void guest_thread_init(void);
    crash_thread_init();
    guest_thread_init();
    void *page = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) { LOG("[autogame] 调用蹦板 mmap 失败"); return NULL; }
    uint32_t w[16];
    // stp x19,x30,[sp,#-16]!; mov x19,this(4); mov x0,x19; movz w1,#w1;
    // mov x16,fn(4); blr x16; ldp x19,x30,[sp],#16; ret
    w[0] = 0xA9BF7BF3;
    w[1] = 0xD2800003 | ((c->this_ & 0xFFFF) << 5);
    w[2] = 0xF2800000 | (1 << 21) | (((c->this_ >> 16) & 0xFFFF) << 5);
    w[3] = 0xF2800000 | (2 << 21) | (((c->this_ >> 32) & 0xFFFF) << 5);
    w[4] = 0xF2800000 | (3 << 21) | (((c->this_ >> 48) & 0xFFFF) << 5);
    w[5] = 0xAA1303E0;
    w[6] = 0x52800001 | ((c->w1 & 0xFFFF) << 5);
    w[7] = 0xD2800010 | ((c->fn & 0xFFFF) << 5);
    w[8] = 0xF2800010 | (1 << 21) | (((c->fn >> 16) & 0xFFFF) << 5);
    w[9] = 0xF2800010 | (2 << 21) | (((c->fn >> 32) & 0xFFFF) << 5);
    w[10] = 0xF2800010 | (3 << 21) | (((c->fn >> 48) & 0xFFFF) << 5);
    w[11] = 0xD63F0200;
    w[12] = 0xA8C17BF3; w[13] = 0xD65F03C0;
    memcpy(page, w, sizeof(w));
    __sync_synchronize();
    if (mprotect(page, 0x1000, PROT_READ | PROT_EXEC) != 0) { LOG("[autogame] 调用蹦板 mprotect 失败"); munmap(page, 0x1000); return NULL; }
    // 给蹦板线程装客体 ELF TLS 块（tpidr_el0）——游戏函数的 C++/libc TLS 依赖；
    // 8192 字节全零与 guest_thread_init 的分配一致。x18/tpidrro 是 Apple 平台保留寄存器，
    // 宿主上不可用户态改写；客体对 TLR 的访问走 hle 拦截，不在这里处理。
    // Install a guest ELF TLS block (tpidr_el0) on this thread — the C++/libc TLS
    // dependency of game functions; 8192 zeroed bytes matches guest_thread_init's
    // allocation. x18/tpidrro is Apple's reserved platform register and cannot be
    // written from user mode here; guest TLR access is intercepted in hle.
    void *tls = calloc(1, 8192);
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tls));
    ((void (*)(void))page)();
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tls));   // 恢复无意义但无害;进程内泄漏 8KB/次可忽略
    free(tls);
    munmap(page, 0x1000);
    return NULL;
}
static void ag_call_fn(uint64_t fn, uint64_t this_, uint32_t w1) {
    static struct ag_call c;
    c.fn = fn; c.this_ = this_; c.w1 = w1;
    pthread_t th; pthread_create(&th, NULL, ag_call_run, &c);
    pthread_detach(th);
    usleep(300000);
}
static void ag_run_transition(module_t *m, int32_t dest) {
    static struct ag_tramp t;   // 静态：线程参数生存期
    t.base = (uint64_t)m->base; t.dest = dest;
    pthread_t th; pthread_create(&th, NULL, ag_tramp_run, &t);
    pthread_detach(th);
    usleep(300000);   // 给蹦板线程时间送出请求（509570/130500 内部有锁，不依赖本睡眠） | give the trampoline a moment
}
static void *autogame_thread(void *p) {
    module_t *m = p;
    int stage = 0; uint64_t t0 = 0;
    for (;;) {
        usleep(50000);   // 20 Hz 轮询 | 20 Hz poll
        uint64_t app = 0, mgr = 0, scene = 0;
        memcpy(&app, (uint8_t *)m->base + 0x1106580, 8); if (!app) continue;
        memcpy(&mgr, (uint8_t *)app + 0x10, 8); if (!mgr) continue;
        memcpy(&scene, (uint8_t *)mgr + 0x10, 8); if (!scene) continue;
        int32_t sid = 0; memcpy(&sid, (uint8_t *)scene + 8, 4);
        if (sid == 1 || sid == 8) {
            // —— 场景 1（boot logo，TYPEREG 下会卡）/ 场景 8（标题确认的旁支）：
            // 稳定 15 秒后请求场景 2（教程/标题）。 | scene 1 (boot, stalls under TYPEREG)
            // or scene 8 (title side branch): after 15 s idle, request scene 2.
            if (g_ag_boot_kicked) continue;
#ifdef __ANDROID__
            // 安卓 28 号修：主循环 0.5s 即死于懒加载崩溃链——15s 等永远等不到。scene 链一
            // 出现（type 表补写完成、场景对象已建）立刻 kick R=2，抢在首次懒加载前送出
            // 转场请求（游戏转场机制会绕开懒加载分支）。
            // Android fix 28: the main loop dies in the lazy-load chain at 0.5 s, so the
            // 15 s wait never completes. Kick R=2 as soon as the scene chain exists (the
            // type-table fill is done and the scene object built), racing the first lazy
            // load (the transition machinery bypasses the lazy-load branch).
            if (!t0) { t0 = now_ns(); continue; }
            if (now_ns() - t0 < 200000000ull) continue;   // 等 0.2s 让 boot 场景稳定挂到管理器 | 0.2 s for the boot scene to attach
#else
            if (!t0) { t0 = now_ns(); continue; }
            if (now_ns() - t0 < 15000000000ull) continue;
#endif
            g_ag_boot_kicked = 1;
            ag_run_transition(m, 2);
            LOG("[autogame] boot→场景 2 转场请求已送出");
            t0 = now_ns();
            continue;
        }
        if (sid == 2) {
            // —— 场景 2：先写教程确认（S+9420=1）。若 8 秒内未被消费（仍为 1），
            // 说明是 boot 形态（logo，与教程同 id）——请求 R=2 重建进教程；教程确认
            // 被消费（9420=2）后再请求 R=3 进主菜单。
            // —— scene 2: write the tutorial confirm (S+9420=1). If it is not consumed
            // within 8 s (still 1), this is the boot form (logo, same id) — request R=2
            // to rebuild into the tutorial; once consumed (9420=2), request R=3 (menu).
            uint32_t st = 0; memcpy(&st, (uint8_t *)scene + 0x941c, 4);
            if (st != 1) continue;
            int32_t cf = 0; memcpy(&cf, (uint8_t *)scene + 0x9420, 4);
            if (cf == 0) {
                int32_t one = 1; memcpy((uint8_t *)scene + 0x9420, &one, 4);
                LOG("[autogame] 教程确认已写入（S+9420=1）");
                t0 = now_ns();
                continue;
            }
            if (cf == 1) {
                // 未消费 → boot 形态：kick R=2 重建（只做一次；若已 kick 过就只等待）
                if (now_ns() - t0 < 8000000000ull) continue;
                if (!g_ag_boot_kicked) {
                    g_ag_boot_kicked = 1;
                    ag_run_transition(m, 2);
                    LOG("[autogame] boot 形态：转场请求已送出（R=2，重建进教程）");
                }
                t0 = now_ns();
                continue;
            }
            // cf==2：教程已确认（标题画面）——等 6 秒稳定后请求主菜单
            if (now_ns() - t0 < 6000000000ull) continue;   // 等标题画面稳定 | let the title settle
            if (g_ag_menu_kicked) continue;   // 只做一次（防重入） | do it only once
            g_ag_menu_kicked = 1;
            ag_run_transition(m, 3);   // 509570(S,3,0)+130500(S,1) 请求主菜单
            LOG("[autogame] 主菜单转场请求已送出（R=3）");
            t0 = now_ns();
            continue;
        }
        if (sid != 3) { stage = 0; continue; }   // 只在场景 3（主菜单）继续 | continue only in scene 3
        uint32_t st = 0; memcpy(&st, (uint8_t *)scene + 0x941c, 4);
        if (st != 1) continue;
        uint64_t ctl = 0; memcpy(&ctl, (uint8_t *)scene + 0xa078, 8);   // controller
        uint8_t *U = (uint8_t *)ctl + 0x2c98;   // UI = controller + 0x2c98（Codex 公式，恒定）
        uint64_t Q = 0; memcpy(&Q, U + 0x3cc8, 8);
        if (stage == 0) {
            // 触发 item 创建链：等价于写 U+3150=1 并调用 505a90(U,0)（后者只是写
            // 2969/296a/296b 三个字节）。3150 的旧值可能是浮点残影（0x3f800000），
            // 不能当作「忙」判断——直接覆盖为 1。
            // Trigger the item-creation chain: equivalent to writing U+3150=1 and calling
            // 505a90(U,0) (the latter just writes the 2969/296a/296b bytes). A stale
            // float-looking value (0x3f800000) may sit in 3150 — overwrite it with 1.
            uint32_t one = 1; memcpy(U + 0x3150, &one, 4);
            U[0x2969] = 1; U[0x296a] = 0; U[0x296b] = 1;
            LOG("[autogame] 菜单 item 建立已触发（3150=1 + 296b=1）");
            stage = 1; t0 = now_ns();
            continue;
        }
        if (stage == 1) {
            // 等 12b3d0→24f170→135460 建好 item 数组（Q+0x18 → 数组首项非零）
            if (!Q) continue;
            uint64_t arr = 0; memcpy(&arr, (uint8_t *)Q + 0x18, 8); if (!arr) continue;
            uint64_t i0 = 0; memcpy(&i0, (uint8_t *)arr, 8); if (!i0) continue;
            U[0x296b] = 0;   // 停止重复触发 | stop re-triggering
            LOG("[autogame] 菜单 item 已建立（Q=%llx），准备选择", (unsigned long long)Q);
            stage = 2; t0 = now_ns();
            continue;
        }
        if (stage == 2) {
            // 菜单稳定 2 秒后把确认交给 obs_gate6(每帧的客体线程门回调)执行——
            // 135fe0(Q,1) 与 lldb 版同上下文。Q 此刻重读并校验(Q+0x20 应为 9)。
            // After the menu settles for 2 s hand the confirm to obs_gate6 (the
            // per-frame guest-thread gate callback) — 135fe0(Q, 1) runs in the same
            // context as the lldb version. Re-read and sanity-check Q here.
            if (now_ns() - t0 < 2000000000ull) continue;
            uint64_t Q2 = 0; memcpy(&Q2, U + 0x3cc8, 8);
            int32_t n = 0; if (Q2 > 0x1000) memcpy(&n, (uint8_t *)Q2 + 0x20, 4);
            if (Q2 <= 0x1000 || n != 9) { LOG("[autogame] selector 异常 Q=%#llx n=%d，等待重建", (unsigned long long)Q2, n); t0 = now_ns(); continue; }
            g_ag_confirm_base = (uint64_t)m->base;
            g_ag_confirm_q = Q2;   // 下一个门回调(客体线程)消费
            LOG("[autogame] 确认已提交给门回调（Q=%#llx）", (unsigned long long)Q2);
            stage = 4; t0 = now_ns();
            continue;
        }
        if (stage == 4) {
            // 等 8 秒;若仍停在场景 3(确认未生效),重试一次
            if (now_ns() - t0 < 8000000000ull) continue;
            int32_t sid2 = 0; memcpy(&sid2, (uint8_t *)scene + 8, 4);
            if (sid2 == 3) {
                uint64_t Q2 = 0; memcpy(&Q2, U + 0x3cc8, 8);
                if (Q2 > 0x1000) { g_ag_confirm_base = (uint64_t)m->base; g_ag_confirm_q = Q2; }
                LOG("[autogame] 确认重试一次（Q=%#llx）", (unsigned long long)Q2);
                t0 = now_ns();
                continue;
            }
            LOG("[autogame] 已离开场景 3，自动推进结束");
            return NULL;
        }
    }
    return NULL;
}

void dm_autogame_start(void) {
    if (!getenv("DM_AUTOGAME")) return;
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) return;
    pthread_t th; pthread_create(&th, NULL, autogame_thread, m); pthread_detach(th);
}

static void flags_init(void) {
    g_flag.thr = !!getenv("DM_THR"); g_flag.svc = !!getenv("DM_SVC"); g_flag.fs = !!getenv("DM_FS");
    g_flag.movie = !!getenv("DM_MOVIE"); g_flag.dbg = !!getenv("DM_DBG"); g_flag.imports = !!getenv("DM_IMPORTS");
    g_flag.bootprof = !!getenv("DM_BOOTPROF");
}

// 启动时间线零点 = 进程创建时刻（sysctl KERN_PROC_PID 的 p_starttime，墙钟）换算到 now_ns 单调时基；Mac / iOS 同口径
// Zero point of the boot timeline = process creation time (p_starttime from sysctl KERN_PROC_PID, wall clock) converted to the now_ns monotonic time base; same definition on Mac / iOS
static uint64_t proc_start_mono_ns(void) {
    uint64_t mono = now_ns(); struct timeval tv; gettimeofday(&tv, NULL);
#ifdef __APPLE__
    struct kinfo_proc kp; size_t n = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(mib, 4, &kp, &n, NULL, 0) || !n) return mono;
    int64_t age = (int64_t)(tv.tv_sec - kp.kp_proc.p_starttime.tv_sec) * 1000000000ll + (tv.tv_usec - kp.kp_proc.p_starttime.tv_usec) * 1000ll;
    return age > 0 ? mono - (uint64_t)age : mono;
#else
    (void)tv; return mono;   // 安卓无 kinfo_proc——用当前单调时刻（时间线零点差不影响逻辑） | Android has no kinfo_proc - use the current monotonic instant
#endif
}

// 宿主装载（与 UIKit / Metal 无关）：读 NSO、重定位、强制接管、探针 / 拍门 / 按键、预取起跑。缺省在 main 里同步跑；
// iOS 前端可在 App 的 main() 一开头调 dm_host_preload_start(同一组参数) 让它在后台线程先跑，与 UIKit 启动链并行（⓪ + ① ≈ max(UIKit 链, 宿主装载)）。
// Host loading (independent of UIKit / Metal): read the NSO, relocate, forced take-over, probes / tick gate / keys, start prefetch. By default it runs synchronously in main;
// the iOS front end can call dm_host_preload_start(same arguments) at the very start of the App's main() so that it runs first on a background thread, in parallel with the UIKit startup chain (⓪ + ① ≈ max(UIKit chain, host loading)).
static pthread_key_t g_race_keys[512] __attribute__((unused));
static unsigned g_race_n __attribute__((unused)), g_race_want __attribute__((unused));
#ifndef __ANDROID__
uint64_t g_t0_ns;   // v22C 回补：安卓移植期从 main.c 删了定义（安卓在 app_jni.c 定义），Mac 链接缺符号
struct dm_flags g_flag;
static void *keyrace_thread(void *unused);
static void host_preload(int argc, char **argv) {
    g_t0_ns = proc_start_mono_ns();
    flags_init();
    dm_prof_mark("⓪ 进程创建 → 宿主 main（dyld）");
    if (argc < 2) die("usage: dmhost <dir> [mod...] [-- 游戏参数...]");
    // 时区：游戏把绝对时刻换成本地时刻走两条路，都落到宿主 libc——nn::time::ToCalendarTime（hle_time.c，localtime_r）和客体 libc 的
    // localtime_r / mktime（hle_table.c 直连宿主）。所以宿主跟系统时区走，原版界面（编年史发现时刻等）才是本机时间。
    // 只有确定性对拍（DM_FIXED_TIME）钉成 UTC：同一份回放在本机（东八区）和 mini（东九区）上要换出同样的日历时刻
    // Time zone: the game converts absolute time to local time along two paths, both ending in the host libc -- nn::time::ToCalendarTime (hle_time.c, localtime_r) and the guest libc's
    // localtime_r / mktime (hle_table.c wires them straight to the host). So only when the host follows the system time zone do the original game's screens (chronicle discovery time etc.) show this machine's time.
    // Only deterministic comparison (DM_FIXED_TIME) pins it to UTC: the same replay must convert to the same calendar time on this machine (UTC+8) and on the second Mac (UTC+9)
    if (getenv("DM_FIXED_TIME")) setenv("TZ", "UTC", 1);
    crash_install();
    const char *race = getenv("DM_TEST_KEYRACE");
    if (race && *race) {
        g_race_want = (unsigned)strtoul(race, NULL, 10);
        if (g_race_want > 512) g_race_want = 512;
        pthread_t th;
        if (pthread_create(&th, NULL, keyrace_thread, NULL)) die("DM_TEST_KEYRACE 起线程失败");
        pthread_detach(th);
    }
    { void dm_term_install(void); dm_term_install(); }   // SIGTERM 优雅退出（hle_lifecycle.c） | SIGTERM graceful exit (hle_lifecycle.c)
    { void crash_set_dump_hook(void (*)(void)); void svc_thread_dump(void); crash_set_dump_hook(svc_thread_dump); }   // 崩溃时导出客体线程现场 | Dump guest thread state on a crash
    const char *names[MAX_MOD]; int n = 0;
    int i = 2;
    for (; i < argc && strcmp(argv[i], "--") && n < MAX_MOD; i++) names[n++] = argv[i];
    if (i < argc && !strcmp(argv[i], "--")) for (i++; i < argc && g_argc < 63; i++) g_argv[g_argc++] = argv[i];
    if (!n) { names[n++] = "main"; names[n++] = "sdk"; }
    load_modules(argv[1], names, n);
    uint64_t ta = now_ns();
    probe_install();   // DM_PROBE 探针（诊断用） | DM_PROBE probes (for diagnostics)
    ticks_install();   // DM_TICKS 逐拍状态哈希（拍入口观测门） | DM_TICKS per-tick state hash (tick-entry observation gate)
    hid_keys_init();   // DM_KEYS 脚本输入（按 UI 消息驱动的步骤要在消息出现前订阅） | DM_KEYS scripted input (steps driven by UI messages must subscribe before the message appears)
    uint64_t tb = now_ns();
    uint64_t tc = now_ns();
    stubs_report();
    if (g_flag.bootprof) LOG("① 细分（装载后）：探针 / 拍门 / 按键 %.1f ms、预取启动（读清单）%.1f ms、桩报告 %.1f ms", (tb - ta) / 1e6, (tc - tb) / 1e6, (now_ns() - tc) / 1e6);
    dm_prof_mark("① 宿主装载（读 NSO / 重定位 / 强制接管）");
    { void loader_hash_log(void); loader_hash_log(); }   // DM_LOADHASH=1：装载结果的地址无关哈希（loader.c） | DM_LOADHASH=1: address-independent hash of the load result (loader.c)
}
static struct { int argc; char **argv; pthread_t th; _Atomic int started; } g_pre;
static void *preload_thread(void *a) { (void)a; host_preload(g_pre.argc, g_pre.argv); return NULL; }
static void *keyrace_thread(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < g_race_want; i++) if (pthread_key_create(&g_race_keys[g_race_n++], NULL)) break;
    return NULL;
}
// 提前起跑宿主装载。调用时机：进程里第一件事之一（iOS：App 的 main() 进 UIApplicationMain 之前），须先设好装载要用的环境变量
// （DM_ROMFS / DM_SAVE / DM_TEMP 等与 cwd——装载与预取起跑读它们）；参数须与随后 dm_main 的参数逐项相同（不同即报红退出）。
// 只能调一次；不调则 main 内同步装载，行为与调用前完全一致。不依赖 UIKit / Metal。
// Start host loading early. When to call: as one of the first things in the process (iOS: in the App's main() before entering UIApplicationMain); the environment variables loading needs must already be set
// (DM_ROMFS / DM_SAVE / DM_TEMP etc. and the cwd -- loading and the prefetch start read them); the arguments must be identical, item by item, to those of the later dm_main (a difference reports a failure and exits).
// May be called only once; if not called, loading is synchronous inside main and behavior is exactly as before this call existed. Does not depend on UIKit / Metal.
void dm_host_preload_start(int argc, char **argv) {
    if (atomic_exchange(&g_pre.started, 1)) return;
    g_pre.argc = argc; g_pre.argv = calloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) g_pre.argv[i] = strdup(argv[i]);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);
    if (pthread_create(&g_pre.th, &a, preload_thread, NULL)) die("dm_host_preload_start：起线程失败");
}
#endif   // !__ANDROID__
#ifndef __ANDROID__
#include <TargetConditionals.h>
int dm_app_bootstrap(int *argc, char ***argv);   // app_mac.m：包成 .app 双击起时备好环境与参数；命令行起原样 | app_mac.m: when packaged as an .app and started by double-click, prepare the environment and arguments; command-line starts are unchanged
int main(int argc, char **argv) {
#if TARGET_OS_OSX   // iOS 包也编这个文件（入口由外壳调），那边没有 app_mac.m | The iOS package compiles this file too (the entry is called by the shell); there is no app_mac.m on that side
    if (dm_app_bootstrap(&argc, &argv)) { }   // App 形态：挂存档之前先做一次 iCloud 同步（同 iOS 壳的启动次序）；取不到容器 / 测试存档时它自己记一行就返回 | App form: do one iCloud sync before mounting the save (same startup order as the iOS shell); when the container cannot be obtained / with a test save it logs one line and returns
#endif
    if (getenv("DM_PRELOAD")) dm_host_preload_start(argc, argv);   // Mac 上验证提前起跑的路径（iOS 由 App 的 main() 调） | Exercise the early-start path on Mac (on iOS it is called from the App's main())
    if (atomic_load(&g_pre.started)) {
        pthread_join(g_pre.th, NULL);
        int same = argc == g_pre.argc; for (int i = 0; same && i < argc; i++) same = !strcmp(argv[i], g_pre.argv[i]);
        if (!same) die("【红】dm_host_preload_start 的参数与 main 不同");
        void crash_thread_init(void); crash_thread_init();   // 信号备用栈是每线程的：装载在别的线程上装过，本线程补上 | The signal alternate stack is per thread: loading installed it on another thread, so add it for this thread
        LOG("宿主装载已在后台提前完成（dm_host_preload_start）");
    } else host_preload(argc, argv);
    front_run(guest_main);
    return 0;
}
#endif   // !__ANDROID__
static void obs_progtick(const uint64_t *r) { static int n; if (rtg_observation_logs() && (++n <= 3 || n % 3000 == 0)) fprintf(stderr, "[dm] [rtg-obs] 进度累加 #%d 场景=%#llx\n", n, (unsigned long long)r[0]); }   // 0x131920

#include "../host/dm.h"
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sys/uio.h>
#include <dlfcn.h>

static pthread_once_t signals_once = PTHREAD_ONCE_INIT;
static pthread_once_t alt_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t alt_key;
static _Atomic int signals_ready;
static int signal_breadcrumbs;
static _Atomic uint64_t frames, svc_calls[256], prof_ns[PROF_N], prof_bytes[PROF_N];
static void (*ui_subscribers[16])(const char *, const char *, const char *);
static size_t ui_count;
static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*write_hooks[8])(void *, size_t);
static size_t write_hook_count;
static pthread_mutex_t write_hook_lock = PTHREAD_MUTEX_INITIALIZER;
typedef int (*fault_hook)(void *, int, void *);
static _Atomic(fault_hook) fault_hooks[8];
static pthread_mutex_t fault_hook_lock = PTHREAD_MUTEX_INITIALIZER;
static const int handled_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT};
static struct sigaction previous_signals[4];
void dm_fault_hook_register(fault_hook fn) {
    if (!fn) die("Android null fault hook");
    pthread_mutex_lock(&fault_hook_lock);
    unsigned i;
    for (i = 0; i < sizeof fault_hooks / sizeof *fault_hooks; ++i) {
        fault_hook old = atomic_load_explicit(&fault_hooks[i], memory_order_acquire);
        if (old == fn) die("Android duplicate fault hook");
        if (!old) { atomic_store_explicit(&fault_hooks[i], fn, memory_order_release); break; }
    }
    if (i == sizeof fault_hooks / sizeof *fault_hooks) die("Android fault hook capacity");
    pthread_mutex_unlock(&fault_hook_lock);
}
/* Linux arm64's ESR context record describes the faulting access. Unknown or
 * malformed records must not be treated as write faults. */
static int fault_is_write(const ucontext_t *u) {
    const unsigned char *p = u->uc_mcontext.__reserved;
    size_t left = sizeof u->uc_mcontext.__reserved;
    while (left >= 8) {
        uint32_t magic, size; memcpy(&magic, p, 4); memcpy(&size, p + 4, 4);
        if (!magic || size < 8 || size > left || size % 16) return 0;
        if (magic == 0x45535201 && size >= 16) {
            uint64_t esr; memcpy(&esr, p + 8, 8);
            unsigned ec = (unsigned)(esr >> 26) & 63;
            return (ec == 0x24 || ec == 0x25) && (esr & (1u << 6));
        }
        p += size; left -= size;
    }
    return 0;
}
/* Mac crash.c 的 DM_SOFTGARBAGE 等价物:读缺页落在明显垃圾地址(错位虚表、
 * 非规范指针)时跳过错位调用指令、x0=0,线程继续。注入激活的链上有对象布局
 * 错位(scene=-1 下游),先让游戏活着往下走。DM_SOFTGARBAGE=N 上限,未设=关。
 * Equivalent of Mac crash.c's DM_SOFTGARBAGE: when a read fault lands on an
 * obviously garbage address, skip the offending instruction with x0=0 and let
 * the thread continue. DM_SOFTGARBAGE=N caps the hits; unset = off. */
static int android_softgarbage(siginfo_t *info, ucontext_t *u) {
    static int budget = -1, hits;
    if (budget < 0) { const char *e = getenv("DM_SOFTGARBAGE"); budget = e ? atoi(e) : 0; }
    if (budget <= hits) return 0;
    uint64_t fa = (uint64_t)info->si_addr, pc = u->uc_mcontext.pc, lr = u->uc_mcontext.regs[30];
    /* 26 号修：懒加载补偿路径（预读缺失时）的连锁空读——放宽到全地址捕获（读缺页一律
     * x0=0 跳过、执行缺页 pc←lr），先让游戏活过崩溃点到 DM_AUTOGAME 的 kick（~10s）。
     * 模块区/堆区的合法读不会缺页，缺页的必是垃圾指针——原地址过滤器反而漏掉第 5 个。
     * Fix 26: the lazy-load compensation path (when the pre-read is missing) chains NULL
     * reads; widen to catch-all (any read fault -> x0=0 skip, any exec fault -> pc<-lr)
     * so the game survives to the DM_AUTOGAME kick (~10 s). Legitimate reads of mapped
     * module/heap memory never fault - anything that faults is a garbage pointer; the old
     * address filter let the 5th one through and killed the process. */
    (void)0;
    hits++;
    if (fa >= pc - 8 && fa <= pc + 8) {   /* 执行缺页(跳到垃圾):按调用失败返回 pc←lr、x0=0 */
        u->uc_mcontext.regs[0] = 0;
        u->uc_mcontext.pc = lr;
        if ((hits & 63) == 0) LOG("[softgarbage] %d/%d：执行 %#llx（pc=%#llx）——pc←lr(%#llx)、x0=0", hits, budget,
            (unsigned long long)fa, (unsigned long long)pc, (unsigned long long)lr);
        return 1;
    }
    /* 循环检测：同一 pc 反复缺页（懒加载连锁的死循环模式）——第 4 次起放弃整个函数
     * pc←lr、x0=0（函数按失败返回），跳出循环让上游走错误分支。
     * Loop detection: the same pc faulting repeatedly (the lazy-load chain's tight
     * crash-loop pattern) - from the 4th hit abandon the whole function pc<-lr,
     * x0=0 (the function returns as failed), letting the caller take its error path. */
    static uint64_t last_pc; static int same_pc;
    if (pc == last_pc) same_pc++; else { last_pc = pc; same_pc = 0; }
    if (same_pc >= 3) {
        u->uc_mcontext.regs[0] = 0;
        u->uc_mcontext.pc = lr;
        if ((hits & 1023) == 0) LOG("[softgarbage] %d/%d：pc=%#llx 循环——pc←lr(%#llx) 放弃函数", hits, budget,
            (unsigned long long)pc, (unsigned long long)lr);
        return 1;
    }
    if (fa < 0x1000) {   /* v47：近空指针读（NULL+偏移）——按函数失败返回 pc←lr，不再跳指令留垃圾寄存器引发 0x0/0x10/0x20 级联 */
        u->uc_mcontext.regs[0] = 0x202;
        u->uc_mcontext.pc = lr;
        {   /* v63：近空读全量记 pc/lr 模块偏移（限 8 条）——定位被放弃的函数（帧循环停摆根因） */
            static int nn;
            if (++nn <= 8) {
                extern module_t g_mod[]; extern int g_nmod;
                char where[160] = "";
                for (int i = 0; i < g_nmod && strlen(where) < 120; i++) {
                    uint64_t b = (uint64_t)(uintptr_t)g_mod[i].base;
                    if (pc >= b && pc < b + g_mod[i].size)
                        snprintf(where + strlen(where), sizeof where - strlen(where), " %s+%#llx", g_mod[i].name, (unsigned long long)(pc - b));
                    if (lr >= b && lr < b + g_mod[i].size)
                        snprintf(where + strlen(where), sizeof where - strlen(where), " lr=%s+%#llx", g_mod[i].name, (unsigned long long)(lr - b));
                }
                LOG("[softgarbage] 近空读#%d addr=%#llx pc=%#llx%s", nn, (unsigned long long)fa, (unsigned long long)pc, where);
            }
        }
        return 1;
    }
    u->uc_mcontext.regs[0] = 0;
    u->uc_mcontext.pc = pc + 4;
    if ((hits & 63) == 0) LOG("[softgarbage] %d/%d：读 %#llx 于 pc=%#llx——跳过该指令、x0=0 继续", hits, budget,
        (unsigned long long)fa, (unsigned long long)pc);
    return 1;
}
static void chain_signal(int sig, siginfo_t *info, void *opaque) {
    for (unsigned i = 0; i < sizeof handled_signals / sizeof *handled_signals; ++i) {
        if (handled_signals[i] != sig) continue;
        struct sigaction old = previous_signals[i];
        if (old.sa_handler == SIG_IGN) return;
        if (old.sa_handler == SIG_DFL) {
            sigaction(sig, &old, NULL);
            syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sig);
            return;
        }
        if (old.sa_flags & SA_SIGINFO) old.sa_sigaction(sig, info, opaque);
        else old.sa_handler(sig);
        return;
    }
    _exit(128 + sig);
}
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) {
    if (!fn) die("Android null guest write hook");
    pthread_mutex_lock(&write_hook_lock);
    if (write_hook_count >= sizeof write_hooks / sizeof *write_hooks) die("Android guest write hook capacity");
    for (size_t i = 0; i < write_hook_count; ++i) if (write_hooks[i] == fn) die("Android duplicate guest write hook");
    write_hooks[write_hook_count++] = fn;
    pthread_mutex_unlock(&write_hook_lock);
}
void dm_guest_write_prepare(void *address, size_t length) {
    if (!address && length) die("Android invalid guest write buffer");
    void (*copy[8])(void *, size_t);
    pthread_mutex_lock(&write_hook_lock);
    size_t n = write_hook_count; memcpy(copy, write_hooks, n * sizeof *copy);
    pthread_mutex_unlock(&write_hook_lock);
    for (size_t i = 0; i < n; ++i) copy[i](address, length);
}

/* v22B：挂载链插入/移除观测（OBS_MNTINS=6 @0x4e1fe0、OBS_MNTREM=9 @0x4e1c60）。
 * v21 修正打印后发现：首故障时挂载链是正规空表（tail=first=head、count=0），而 nnMain 入口
 * dump（21fix-C）里有节点——16ms 内条目被全部移除且不经过任何 fs svc。此观测打每次
 * 插入/移除的序号、条目名与 head 三字段，锁定移除者。 */
/* v22B: mount-list insert/remove observation (OBS_MNTINS=6 @0x4e1fe0, OBS_MNTREM=9 @0x4e1c60).
 * After fixing the v21 print bug: at the first fault the list is a canonical empty list
 * (tail=first=head, count=0) while the nnMain-entry dump (21fix-C) had nodes - all entries
 * were removed within 16 ms with no fs svc in between. Log each insert/remove with sequence
 * number, entry name, and the three head fields to pin down the remover. */
static void mnt_ev_obs(const uint64_t *r, int is_ins) {
    static int n;
    if (n >= 400) return;
    n++;
    extern module_t g_mod[]; extern int g_nmod;
    module_t *mm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
    if (!mm) return;
    struct iovec l, rr;
    uint64_t entry = r[0], inner = 0, namep = 0, hb[3] = {0, 0, 0};
    char nm[24] = "";
    l.iov_base = &inner; l.iov_len = 8; rr.iov_base = (void *)(uintptr_t)(entry + 0x10); rr.iov_len = 8;
    if (entry && syscall(SYS_process_vm_readv, getpid(), &l, 1, &rr, 1, 0) == 8) {
        l.iov_base = &namep; l.iov_len = 8; rr.iov_base = (void *)(uintptr_t)(inner + 0x18); rr.iov_len = 8;
        if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &rr, 1, 0) == 8 && namep > 0x10000) {
            l.iov_base = nm; l.iov_len = 23; rr.iov_base = (void *)(uintptr_t)namep; rr.iov_len = 23;
            syscall(SYS_process_vm_readv, getpid(), &l, 1, &rr, 1, 0);
        }
    }
    uint64_t head = (uint64_t)(uintptr_t)((char *)mm->base + 0x10bd738);
    l.iov_base = hb; l.iov_len = 24; rr.iov_base = (void *)(uintptr_t)head; rr.iov_len = 24;
    int hok = syscall(SYS_process_vm_readv, getpid(), &l, 1, &rr, 1, 0) == 24;
    LOG("[mntobs] %s#%d entry=%#llx name=%.23s head tail=%#llx first=%#llx count=%#llx",
        is_ins ? "插入" : "移除", n, (unsigned long long)entry, nm,
        hok ? (unsigned long long)hb[0] : 0ULL, hok ? (unsigned long long)hb[1] : 0ULL,
        hok ? (unsigned long long)hb[2] : 0ULL);
}
void dm_mntins_obs(const uint64_t *r) { mnt_ev_obs(r, 1); }
void dm_mntrem_obs(const uint64_t *r) { mnt_ev_obs(r, 0); }

/* v22D：0x501eb0 字体 init 内的分叉检查点（1=入口 2=过重试环 3=过首 spawn 4=循环出口 5=重试 sleep 6=FS 上下文）。
 * Mac 在此函数里 spawn×30+插入×3；安卓零命中——定位从哪条调用起断流。 */
/* v22D: divergence checkpoints inside font init 0x501eb0 (1=entry 2=past retry loop 3=past first
 * spawn 4=loop exit 5=retry sleep 6=fs context). Mac spawns 30 threads and inserts 3 mounts here;
 * Android gets none -- pinpoint which call diverges. */
static void mntck(int k, uint64_t x0) { static int c[7]; if (k >= 1 && k <= 6 && ++c[k] <= 12) LOG("[mntchk%d] 命中 #%d x0=%#llx", k, c[k], (unsigned long long)x0); }
void dm_mntck1_obs(const uint64_t *r) { mntck(1, r[0]); }
void dm_mntck2_obs(const uint64_t *r) { mntck(2, r[0]); }
void dm_mntck3_obs(const uint64_t *r) { mntck(3, r[0]); }
void dm_mntck4_obs(const uint64_t *r) { mntck(4, r[0]); }   /* x0 = FS 上下文（虚调用 [ctx+0x28] 的 this） */
void dm_mntck5_obs(const uint64_t *r) { mntck(5, r[0]); }
void dm_mntck6_obs(const uint64_t *r) { mntck(6, r[0]); }

/* v22E：fs 打开链入口观测（57=0x4e3ad0 路径解析 x0=路径；58=0x4e36c0 OpenFile 包装 x2=路径）
 * ——HLE OpenFile 从未被调到：确认调用链在哪断的、游戏想开哪个文件。 */
static void fsarg_obs(const uint64_t *r, int which) {
    static int c[3];
    if (which < 1 || which > 2 || ++c[which] > 12) return;
    uint64_t p = which == 1 ? r[0] : r[2];
    char path[96] = "";
    struct iovec l, rr; l.iov_base = path; l.iov_len = 95; rr.iov_base = (void *)(uintptr_t)p; rr.iov_len = 95;
    if (p > 0x10000) syscall(SYS_process_vm_readv, getpid(), &l, 1, &rr, 1, 0);
    path[95] = 0;
    LOG("[fsarg%d] #%d x0=%#llx x1=%#llx x2=%#llx path=%.95s", which, c[which],
        (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], path);
}
void dm_fsarg1_obs(const uint64_t *r) { fsarg_obs(r, 1); }
void dm_fsarg2_obs(const uint64_t *r) { fsarg_obs(r, 2); }

/* v22H：ReadThread 启停控制（0x514e00）——x0=管理器，读 [x0+0x250] 状态（0=将建线程 1=运行 2/3=停） */
void dm_fontreg_obs(const uint64_t *r) {   /* v22Q：注册器入口——x0=id x1=源名串 x2=scale(s0) */
    static int c;
    if (++c > 12) return;
    char nms[64] = "";
    ssize_t got = -1;
    if (r[1] > 0x10000) {
        struct iovec l, rr; l.iov_base = nms; l.iov_len = 63; rr.iov_base = (void *)(uintptr_t)r[1]; rr.iov_len = 63;
        got = syscall(SYS_process_vm_readv, getpid(), &l, 1, &rr, 1, 0);
        nms[63] = 0;
    }
    LOG("[fontreg] #%d id=%#llx x1=%#llx 读=%zd 串=%.32s", c, (unsigned long long)r[0], (unsigned long long)r[1], got, nms);
}
void dm_lazyfont_obs(const uint64_t *r) {   /* v22N：字体懒加载器入口——x0=字体 id，读 [0x1106350+id*8] 名字 */
    static int c;
    if (++c > 12) return;
    extern module_t g_mod[]; extern int g_nmod;
    module_t *mm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
    uint64_t nm = 0, begin = 0;
    if (mm && r[0] < 64) {
        struct iovec l, rr; l.iov_base = &begin; l.iov_len = 8; rr.iov_base = (void *)(uintptr_t)((char *)mm->base + 0x1106350); rr.iov_len = 8;
        if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &rr, 1, 0) == 8 && begin > 0x10000) {
            l.iov_base = &nm; l.iov_len = 8; rr.iov_base = (void *)(uintptr_t)(begin + r[0]*8); rr.iov_len = 8;
            syscall(SYS_process_vm_readv, getpid(), &l, 1, &rr, 1, 0);
        }
    }
    char nms[48] = "";
    if (nm > 0x10000) {
        struct iovec l2, r2; l2.iov_base = nms; l2.iov_len = 47; r2.iov_base = (void *)(uintptr_t)nm; r2.iov_len = 47;
        syscall(SYS_process_vm_readv, getpid(), &l2, 1, &r2, 1, 0);
        nms[47] = 0;
    }
    LOG("[lazyfont] #%d id=%#llx name=%#llx 串=%.47s", c, (unsigned long long)r[0], (unsigned long long)nm, nms);
}
void dm_thrctl_obs(const uint64_t *r) {
    static int c;
    if (++c > 16) return;
    uint64_t st = 0xdeadbeef;
    struct iovec l, rr; l.iov_base = &st; l.iov_len = 4; rr.iov_base = (void *)(uintptr_t)(r[0] + 0x250); rr.iov_len = 4;
    if (r[0] > 0x10000) syscall(SYS_process_vm_readv, getpid(), &l, 1, &rr, 1, 0);
    LOG("[thrctl] #%d mgr=%#llx state250=%#llx", c, (unsigned long long)r[0], (unsigned long long)st);
}

static void fatal_signal(int sig, siginfo_t *info, void *opaque) {
    ucontext_t *u = opaque;
    if (sig != SIGSEGV || !fault_is_write(u)) {
        static const char entered[] = "ANDROID_NONWRITE_SIGNAL_ENTRY\n";
        write(STDERR_FILENO, entered, sizeof entered - 1);
    }
    /* Preserve a register-only breadcrumb even if the fault hook itself or a
     * prior ART handler encounters a second fault. No allocation or locks. */
    static _Atomic unsigned breadcrumbs;
    if(signal_breadcrumbs || !fault_is_write(u)) {
        unsigned count=atomic_fetch_add(&breadcrumbs,1);
        if(count<32 || !fault_is_write(u)) {
            char b[256];int n=snprintf(b,sizeof b,"ANDROID_SIGNAL_ENTRY sig=%d code=%d tid=%ld address=%p pc=%#llx lr=%#llx sp=%#llx guest=%d\n",
                sig,info->si_code,syscall(SYS_gettid),info->si_addr,(unsigned long long)u->uc_mcontext.pc,
                (unsigned long long)u->uc_mcontext.regs[30],(unsigned long long)u->uc_mcontext.sp,dm_android_thread_has_guest_context());
            if(n>0)write(STDERR_FILENO,b,(size_t)n<sizeof b?(size_t)n:sizeof b-1);
        }
    }
    if (sig == SIGILL) {   /* v22I：0x4e371c UDF 断点——抓 OpenFile 实参路径，跳过指令按调用失败继续 */
        char bp[160] = "";
        struct iovec lb, rb; lb.iov_base = bp; lb.iov_len = 159; rb.iov_base = (void *)(uintptr_t)u->uc_mcontext.regs[1]; rb.iov_len = 159;
        if (u->uc_mcontext.regs[1] > 0x10000) syscall(SYS_process_vm_readv, getpid(), &lb, 1, &rb, 1, 0);
        bp[159] = 0;
        LOG("[brkdump] SIGILL pc=%#llx x1=%#llx path=%.159s", (unsigned long long)u->uc_mcontext.pc,
            (unsigned long long)u->uc_mcontext.regs[1], bp);
        {   /* v22J：pathnorm 依赖的 libc GOT（strncmp/snprintf/memset/strnlen）运行时解析对照 */
            extern module_t g_mod[]; extern int g_nmod;
            module_t *mm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
            static const struct { uint64_t off; const char *nm; } cs[] = {
                {0xda50a0, "strncmp"}, {0xda5018, "snprintf"}, {0xda5010, "memset"}, {0xda51d0, "strnlen"}, {0xda5020, "memcpy"},
            };
            Dl_info di; void *hb = dladdr((void *)&fatal_signal, &di) ? di.dli_fbase : NULL;
            for (unsigned k = 0; k < sizeof cs / sizeof cs[0]; k++) {
                uint64_t v = 0;
                struct iovec lb2, rb2; lb2.iov_base = &v; lb2.iov_len = 8; rb2.iov_base = (void *)(uintptr_t)((char *)mm->base + cs[k].off); rb2.iov_len = 8;
                if (syscall(SYS_process_vm_readv, getpid(), &lb2, 1, &rb2, 1, 0) == 8)
                    LOG("[brklibc] %s got=%#llx got-host=%#llx dlsym=%#llx", cs[k].nm, (unsigned long long)v,
                        hb ? (unsigned long long)(v - (uint64_t)(uintptr_t)hb) : 0ULL,
                        (unsigned long long)(uintptr_t)dlsym(RTLD_DEFAULT, cs[k].nm));
            }
        }
        u->uc_mcontext.pc += 4;          /* 跳过 udf */
        u->uc_mcontext.regs[0] = 0x202;  /* nn::Result 错误码：按 OpenFile 失败继续（原崩溃链照常走） */
        return;
    }
    {   /* v20B：最早故障时刻 dump 挂载链（重置循环开始前——种子观测） | dump the mount list at the very first fault (before the retry loop resets it) */
        static int dumped;
        if (!dumped) {
            dumped = 1;
            extern module_t g_mod[]; extern int g_nmod;
            module_t *mm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
            if (mm) {
                struct iovec l, r;
                uint64_t buf[3];
                uint64_t head = (uint64_t)(uintptr_t)((char *)mm->base + 0x10bd738);
                l.iov_base = buf; l.iov_len = 24;
                r.iov_base = (void *)(uintptr_t)head; r.iov_len = 24;
                if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == 24) {
                    uint64_t cur = buf[1];
                    LOG("[mntdump] 首故障 head=%#llx tail[+0]=%#llx first[+8]=%#llx count[+0x10]=%#llx", (unsigned long long)head, (unsigned long long)buf[0], (unsigned long long)buf[1], (unsigned long long)buf[2]);
                    {   /* v27B：OpenFile GOT 是否被 HLE 强制接管（安卓从未见 [fs-open-debug]——怀疑走去了 SDK 实现） */
                        uint64_t got = 0;
                        l.iov_base = &got; l.iov_len = 8; r.iov_base = (void *)(uintptr_t)((char *)mm->base + 0xda52b0); r.iov_len = 8;
                        int gok = syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == 8;
                        void *hle = dlsym(RTLD_DEFAULT, "h_fs_OpenFile");
                        Dl_info di; void *hb = dladdr((void *)&fatal_signal, &di) ? di.dli_fbase : NULL;
                        LOG("[gotchk] OpenFile got=%#llx hle=%#llx hostbase=%p got-host=%#llx %s", (unsigned long long)got, (unsigned long long)(uintptr_t)hle, hb,
                            hb ? (unsigned long long)(got - (uint64_t)(uintptr_t)hb) : 0ULL,
                            gok && hle && got == (uint64_t)(uintptr_t)hle ? "接管OK" : (gok ? "未接管!!" : "读失败"));
                    }
                    {   /* v22L：字体链 GOT 槽运行时值（LoadFont SDK 实现地址） */
                        static const uint64_t gots[] = {0xda5338, 0xda52b0, 0xda5018, 0xda50a0, 0xda5010, 0xda50e8};
                        {   /* v22M：SDK 模块 LoadFont 内部 PLT 槽（0xb590e0）运行时值——静态 GOT 全零，须运行时读 */
                            extern module_t g_mod[]; extern int g_nmod;
                            module_t *sm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) sm = &g_mod[i];
                            if (sm) {
                                uint64_t v = 0;
                                static const uint64_t sslots[] = {0xb590e0, 0xb59320};
                                for (unsigned k = 0; k < 2; k++) {
                                    v = 0;
                                    l.iov_base = &v; l.iov_len = 8; r.iov_base = (void *)(uintptr_t)((char *)sm->base + sslots[k]); r.iov_len = 8;
                                    if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == 8)
                                        LOG("[sdkgot] 槽%#llx=%#llx (sdk+%#llx)", (unsigned long long)sslots[k], (unsigned long long)v, (unsigned long long)(v - (uint64_t)(uintptr_t)sm->base));
                                }
                            }
                        }
                        for (unsigned k = 0; k < sizeof gots / sizeof gots[0]; k++) {
                            uint64_t v = 0;
                            l.iov_base = &v; l.iov_len = 8; r.iov_base = (void *)(uintptr_t)((char *)mm->base + gots[k]); r.iov_len = 8;
                            if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == 8)
                                {   extern module_t g_mod[]; extern int g_nmod;
                                    uint64_t sb = 0;
                                    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) sb = (uint64_t)(uintptr_t)g_mod[i].base;
                                    LOG("[gotdump] slot+%#llx=%#llx main=%#llx sdk=%#llx sdk_off=%#llx", (unsigned long long)gots[k], (unsigned long long)v,
                                        (unsigned long long)(uintptr_t)mm->base, (unsigned long long)sb, (unsigned long long)(v - sb));
                                }
                        }
                    }
                    {   /* v22K：FS 上下文虚表 +0x28 实现（字体 LoadFont 的实际打开入口） */
            extern module_t g_mod[]; extern int g_nmod;
            module_t *mm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
            uint64_t ctxp = 0, vt = 0, impl = 0;
            struct iovec lb3, rb3;
            lb3.iov_base = &ctxp; lb3.iov_len = 8; rb3.iov_base = (void *)(uintptr_t)((char *)mm->base + 0x10bc228); rb3.iov_len = 8;
            if (mm && syscall(SYS_process_vm_readv, getpid(), &lb3, 1, &rb3, 1, 0) == 8 && ctxp > 0x10000) {
                lb3.iov_base = &vt; rb3.iov_base = (void *)(uintptr_t)ctxp; rb3.iov_len = 8;
                if (syscall(SYS_process_vm_readv, getpid(), &lb3, 1, &rb3, 1, 0) == 8 && vt > 0x10000) {
                    lb3.iov_base = &impl; rb3.iov_base = (void *)(uintptr_t)(vt + 0x28); rb3.iov_len = 8;
                    syscall(SYS_process_vm_readv, getpid(), &lb3, 1, &rb3, 1, 0);
                    LOG("[ctxvt] ctx=%#llx vt=%#llx vt+0x28=%#llx (main+%#llx)", (unsigned long long)ctxp, (unsigned long long)vt, (unsigned long long)impl,
                        (unsigned long long)(impl - (uint64_t)(uintptr_t)mm->base));
                }
            }
        }

                    {   /* v22F：0x4e36c0 链路上的关键全局（路径规范化 fn [0x10bd808] / 扩展表 [0x10bd810] / TLS 槽号 [0x10bc220] / 兜底上下文 [0x10bc228]） */
                        {   /* v22S：前缀链 [0xd96030]→[x]→串——snprintf 无输出的嫌疑源 */
                            uint64_t p1 = 0, p2 = 0;
                            l.iov_base = &p1; l.iov_len = 8; r.iov_base = (void *)(uintptr_t)((char *)mm->base + 0xd96030); r.iov_len = 8;
                            if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == 8) {
                                l.iov_base = &p2; l.iov_len = 8; r.iov_base = (void *)(uintptr_t)p1; r.iov_len = 8;
                                syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0);
                                char pf[32] = "";
                                if (p2 > 0x10000) { l.iov_base = pf; l.iov_len = 31; r.iov_base = (void *)(uintptr_t)p2; r.iov_len = 31; syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0); pf[31] = 0; }
                                LOG("[gdump] 前缀链 [0xd96030]=%#llx [x]=%#llx 串=%.31s", (unsigned long long)p1, (unsigned long long)p2, pf);
                            }
                        }
                        {   /* v22R：静态名字数组 0xda6008 前 8 项运行时值——镜像存裸偏移靠 RELR 重定位；若运行时仍裸值/零 = 重定位没应用 */
                            uint64_t a8[8] = {0};
                            l.iov_base = a8; l.iov_len = 64; r.iov_base = (void *)(uintptr_t)((char *)mm->base + 0xda6008); r.iov_len = 64;
                            if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == 64) {
                                char line[256]; int ln = 0;
                                for (int q = 0; q < 8; q++) ln += snprintf(line + ln, sizeof line - ln, "%#llx ", (unsigned long long)a8[q]);
                                LOG("[gdump] 静态名数组[0..7]=%s (基址=%#llx)", line, (unsigned long long)(uintptr_t)mm->base);
                            }
                        }
                        {   /* v22P：字体分类过滤链全槽（0x11061d8 +0x18/+0x30/+0x48/+0x60/+0x78）——命中 type(1-6) 即跳过注册 */
                            static const uint64_t foffs[] = {0x11061d8, 0x11061f0, 0x1106208, 0x1106220, 0x1106238, 0x1106250};
                            for (unsigned k = 0; k < 6; k++) {
                                uint32_t fv = 0;
                                l.iov_base = &fv; l.iov_len = 4; r.iov_base = (void *)(uintptr_t)((char *)mm->base + foffs[k]); r.iov_len = 4;
                                if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == 4)
                                    LOG("[gdump] font_filter+%#llx=%#x", (unsigned long long)(foffs[k] - 0x11061d8), fv);
                            }
                        }
                        static const struct { uint64_t off; const char *nm; } gs[] = {
                            {0x10bd808, "pathnorm_fn"}, {0x10bd810, "ext_tbl"}, {0x10bc220, "tls_slot"}, {0x10bc228, "ctx_fallback"}, {0x10bd7d0, "pathobj"}, {0x10bd7a0, "fs_lock"}, {0xd9da60, "font_eng"}, {0xd9da58, "font_x"}, {0x1106350, "fontnames"}, {0x11061d8, "font_filter"}, {0xd96020, "font_src"}, {0xd9da70, "font_desc"},
                        };
                        for (unsigned k = 0; k < sizeof gs / sizeof gs[0]; k++) {
                            uint64_t v = 0;
                            l.iov_base = &v; l.iov_len = 8; r.iov_base = (void *)(uintptr_t)((char *)mm->base + gs[k].off); r.iov_len = 8;
                            if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == 8) {
                                LOG("[gdump] %s[%#llx]=%#llx", gs[k].nm, (unsigned long long)gs[k].off, (unsigned long long)v);
                                if (gs[k].off == 0x1106350 && v > 0x10000) {   /* v22O：fontnames vector {begin,end,cap}——读 end + 前 16 项 + 第一项字符串 */
                                    uint64_t endv = 0;
                                    l.iov_base = &endv; l.iov_len = 8; r.iov_base = (void *)(uintptr_t)((char *)mm->base + 0x1106358); r.iov_len = 8;
                                    syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0);
                                    LOG("[gdump] fontnames begin=%#llx end=%#llx 项数=%llu", (unsigned long long)v, (unsigned long long)endv, (unsigned long long)((endv - v) / 8));
                                    uint64_t f16[16] = {0};
                                    l.iov_base = f16; l.iov_len = 128; r.iov_base = (void *)(uintptr_t)v; r.iov_len = 128;
                                    if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == 128) {
                                        char line[256]; int ln = 0;
                                        for (int q = 0; q < 16; q++) ln += snprintf(line + ln, sizeof line - ln, "%#llx ", (unsigned long long)f16[q]);
                                        LOG("[gdump] fontnames[0..15]=%s", line);
                                        if (f16[0] > 0x10000) {   /* 第一项字符串 */
                                            char nm[64] = "";
                                            l.iov_base = nm; l.iov_len = 63; r.iov_base = (void *)(uintptr_t)f16[0]; r.iov_len = 63;
                                            if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) > 0) LOG("[gdump] fontnames[0]串=%s", nm);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    for (int k = 0; k < 6 && cur && cur != head; k++) {
                        l.iov_len = 24; r.iov_base = (void *)(uintptr_t)cur; r.iov_len = 24;
                        if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) != 24) { LOG("[mntdump] 条目%d 读失败", k); break; }
                        uint64_t nxt = buf[1], entry = buf[2];
                        uint64_t inner = 0;
                        l.iov_len = 8; r.iov_base = (void *)(uintptr_t)(entry + 0x10); r.iov_len = 8;
                        if (entry && syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == 8) inner = buf[0];
                        uint64_t namep = 0;
                        l.iov_len = 8; r.iov_base = (void *)(uintptr_t)(inner + 0x18); r.iov_len = 8;
                        if (inner && syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == 8) namep = buf[0];
                        char nm[32] = "";
                        if (namep > 0x10000) { l.iov_len = 31; r.iov_base = (void *)(uintptr_t)namep; r.iov_len = 31;
                            if (syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) > 0) { memcpy(nm, buf, 31); nm[31] = 0; } }
                        LOG("[mntdump] 条目%d node=%#llx entry=%#llx inner=%#llx name=%s",
                            k, (unsigned long long)cur, (unsigned long long)entry, (unsigned long long)inner, nm);
                        if (!nxt || nxt == cur) break;
                        cur = nxt;
                    }
                } else LOG("[mntdump] head 读失败");
            }
        }
    }
    if ((sig == SIGSEGV || sig == SIGBUS) && info->si_code > 0) {
        int is_write = fault_is_write(u);
        for (unsigned i = 0; i < sizeof fault_hooks / sizeof *fault_hooks; ++i) {
            fault_hook fn = atomic_load_explicit(&fault_hooks[i], memory_order_acquire);
            if (!fn) break;
            if (fn(info->si_addr, is_write, opaque)) return;
        }
        if (!is_write && android_softgarbage(info, u)) return;   /* v19 验证：写缺页不跳（排查软降级跳写破坏挂载链） | v19 test: no write-fault skipping (investigate softgarbage breaking the mount list) */
    }
    /* ART uses SIGSEGV for managed implicit checks. Claim only threads that
     * actually own a registered guest context; preserve the previous handler. */
    if (!dm_android_thread_has_guest_context()) { chain_signal(sig, info, opaque); return; }
    char line[256];
    int n = snprintf(line, sizeof line,
        "ANDROID_FATAL signal=%d tid=%ld address=%p pc=%#llx lr=%#llx sp=%#llx\n",
        sig, syscall(SYS_gettid), info->si_addr,
        (unsigned long long)u->uc_mcontext.pc, (unsigned long long)u->uc_mcontext.regs[30],
        (unsigned long long)u->uc_mcontext.sp);
    if (n > 0) write(STDERR_FILENO, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    for(unsigned i=0;i<31;i+=3) {
        n=snprintf(line,sizeof line,"ANDROID_REG x%u=%#llx x%u=%#llx x%u=%#llx\n",i,(unsigned long long)u->uc_mcontext.regs[i],
            i+1,(unsigned long long)u->uc_mcontext.regs[i+1<31?i+1:30],i+2,(unsigned long long)u->uc_mcontext.regs[i+2<31?i+2:30]);
        if(n>0)write(STDERR_FILENO,line,(size_t)n<sizeof line?(size_t)n:sizeof line-1);
    }
    for (int i = 0; i < g_nmod; ++i) {
        n = snprintf(line, sizeof line, "ANDROID_MODULE %s base=%p pc_offset=%#llx lr_offset=%#llx\n",
            g_mod[i].name, g_mod[i].base, (unsigned long long)(u->uc_mcontext.pc - (uintptr_t)g_mod[i].base),
            (unsigned long long)(u->uc_mcontext.regs[30] - (uintptr_t)g_mod[i].base));
        if (n > 0) write(STDERR_FILENO, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    }
    dm_backtrace(u->uc_mcontext.regs[29]);
    _exit(128 + sig);
}
static void setup_signals(void) {
    signal_breadcrumbs=getenv("DM_RUNTIME_DIAGNOSTICS")!=NULL;
    struct sigaction sa = {.sa_sigaction = fatal_signal, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    for (unsigned i = 0; i < sizeof handled_signals / sizeof *handled_signals; ++i)
        if (sigaction(handled_signals[i], &sa, &previous_signals[i])) die("Android crash signal setup errno=%d", errno);
    atomic_store(&signals_ready, 1);
}
static void destroy_alt(void *p) {
    stack_t off = {.ss_flags = SS_DISABLE};
    sigaltstack(&off, NULL);
    free(p);
}
static void setup_alt_key(void) {
    if (pthread_key_create(&alt_key, destroy_alt)) die("Android crash alt-stack key");
}
void crash_thread_init(void) {
    pthread_once(&signals_once, setup_signals);
    pthread_once(&alt_key_once, setup_alt_key);
    if (pthread_getspecific(alt_key)) return;
    void *p = malloc(128 * 1024);
    if (!p) die("Android signal stack allocation");
    stack_t alt = {.ss_sp = p, .ss_size = 128 * 1024};
    if (sigaltstack(&alt, NULL) || pthread_setspecific(alt_key, p)) die("Android signal stack attachment");
}
int dm_android_diagnostics_ready(void) {
    if (!atomic_load(&signals_ready)) return 0;
    stack_t alt;
    if (sigaltstack(NULL, &alt) || (alt.ss_flags & SS_DISABLE) || alt.ss_size < 128 * 1024) return 0;
    return alt.ss_sp == pthread_getspecific(alt_key);
}
int dm_android_diagnostics_flush(void) {
    if (fflush(stderr) || fflush(stdout)) return errno ? errno : EIO;
    return 0;
}
void crash_watch_frame(void) { atomic_fetch_add(&frames, 1); }
__attribute__((weak)) void dm_android_front_presented(uint64_t frame);
void dm_android_nvn_presented(uint64_t frame) {
    dm_guest_frame_tick(); crash_watch_frame();
    if (dm_android_front_presented) dm_android_front_presented(frame);
}
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) {
    if (id < 0 || id >= PROF_N) die("Android invalid profile counter %d", id);
    atomic_fetch_add(&prof_ns[id], ns); atomic_fetch_add(&prof_bytes[id], bytes);
}
void dm_prof_mark(const char *name) { LOG("ANDROID_PROFILE %s time_ns=%llu", name, now_ns()); }
void uimsg_subscribe(void (*fn)(const char *, const char *, const char *)) {
    pthread_mutex_lock(&ui_lock);
    if (ui_count >= sizeof ui_subscribers / sizeof *ui_subscribers) die("Android UI subscriber capacity");
    ui_subscribers[ui_count++] = fn;
    pthread_mutex_unlock(&ui_lock);
}
void dm_android_uimsg(const char *category, const char *action, const char *argument) {
    void (*copy[16])(const char *, const char *, const char *);
    pthread_mutex_lock(&ui_lock);
    size_t n = ui_count; memcpy(copy, ui_subscribers, n * sizeof *copy);
    pthread_mutex_unlock(&ui_lock);
    for (size_t i = 0; i < n; ++i) copy[i](category, action, argument);
}
void *hlecount_wrap(const char *kind, const char *name, void *fn) {
    (void)kind; (void)name;
    if (getenv("DM_HLECOUNT")) die("Android per-import counting thunks not implemented");
    return fn;
}
void hlecount_seal(void) {}
void hlecount_svc(int n) { if (n >= 0 && n < 256) atomic_fetch_add(&svc_calls[n], 1); }
uint64_t dm_android_svc_count(int n) { return n >= 0 && n < 256 ? atomic_load(&svc_calls[n]) : 0; }
uint64_t svc_host_call(int n, uint64_t *regs) {
    (void)regs; die("Android build-specific host call %#x requires verified version mapping", n);
}
void dm_backtrace(uint64_t fp) {
    /* dm_thread_enter switches to an SDK-owned alias stack, outside Bionic's
     * pthread stack bounds. Self process_vm_readv validates each read in the
     * kernel and cannot cause a nested fault on an invalid guest frame. */
    uintptr_t frame=(uintptr_t)fp&0x00ffffffffffffffULL;
    LOG("ANDROID_BACKTRACE fp=%#llx SDK_owned_stack=true",fp);
    for(unsigned i=0;i<32 && frame && !(frame&15);++i){
        uint64_t pair[2];struct iovec local={pair,sizeof pair},remote={(void *)frame,sizeof pair};
        if(syscall(SYS_process_vm_readv,getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof pair)break;
        module_t *m=module_of((void *)(uintptr_t)pair[1]);
        LOG("ANDROID_CALLER %u lr=%#llx module=%s offset=%#llx",i,pair[1],m?m->name:"host",
            m?pair[1]-(uint64_t)(uintptr_t)m->base:0);
        uintptr_t next=(uintptr_t)pair[0]&0x00ffffffffffffffULL;if(next<=frame)break;frame=next;
    }
}
uint64_t front_game_res(int docked) { return ((uint64_t)(docked ? 900 : 720) << 32) | (docked ? 1600 : 1280); }
uint64_t dm_game_res(int docked) { return front_game_res(docked); }

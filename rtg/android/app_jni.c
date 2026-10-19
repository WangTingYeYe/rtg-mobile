#include "../host/dm.h"
#include "services_platform.h"
#include "services_sdk.h"
#include "services_fs.h"
#include "romfs_image.h"
#include "input_backend.h"
#include "nvn_platform_clock.h"
#include <jni.h>
#include <android/native_window_jni.h>
#include <stdatomic.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <math.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
extern void guest_thread_init(void);
extern void crash_thread_init(void);
extern uint32_t svc_thread_main_init(void);
extern int dm_android_diagnostics_flush(void);
extern void dm_vk_set_shader_root(const char *);
extern void dm_vk_set_window(void *);
extern void dm_vk_set_display_hz(int);
extern int dm_vk_attachment_applied_flags(void);
extern void dm_gfx_suspend(int);
extern void dm_audio_suspend(int);
extern void dm_rtg_audio_suspend(int);

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *native_window;
static char app_files[PATH_MAX], port_root[PATH_MAX], locale_tag[32];
static char phase[96] = "等待游戏文件";
static _Atomic int configured, started, sdk_ready, main_entered, returned;
static _Atomic int input_route_ready, input_bound;
static _Atomic uint64_t presented_frames;
static dm_android_window_state window_state = {.focus=3, .thermal_status=-1};
static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
static char report_file[PATH_MAX], report_temp[PATH_MAX];
static void path(char *out, size_t cap, const char *base, const char *leaf) {
    if (snprintf(out, cap, "%s/%s", base, leaf) >= (int)cap) die("Android private path too long");
}
/* A fixed scale is an explicit diagnostic experiment. An absent marker keeps
 * the original 1.0 scale; reject truncated values, NaN/Inf, and extra tokens. */
static int configure_render_scale(void) {
    char marker[PATH_MAX], text[64], value[32];
    path(marker,sizeof marker,port_root,"diagnostics/render-scale.txt");
    unsetenv("DM_3D_SCALE");
    FILE *f=fopen(marker,"r");
    if (!f) return errno==ENOENT?0:errno;
    size_t n=fread(text,1,sizeof text-1,f);
    int error=ferror(f)?EIO:0;
    int trailing=fgetc(f);
    if (ferror(f)) error=EIO;
    if (fclose(f) && !error) error=errno?errno:EIO;
    if (error) return error;
    if (trailing!=EOF || memchr(text,'\0',n)) return EINVAL;
    text[n]='\0';
    char *start=text, *end;
    while (isspace((unsigned char)*start)) start++;
    errno=0; float scale=strtof(start,&end);
    if (end==start || errno==ERANGE || !isfinite(scale) || scale<0.70f || scale>1.0f) return EINVAL;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return EINVAL;
    snprintf(value,sizeof value,"%.9g",(double)scale);
    return setenv("DM_3D_SCALE",value,1)?errno:0;
}
static void status_text(char *text,size_t capacity) {
    snprintf(text,capacity,"{\"phase\":\"%s\",\"sdk_ready\":%s,\"nnMain_entered\":%s,\"presented_frames\":%llu,\"returned\":%s,\"renderer_flags\":%d}",phase,
        atomic_load(&sdk_ready)?"true":"false",atomic_load(&main_entered)?"true":"false",
        (unsigned long long)atomic_load(&presented_frames),atomic_load(&returned)?"true":"false",dm_vk_attachment_applied_flags());
}
static void persist_status(void) {
    if (!report_file[0]) return;
    pthread_mutex_lock(&report_lock);
    char text[384];pthread_mutex_lock(&state_lock);status_text(text,sizeof text);pthread_mutex_unlock(&state_lock);
    int fd=open(report_temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600),error=0;
    if(fd<0)error=errno;
    else {size_t n=strlen(text),sent=0;while(sent<n){ssize_t k=write(fd,text+sent,n-sent);if(k<0&&errno==EINTR)continue;if(k<=0){error=errno?errno:EIO;break;}sent+=(size_t)k;}
        if(!error&&fsync(fd))error=errno;if(close(fd)&&!error)error=errno;
        if(!error&&rename(report_temp,report_file))error=errno;}
    if(error)LOG("ANDROID_STATUS_WRITE_FAILED errno=%d",error);
    pthread_mutex_unlock(&report_lock);
}
static void phase_set(const char *text) {
    pthread_mutex_lock(&state_lock); snprintf(phase, sizeof phase, "%s", text); pthread_mutex_unlock(&state_lock);
    LOG("ANDROID_GAME_STAGE %s", text);
    persist_status();
}
void *dm_android_native_window(void) {
    pthread_mutex_lock(&state_lock); ANativeWindow *w = native_window; pthread_mutex_unlock(&state_lock);
    return w; /* Root owns an acquired reference until graphics are suspended/drained. */
}
void dm_android_front_presented(uint64_t frame) {
    atomic_store(&presented_frames, frame);
    if (frame == 1) phase_set("游戏画面已呈现");
}
static int suspend_graphics(int on) { dm_gfx_suspend(on); return 0; }
static int pause_audio(int on) {dm_audio_suspend(on);dm_rtg_audio_suspend(on);return 0;}
static int input_source_ready(void *opaque) {(void)opaque;return atomic_load(&input_route_ready)&&dm_android_native_window()!=NULL;}
static void *required(const char *name) {
    void *fn = guest_sym(name); if (!fn) die("Missing original startup export %s", name); return fn;
}
static void call_original(const char *name) {
    LOG("ANDROID_ORIGINAL_CALL %s", name); ((void (*)(void))required(name))();
    LOG("ANDROID_ORIGINAL_RETURN %s", name);
}
static void *game_thread(void *unused) {
    (void)unused;
    crash_thread_init();
    char debug_hold[PATH_MAX];path(debug_hold,sizeof debug_hold,port_root,"diagnostics/debug-hold");
    if(!access(debug_hold,F_OK)) { LOG("ANDROID_DEBUG_HOLD before original startup");raise(SIGSTOP); }
    char modules[PATH_MAX], rom[PATH_MAX], save[PATH_MAX], temp[PATH_MAX], profile[PATH_MAX];
    path(modules,sizeof modules,port_root,"modules"); path(save,sizeof save,port_root,"save");
    path(temp,sizeof temp,port_root,"temp"); path(profile,sizeof profile,port_root,"profile");
    path(modules,sizeof modules,port_root,"modules");
    // 节奏天国:RomFS 是解包目录(与 Mac 相同的 DM_ROMFS 目录挂载),不是 D2R 的整包镜像。
    // 优先 app 外部私有目录(adb push 可直达):/sdcard/Android/data/<pkg>/files/rhythm-tengoku/romfs;
    // 兜底用内部 files 下的 rhythm-tengoku/romfs。
    // Rhythm Heaven: the RomFS is an unpacked directory (same directory mount as the
    // Mac host's DM_ROMFS), not D2R's packaged image. Prefer the app's external
    // private dir (reachable by adb push): /sdcard/Android/data/<pkg>/files/rhythm-tengoku/romfs;
    // fall back to the internal files dir.
    { const char *ext = getenv("RTG_ROMFS_DIR");   // nativeConfigure 里按包名推导后写入
      if (ext && access(ext,F_OK)==0) snprintf(rom,sizeof rom,"%s",ext);
      else path(rom,sizeof rom,port_root,"romfs"); }
    phase_set("正在读取游戏资源");
    if (access(rom,F_OK)!=0) die("RomFS directory absent: %s",rom);
    int error = 0;   // 平台 bind/register 的错误码(下方沿用原变量) | error code for platform bind/register below
    if (dm_android_fs_ensure_directory(save) || dm_android_fs_ensure_directory(temp) || dm_android_fs_ensure_directory(profile)) die("Android private directories: save=%s temp=%s profile=%s errno=%d",save,temp,profile,errno);
    setenv("DM_ROMFS",rom,1); setenv("DM_SAVE",save,1); setenv("DM_TEMP",temp,1);
    // Game-specific startup defaults. Keep dirty tracking enabled: Android's
    // fault handler routes protected guest writes through the tracker, allowing
    // current uniforms without decoding/re-uploading every texture each draw.
    setenv("DM_DESWZ","1",1);
    setenv("DM_TYPEREG","1",1);
    unsetenv("DM_DIRTYOFF");
    /* A successful boot must not depend on skipping faulting guest instructions.
     * Valid mapping/write-protection faults have their own handlers. */
    setenv("DM_SOFTGARBAGE","0",1);
    setenv("DM_MEM_CAP_GB","44",1);
    /* Normal play owns every menu choice; never inject the bring-up script. */
    unsetenv("DM_AUTOGAME");
    setenv("DM_PSO_MANIFEST","off",1);
    setenv("DM_LANG","zh-Hans",1);   /* v42：语言锁 zh-Hans（Mac 配方缺的一项）——安卓原走设备 locale=en，游戏语言表越界 → out_of_range Break；Mac 默认 zh-Hans 加载中文字体 */
    /* Per-syscall logging is useful during startup diagnosis but adds work to
     * every frame. Keep it opt-in, including a writable path for read traces. */
    unsetenv("DM_FS");unsetenv("DM_THR");unsetenv("DM_FSLOG");
    char verbose_marker[PATH_MAX];path(verbose_marker,sizeof verbose_marker,port_root,"diagnostics/verbose-host");
    if(!access(verbose_marker,F_OK)){
        setenv("DM_FS","1",1);setenv("DM_THR","1",1);
        char fs_trace[PATH_MAX];path(fs_trace,sizeof fs_trace,port_root,"diagnostics/fslog.tsv");
        setenv("DM_FSLOG",fs_trace,1);
    }
    { const char *dbg = getenv("RTG_DEBUG");
      if (dbg && strstr(dbg, "nvn")) setenv("DM_NVN_LOG", "1", 1); }   // RTG_DEBUG=nvn 开 nvn 日志 | enables nvn logging
    // 共享字体:Mac 路径在安卓不存在——用系统 CJK 字体(游戏文字渲染的兜底)。
    // Shared font: the Mac path does not exist on Android - use a system CJK font.
    { /* v62：共享字体必须带 CJK/符号字形——0x5017d0 系统字体路径用 CheckGlyphExist(U+25A1□/平假名)
       * 逐项探测，DroidSans 只有拉丁（实测 cmap 无 U+25A1/あ/中）→ 全 false → 字体工厂 NULL →
       * 0x5029c0 空指针崩。改按优先级选 NotoSansCJK，DroidSans 系只作兜底。 */
      char font_asset[PATH_MAX]; path(font_asset,sizeof font_asset,port_root,"fonts/ArialUnicode.ttf");
      const char *fonts[] = {font_asset,"/system/fonts/NotoSansCJK-Regular.ttc","/system/fonts/NotoSansCJK.ttc","/system/fonts/DroidSansFallback.ttf","/system/fonts/DroidSans.ttf"};
      const char *env_font = getenv("DM_SHARED_FONT");
      int have = 0;
      if (env_font && access(env_font,R_OK)==0) have = 1;
      else for (unsigned i = 0; i < sizeof fonts / sizeof fonts[0]; i++) if (access(fonts[i],R_OK)==0) { setenv("DM_SHARED_FONT",fonts[i],1); have = 1; break; }
      if (!have) LOG("Android 系统字体未找到,游戏文字渲染可能缺字"); }
    { char msl[PATH_MAX]; path(msl,sizeof msl,port_root,"mslcache");
      if (!access(rom,F_OK)) setenv("DM_MSL_DIR",msl,1); }
    char private_vulkan[PATH_MAX];path(private_vulkan,sizeof private_vulkan,port_root,"diagnostics/private-vulkan");
    if(access(private_vulkan,F_OK)==0)setenv("DM_PRIVATE_VULKAN","turnip",1);
    dm_android_platform_config config = {.mode=DM_ANDROID_HOST_ACTIVITY,
        .profile_directory=profile, .locale=locale_tag, .locale_source="Activity.Configuration",
        /* Selected add-on set is explicitly empty. No unverified DLC is exposed. */
        .content_manifest_verified=1, .graphics_present=1, .audio_present=1,
        .suspend_and_drain_graphics=suspend_graphics, .pause_audio=pause_audio, .flush_diagnostics=dm_android_diagnostics_flush};
    if ((error=dm_android_platform_bind(&config)) || (error=dm_android_platform_register_services())) die("Android platform bind/register error=%d",error);
    pthread_mutex_lock(&state_lock); dm_android_window_state w=window_state; pthread_mutex_unlock(&state_lock);
    if ((error=dm_android_platform_update_window(&w))) die("Android initial window state error=%d",error);
    dm_android_service_register(DM_ANDROID_SERVICE_FS,dm_android_fs_initialize,NULL);
    phase_set("正在装载游戏");
    const char *names[]={"main","sdk"}; load_modules(modules,names,2); guest_thread_init();
    uint32_t handle=svc_thread_main_init();
    phase_set("正在初始化游戏"); call_original("_init_libc0");
    LOG("ANDROID_ORIGINAL_CALL nnosInitialize handle=%u",handle);
    ((void (*)(uint64_t,uint64_t))required("nnosInitialize"))(handle,0);
    LOG("ANDROID_ORIGINAL_RETURN nnosInitialize");
    call_original("_init_libc1"); call_original("nninitInitializeSdkModule");
    call_original("nninitStartup"); call_original("_init_libc2"); atomic_store(&sdk_ready,1);
    // 25号修终极发现：Mac 全自动链（门挂接/类型表补注册/DM_AUTOGAME 转场）在 main.c 的
    // Mac 专属 guest_main 里启动——安卓走本函数自己的引导、从未调用它们！情景引导因此
    // 没跑（[+0x3cc8] 标题就绪位不置 → ReadThread 不创建 → 预读为零 → 懒加载崩溃）。
    // 在 run_module_inits 前补齐 Mac 同款启动块（main.c 231..306 的等价物）。
    // Ultimate fix: the Mac auto chain (gate attach / type-table bootstrap / DM_AUTOGAME
    // transition) starts inside main.c's Mac-only guest_main; Android's own bootstrap here
    // never called any of it, so the situation bootstrap never ran ([+0x3cc8] title-ready
    // flag never set -> ReadThread never created -> zero pre-read -> lazy-load crash).
    // Mirror the Mac startup block (equivalent of main.c 231..306) before run_module_inits.
    LOG("AUTOCHAIN_BLOCK_ENTER");
    {
        extern int gate_obs_present(int); extern void gate_obs_set(int, void (*)(const uint64_t *));
        extern void (*const g_blr_fns[90])(const uint64_t *);   // main.c 的 30..119 号包装表 | the 30..119 wrapper table from main.c
        for (int id = 30; id < 120; id++) if (gate_obs_present(id)) gate_obs_set(id, g_blr_fns[id - 30]);
        { extern void scenereq_obs(const uint64_t *); if (gate_obs_present(OBS_SCENEREQ)) gate_obs_set(OBS_SCENEREQ, scenereq_obs); }
        { extern void obs_mgrentry(const uint64_t *); if (gate_obs_present(OBS_MGRENTRY)) gate_obs_set(OBS_MGRENTRY, obs_mgrentry); }
        { extern void obs_gate6(const uint64_t *); if (gate_obs_present(OBS_GATE6)) gate_obs_set(OBS_GATE6, obs_gate6); }
        { extern void dm_mntins_obs(const uint64_t *); if (gate_obs_present(OBS_MNTINS)) gate_obs_set(OBS_MNTINS, dm_mntins_obs); }
        { extern void dm_mntrem_obs(const uint64_t *); if (gate_obs_present(OBS_MNTREM)) gate_obs_set(OBS_MNTREM, dm_mntrem_obs); }
        { extern void dm_mntck1_obs(const uint64_t *); if (gate_obs_present(OBS_MNTCK1)) gate_obs_set(OBS_MNTCK1, dm_mntck1_obs); }
        { extern void dm_mntck2_obs(const uint64_t *); if (gate_obs_present(OBS_MNTCK2)) gate_obs_set(OBS_MNTCK2, dm_mntck2_obs); }
        { extern void dm_mntck3_obs(const uint64_t *); if (gate_obs_present(OBS_MNTCK3)) gate_obs_set(OBS_MNTCK3, dm_mntck3_obs); }
        { extern void dm_mntck4_obs(const uint64_t *); if (gate_obs_present(OBS_MNTCK4)) gate_obs_set(OBS_MNTCK4, dm_mntck4_obs); }
        { extern void dm_mntck5_obs(const uint64_t *); if (gate_obs_present(OBS_MNTCK5)) gate_obs_set(OBS_MNTCK5, dm_mntck5_obs); }
        { extern void dm_mntck6_obs(const uint64_t *); if (gate_obs_present(OBS_MNTCK6)) gate_obs_set(OBS_MNTCK6, dm_mntck6_obs); }
        { extern void obs_spawn(const uint64_t *); if (gate_obs_present(OBS_SPAWN)) gate_obs_set(OBS_SPAWN, obs_spawn); }   /* v22D：安卓补挂 spawn 观测（此前只 Mac 挂——「零 spawn」判断的盲区） */
        { extern void dm_fsarg1_obs(const uint64_t *); if (gate_obs_present(OBS_FSARG1)) gate_obs_set(OBS_FSARG1, dm_fsarg1_obs); }
        { extern void dm_fsarg2_obs(const uint64_t *); if (gate_obs_present(OBS_FSARG2)) gate_obs_set(OBS_FSARG2, dm_fsarg2_obs); }
        { extern void dm_fsarg1_obs(const uint64_t *); if (gate_obs_present(OBS_FSARG3)) gate_obs_set(OBS_FSARG3, dm_fsarg1_obs); }
        { extern void dm_fsarg1_obs(const uint64_t *); if (gate_obs_present(OBS_FSARG4)) gate_obs_set(OBS_FSARG4, dm_fsarg1_obs); }
        { extern void dm_thrctl_obs(const uint64_t *); if (gate_obs_present(OBS_THRCTL)) gate_obs_set(OBS_THRCTL, dm_thrctl_obs); }
        { extern void dm_lazyfont_obs(const uint64_t *); if (gate_obs_present(OBS_LAZYFONT)) gate_obs_set(OBS_LAZYFONT, dm_lazyfont_obs); }
        { extern void dm_fontreg_obs(const uint64_t *); if (gate_obs_present(OBS_FONTREG)) gate_obs_set(OBS_FONTREG, dm_fontreg_obs); }
        { void dm_tablewatch_start(void); dm_tablewatch_start(); }
        LOG("[android-bootstrap] Compatibility hooks and scene registration ready; automatic menu input disabled");
        if(getenv("DM_RUNTIME_DIAGNOSTICS")) {   // Explicit startup diagnosis only.
            // Diagnostic: dump the game-internal mount list [0x10bd738] entry names (read-chain break investigation)
            module_t *mm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
            if (mm) {
                uint64_t head = 0, cur = 0;
                memcpy(&head, (void *)(mm->base + 0x10bd738), 8);
                memcpy(&cur, (void *)(mm->base + 0x10bd740), 8);
                LOG("[mntdump] 表头 tail=%#llx first=%#llx", (unsigned long long)head, (unsigned long long)cur);
                for (int k = 0; k < 8 && cur && cur != (uint64_t)(mm->base + 0x10bd738); k++) {
                    uint64_t entry = 0, inner = 0, namep = 0;
                    memcpy(&entry, (void *)(uintptr_t)(cur + 0x10), 8);
                    if (entry) memcpy(&inner, (void *)(uintptr_t)(entry + 0x10), 8);
                                        if (inner) { memcpy(&namep, (void *)(uintptr_t)(inner + 0x18 - 0x18 + 0x18), 8); }   // [x22+0x18]
                    // 保守读：条目链 cur=[cur+8]
                    uint64_t next = 0; memcpy(&next, (void *)(uintptr_t)(cur + 8), 8);
                    LOG("[mntdump] 条目%d node=%#llx entry=%#llx inner=%#llx name_ptr=%#llx", k,
                        (unsigned long long)cur, (unsigned long long)entry, (unsigned long long)inner, (unsigned long long)namep);
                    if (!next || next == cur) break;
                    cur = next;
                }
            }
        }
    }
    phase_set("正在启动游戏"); run_module_inits();
    char *args[]={"RhythmTengokuMobile",NULL};
    ((void (*)(int))required("_ZN2nn2os11SetHostArgcEi"))(1);
    ((void (*)(char **))required("_ZN2nn2os11SetHostArgvEPPc"))(args);
    atomic_store(&main_entered,1);persist_status();
    if(getenv("DM_RUNTIME_DIAGNOSTICS")) {   // Explicit startup diagnosis only.
        extern module_t g_mod[MAX_MOD]; extern int g_nmod;
        for (int i=0;i<g_nmod;i++) if(!strcmp(g_mod[i].name,"main")){
            char fn[PATH_MAX]; path(fn,sizeof fn,port_root,"diagnostics/main_data.bin");
            uint64_t sz = g_mod[i].size - 0xcd9000;   // 数据段从 cd9000 起
            FILE *df = fopen(fn,"wb");
            if(df){ fwrite((void*)(g_mod[i].base+0xcd9000),1,sz,df); fclose(df);
                LOG("[21fix-C] main 数据段 dump %#llx 字节 → %s",(unsigned long long)sz,fn); }
            break;
        }
    }
    if(getenv("DM_RUNTIME_DIAGNOSTICS")) {   // Explicit startup diagnosis only.
        extern module_t g_mod[MAX_MOD]; extern int g_nmod;
        uint64_t mb=0; for (int i=0;i<g_nmod;i++) if(!strcmp(g_mod[i].name,"main")) mb=(uint64_t)g_mod[i].base;
        uint64_t app=0,mgr=0; int st=-1;
        if(mb){ memcpy(&app,(void*)(mb+0x1106580),8);
            if(app){ memcpy(&mgr,(void*)(app+0x10),8);
                if(mgr) memcpy(&st,(void*)(mgr+0x250),4); } }
        LOG("[20fix] nnMain 前 mb=%#llx app=%#llx mgr=%#llx [mgr+0x250]=%d",(unsigned long long)mb,(unsigned long long)app,(unsigned long long)mgr,st);
    }
    call_original("nnMain");
    atomic_store(&returned,1); phase_set("游戏已退出"); return NULL;
}
static int copy_jstring(JNIEnv *env,jstring text,char *out,size_t cap) {
    if (!text) return EINVAL;
    const char *s=(*env)->GetStringUTFChars(env,text,NULL); if (!s) return ENOMEM;
    int rc=strlen(s)>=cap?ENAMETOOLONG:0; if (!rc) strcpy(out,s);
    (*env)->ReleaseStringUTFChars(env,text,s); return rc;
}
JNIEXPORT jint JNICALL Java_org_rhythmtengoku_mobile_GameActivity_nativeConfigure(JNIEnv *env,jclass cls,jstring files,jstring locale,jboolean touch_route) {
    (void)cls;
    if (atomic_exchange(&configured,1)) return EALREADY;
    int rc=copy_jstring(env,files,app_files,sizeof app_files);
    if (!rc) rc=copy_jstring(env,locale,locale_tag,sizeof locale_tag);
    if (rc || app_files[0]!='/') return rc?rc:EINVAL;
    if (!touch_route) return ENODEV;
    atomic_store(&input_route_ready,1);
    path(port_root,sizeof port_root,app_files,"rhythm-tengoku");
    /* Redirect stderr only after creating diagnostics below. On a fresh
     * install, a failed freopen would close the FILE stream; later dup2 alone
     * cannot restore fprintf/LOG even if file descriptor 2 is valid again. */
    // RomFS 的外部候选路径:app 外部私有目录(adb push 可直达)/storage/emulated/0/Android/data/<pkg>/files/rhythm-tengoku/romfs。
    // game_thread 优先使用它(整目录 ~3.2GB 推私有内部目录走 run-as 太慢)。
    // Preferred RomFS location: the app's external private dir (adb-pushable)
    // /storage/emulated/0/Android/data/<pkg>/files/rhythm-tengoku/romfs — game_thread
    // picks it up (pushing ~3.2GB into the internal dir via run-as is too slow).
    { char ext_rom[PATH_MAX];
      // getFilesDir() = /data/user/0/<pkg>/files(或 /data/data/<pkg>/files)——取包名段
      // 推导外部私有目录 /storage/emulated/0/Android/data/<pkg>/files/rhythm-tengoku/romfs。
      // getFilesDir() = /data/user/0/<pkg>/files - derive the external private dir
      // /storage/emulated/0/Android/data/<pkg>/files/rhythm-tengoku/romfs from the package name.
      const char *pfx = strstr(app_files, "/data/user/0/");
      size_t pfxlen = 12;
      if (!pfx) { pfx = strstr(app_files, "/data/data/"); pfxlen = 11; }
      if (pfx) {
        const char *pkgname = pfx + pfxlen;
        size_t n = strcspn(pkgname, "/");   // 包名长度(到 /files 为止) | package name length
        snprintf(ext_rom,sizeof ext_rom,"/storage/emulated/0/Android/data/%.*s/files/rhythm-tengoku/romfs",(int)n,pkgname);
        setenv("RTG_ROMFS_DIR",ext_rom,0);
        LOG("RTG RomFS 外部候选:%s",ext_rom);
      } }
    if ((rc=configure_render_scale())) { LOG("ANDROID_3D_SCALE_CONFIG_FAILED errno=%d expected finite 0.70..1.0",rc); return rc; }
    char driver_option[PATH_MAX];
    unsetenv("DM_RUNTIME_DIAGNOSTICS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/runtime-diagnostics");
    if(access(driver_option,F_OK)==0)setenv("DM_RUNTIME_DIAGNOSTICS","1",1);
    unsetenv("DM_ANDROID_PRESENT_60FPS");
    /* The guest's simulation clocks advance per frame. A faster Android
     * display must not change its 60 Hz base until rendering is decoupled. */
    setenv("DM_PRESENT_HZ","60",1);
    setenv("DM_VSYNC_CALIBRATION","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-vsync-calibration");
    if(access(driver_option,F_OK)==0)unsetenv("DM_VSYNC_CALIBRATION");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/present-60fps");
    if(access(driver_option,F_OK)==0)setenv("DM_ANDROID_PRESENT_60FPS","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/private-vulkan");
    if(access(driver_option,F_OK)==0)setenv("DM_PRIVATE_VULKAN","turnip",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/private-vulkan-sysmem");
    if(access(driver_option,F_OK)==0){setenv("DM_PRIVATE_VULKAN","turnip",1);setenv("TU_DEBUG","sysmem",1);}
    path(driver_option,sizeof driver_option,port_root,"diagnostics/vulkan-validation");
    if(access(driver_option,F_OK)==0)setenv("DM_VULKAN_VALIDATION","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-pass-reuse");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_PASS_REUSE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-cpu-uniform-snapshot");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_CPU_UNIFORM_SNAPSHOT","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-pso-reuse");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_PSO_REUSE","1",1);
    unsetenv("DM_PSO_LAST_CACHE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/enable-pso-last-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_PSO_LAST_CACHE","1",1);
    setenv("DM_WRITEBACK_PREPARE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-writeback-prepare");
    if(access(driver_option,F_OK)==0)setenv("DM_WRITEBACK_PREPARE","0",1);
    unsetenv("DM_WRITEBACK_COALESCE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/writeback-coalesce");
    if(access(driver_option,F_OK)==0)setenv("DM_WRITEBACK_COALESCE","1",1);
    unsetenv("DM_COMMAND_STATE_CACHE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/command-state-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_COMMAND_STATE_CACHE","1",1);
    setenv("DM_DIRTY_SUMMARY","1",1);unsetenv("DM_DIRTY_QUERY_STATS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-dirty-summary");
    if(access(driver_option,F_OK)==0)setenv("DM_DIRTY_SUMMARY","0",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/dirty-query-stats");
    if(access(driver_option,F_OK)==0)setenv("DM_DIRTY_QUERY_STATS","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-driver-table-reuse");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_DRIVER_TABLE_REUSE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-temp-pool");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_TEMP_POOL","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-storage-bias");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_STORAGE_BIAS","1",1);
    unsetenv("DM_DIAG_IMG_USAGE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-img-usage-1");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_IMG_USAGE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-img-usage-2");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_IMG_USAGE","2",1);
    unsetenv("DM_BARRIER_PASS_ELIDE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/barrier-pass-elide");
    if(access(driver_option,F_OK)==0)setenv("DM_BARRIER_PASS_ELIDE","1",1);
    unsetenv("DM_DIAG_NO_PASS_DEPS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-no-pass-deps");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_NO_PASS_DEPS","1",1);
    unsetenv("DM_DISABLE_PRECISE_SYNC");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-precise-sync");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_PRECISE_SYNC","1",1);
    unsetenv("DM_HZ_DIAG");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/hz-diag.txt");
    {FILE *hf=fopen(driver_option,"r");if(hf){char hv[8]={0};size_t hn=fread(hv,1,7,hf);fclose(hf);if(hn)setenv("DM_HZ_DIAG",hv,1);}}
    unsetenv("DM_CHUNK_DRAWS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/chunk-draws.txt");
    {FILE *cf=fopen(driver_option,"r");if(cf){char cv[12]={0};size_t cn=fread(cv,1,11,cf);fclose(cf);if(cn)setenv("DM_CHUNK_DRAWS",cv,1);}}
    unsetenv("DM_PROFILE_DETAIL");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/profile-detail");
    if(access(driver_option,F_OK)==0)setenv("DM_PROFILE_DETAIL","1",1);
    unsetenv("DM_DIAG_SKIP_DRAWS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-skip-draws");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_SKIP_DRAWS","1",1);
    unsetenv("DM_DIAG_SKIP_XFER");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/skip-xfer.txt");
    {FILE *xf=fopen(driver_option,"r");if(xf){char xv[8]={0};size_t xn=fread(xv,1,7,xf);fclose(xf);if(xn)setenv("DM_DIAG_SKIP_XFER",xv,1);}}
    unsetenv("DM_DISABLE_SUBRES");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-subres");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_SUBRES","1",1);
    unsetenv("DM_DISABLE_SETRT_SKIP");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-setrt-skip");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_SETRT_SKIP","1",1);
    unsetenv("DM_DISABLE_WB_STAGE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-wb-stage");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_WB_STAGE","1",1);
    unsetenv("DM_DIAG_SKIP_CLASS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/skip-class.txt");
    {FILE *kf=fopen(driver_option,"r");if(kf){char kv[8]={0};size_t kn=fread(kv,1,7,kf);fclose(kf);if(kn)setenv("DM_DIAG_SKIP_CLASS",kv,1);}}
    unsetenv("DM_DIAG_SHAPE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-shape.txt");
    {FILE *hf2=fopen(driver_option,"r");if(hf2){char hv2[8]={0};size_t hn2=fread(hv2,1,7,hf2);fclose(hf2);if(hn2)setenv("DM_DIAG_SHAPE",hv2,1);}}
    unsetenv("DM_NARROW_ACCESS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/narrow-access");
    if(access(driver_option,F_OK)==0)setenv("DM_NARROW_ACCESS","1",1);
    unsetenv("DM_DISABLE_WAIT_ELISION");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-wait-elision");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_WAIT_ELISION","1",1);
    unsetenv("DM_DISABLE_ASYNC_COMMIT");
    setenv("DM_REUSE_QUEUE_IDLE","1",1);
    setenv("DM_CPU_STATE_REPLAY","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-cpu-state-replay");
    if(access(driver_option,F_OK)==0)unsetenv("DM_CPU_STATE_REPLAY");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-queue-idle-reuse");
    if(access(driver_option,F_OK)==0)unsetenv("DM_REUSE_QUEUE_IDLE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-async-commit");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_ASYNC_COMMIT","1",1);
    unsetenv("DM_NVN_SYNC");
    setenv("DM_NVN_INLINE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-nvn-inline");
    if(access(driver_option,F_OK)==0)unsetenv("DM_NVN_INLINE");
    unsetenv("DM_DISABLE_BC_PRELOAD");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-bc-preload");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_BC_PRELOAD","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/nvn-sync");
    if(access(driver_option,F_OK)==0)setenv("DM_NVN_SYNC","1",1);
    unsetenv("DM_CONFLICT_WAIT_ALL");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/conflict-wait-all");
    if(access(driver_option,F_OK)==0)setenv("DM_CONFLICT_WAIT_ALL","1",1);
    unsetenv("DM_DIAG_FIXED_DESC");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-fixed-desc");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_FIXED_DESC","1",1);
    unsetenv("DM_DIAG_NO_OCCLUSION");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-no-occlusion");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_NO_OCCLUSION","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-query-batch");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_QUERY_BATCH","1",1);
    setenv("DM_QUERY_RESET_RANGE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-query-reset-range");
    if(access(driver_option,F_OK)==0)setenv("DM_QUERY_RESET_RANGE","0",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-desc-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_DESC_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-resource-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_RESOURCE_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-dirty-uploads");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_DIRTY_UPLOADS","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-control-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_CONTROL_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-loadstore-store");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_LOADSTORE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-loadstore-both");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_LOADSTORE","2",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-barrier-elision");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_BARRIER_ELISION","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-sample-copy-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_SAMPLE_COPY_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/skip-ps.txt");
    {FILE *sf=fopen(driver_option,"r");if(sf){char sk[17]={0};size_t n=fread(sk,1,16,sf);fclose(sf);if(n==16)setenv("DM_SKIP_PS",sk,1);}}
    unsetenv("DM_EVENT_TRACE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/event-trace");
    if(access(driver_option,F_OK)==0)setenv("DM_EVENT_TRACE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/pass-timing");
    if(access(driver_option,F_OK)==0)setenv("DM_PASS_TIMING","1",1);
    /* Hint sessions are an explicit A/B experiment, off when the marker is absent. */
    unsetenv("DM_PERFORMANCE_HINTS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/performance-hints");
    if(access(driver_option,F_OK)==0)setenv("DM_PERFORMANCE_HINTS","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/capture-frames");
    if(access(driver_option,F_OK)==0)setenv("DM_CAPTURE_FRAMES","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-barrier-cmd");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_BARRIER","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-barrier-soft");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_BARRIER","2",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/barrier-stats");
    if(access(driver_option,F_OK)==0)setenv("DM_BARRIER_STATS","1",1);
    char logs[PATH_MAX], log[PATH_MAX], shaders[PATH_MAX];
    path(logs,sizeof logs,port_root,"diagnostics");
    setenv("DM_FRAME_DIR",logs,1);
    path(driver_option,sizeof driver_option,logs,"attachment-mode.txt");
    setenv("DM_VK_ATTACHMENT_CONTROL",driver_option,1);
    /* Opt-in live A/B control. The file is sampled by the replay thread only
     * at frame boundaries; it does not restart or alter the game session. */
    unsetenv("DM_VK_AB_CONTROL");
    path(driver_option,sizeof driver_option,logs,"attachment-ab");
    if(access(driver_option,F_OK)==0){
        path(driver_option,sizeof driver_option,logs,"attachment-mode.txt");
        setenv("DM_VK_AB_CONTROL",driver_option,1);
    }
    if (dm_android_fs_ensure_directory(logs)) return errno?errno:EIO;
    path(log,sizeof log,logs,"game-startup.log");
    {   /* 上游 7eb212c:保留上一会话(崩溃后重启可查) */
        char previous_log[PATH_MAX];path(previous_log,sizeof previous_log,logs,"game-startup.previous.log");
        if(rename(log,previous_log)&&errno!=ENOENT)return errno; }

    char status_path[PATH_MAX]; path(status_path,sizeof status_path,logs,"status.json");
    if (unlink(status_path) && errno!=ENOENT) return errno;
    strcpy(report_file,status_path);path(report_temp,sizeof report_temp,logs,"status.native.new");
    int fd=open(log,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600); if (fd<0) return errno;
    if(fd!=STDERR_FILENO){
        if (dup2(fd,STDERR_FILENO)<0) {rc=errno;close(fd);return rc;}
        close(fd);
    }
    setvbuf(stderr,NULL,_IONBF,0);
    g_t0_ns=now_ns();
    path(driver_option,sizeof driver_option,port_root,"diagnostics/verbose-host");
    g_flag.fs=g_flag.svc=g_flag.dbg=g_flag.thr=access(driver_option,F_OK)==0;
    path(shaders,sizeof shaders,port_root,"shaders"); dm_vk_set_shader_root(shaders);
    LOG("ANDROID_HOST app_uid=%u min_api=29 original_modules=3 selected_addons=0 no_CPU_interpreter",getuid());
    return 0;
}
JNIEXPORT void JNICALL Java_org_rhythmtengoku_mobile_GameActivity_nativeVsync(JNIEnv *env,jclass cls,jlong stamp) {
    (void)env;(void)cls;dm_android_nvn_vsync(stamp>0?(uint64_t)stamp:0);
}
JNIEXPORT jint JNICALL Java_org_rhythmtengoku_mobile_GameActivity_nativeSurface(JNIEnv *env,jclass cls,jobject surface,jint width,jint height,jfloat density,jfloat hz,jint focus) {
    (void)cls;
    ANativeWindow *next=surface?ANativeWindow_fromSurface(env,surface):NULL;
    if (surface && !next) return EINVAL;
    /* Suspend before replacement; no guest can present through a released window. */
    if (atomic_load(&started)) dm_gfx_suspend(1);
    pause_audio(focus!=1 || !next);
    dm_vk_set_window(next);
    pthread_mutex_lock(&state_lock);
    ANativeWindow *old=native_window; native_window=next;
    window_state=(dm_android_window_state){.focus=focus,.surface_available=next!=NULL,
        .width=(uint32_t)width,.height=(uint32_t)height,.density=density,.refresh_hz=hz,.thermal_status=-1};
    dm_android_window_state w=window_state;
    pthread_mutex_unlock(&state_lock);
    if (old) ANativeWindow_release(old);
    if (next && !atomic_load(&input_bound)) {
        dm_android_input_config input={.source="Android.SurfaceView.MotionEvent",.panel_width=1280,.panel_height=720,
            .source_ready=input_source_ready,.logical_npad_style=1,.logical_npad_id=0};
        int rc=dm_android_input_bind(&input);if(rc)return rc;atomic_store(&input_bound,1);
    }
    if (!next || focus!=1) dm_android_input_cancel();
    dm_vk_set_display_hz((int)(hz+0.5f));
    dm_android_platform_status status; dm_android_platform_get_status(&status);
    if (status.bound) {int rc=dm_android_platform_update_window(&w);if(rc)return rc;}
    if (next && focus==1) dm_gfx_suspend(0);
    return 0;
}
JNIEXPORT jint JNICALL Java_org_rhythmtengoku_mobile_GameActivity_nativeTouch(JNIEnv *env,jclass cls,jintArray ids,jfloatArray xs,jfloatArray ys,jboolean cancelled) {
    (void)cls;
    if (!atomic_load(&input_bound)) return ENODEV;
    if (cancelled) {dm_android_input_cancel();return 0;}
    if (!ids || !xs || !ys) return EINVAL;
    jsize n=(*env)->GetArrayLength(env,ids);
    if(n>16 || (*env)->GetArrayLength(env,xs)!=n || (*env)->GetArrayLength(env,ys)!=n)return EINVAL;
    jint pointer_ids[16];jfloat px[16],py[16];dm_android_pointer points[16];
    (*env)->GetIntArrayRegion(env,ids,0,n,pointer_ids);(*env)->GetFloatArrayRegion(env,xs,0,n,px);(*env)->GetFloatArrayRegion(env,ys,0,n,py);
    if((*env)->ExceptionCheck(env))return EINVAL;
    for(jsize i=0;i<n;++i)points[i]=(dm_android_pointer){.id=(uint32_t)pointer_ids[i],.x=px[i],.y=py[i]};
    return dm_android_input_publish_touch(points,(size_t)n);
}
JNIEXPORT jint JNICALL Java_org_rhythmtengoku_mobile_GameActivity_nativeStart(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;
    if (!atomic_load(&configured) || !dm_android_native_window()) return ENODEV;
    if (atomic_exchange(&started,1)) return EALREADY;
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,8*1024*1024);
    pthread_t worker;int error=pthread_create(&worker,&attr,game_thread,NULL);pthread_attr_destroy(&attr);
    if (error) {atomic_store(&started,0);return error;} pthread_detach(worker);return 0;
}
JNIEXPORT jint JNICALL Java_org_rhythmtengoku_mobile_GameActivity_nativeActions(JNIEnv *env,jclass cls,jlong buttons,jint lx,jint ly,jint rx,jint ry) {
    (void)env;(void)cls;return dm_android_input_publish_actions((uint64_t)buttons,lx,ly,rx,ry);
}
JNIEXPORT void JNICALL Java_org_rhythmtengoku_mobile_GameActivity_nativeCancelInput(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;dm_android_input_cancel();
}
JNIEXPORT jint JNICALL Java_org_rhythmtengoku_mobile_GameActivity_nativeControllerButtons(JNIEnv *env,jclass cls,jlong buttons) {
    (void)env;(void)cls;return dm_android_input_publish_controller_buttons((uint64_t)buttons);
}
JNIEXPORT void JNICALL Java_org_rhythmtengoku_mobile_GameActivity_nativeControllerCancel(JNIEnv *env,jclass cls,jlong remaining_buttons) {
    (void)env;(void)cls;dm_android_input_cancel_controller((uint64_t)remaining_buttons);
}
JNIEXPORT jstring JNICALL Java_org_rhythmtengoku_mobile_GameActivity_nativeStatus(JNIEnv *env,jclass cls) {
    (void)cls;char text[384];
    pthread_mutex_lock(&state_lock);
    status_text(text,sizeof text);
    pthread_mutex_unlock(&state_lock);persist_status();return (*env)->NewStringUTF(env,text);
}

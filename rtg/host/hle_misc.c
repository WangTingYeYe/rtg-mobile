// 零散服务层：追加内容 / 商店 / 错误小程序 / 敏感词过滤 / 服务器名解析 / 家长控制 / 软键盘。
// 真机上各自走 IPC 或系统小程序；宿主按单机离线给结果。纯计算的 SDK 部分（hash、util、软件 Opus）不接管，直接用原实现。
// Miscellaneous service layer: add-on content / shop / error applet / profanity filter / server name resolution / parental controls / software keyboard.
// On the real device each goes through IPC or a system applet; the host gives results as single-player offline. Pure-computation SDK parts (hash, util, software Opus) are not taken over, the original implementation is used directly.
#include "hle.h"
#include "shared_font_data.h"
#ifdef __ANDROID__
#include "../android/services_platform.h"
#endif

// ---------- aoc 追加内容（术士君临，保留）/ ec 商店（排除）----------
// 术士君临（Reign of the Warlock）是 Switch 包里唯一的追加内容：AOC 0100726014353001 = 本体 0100726014352000 的追加内容序号 1。
// 该 DLC 只是约 100 KB 的解锁凭证，内容本体已在主 RomFS；main 不导入 MountAddOnContent，只按「有没有序号 1」判断。
// 复刻范围要求保留术士君临，所以报告已拥有序号 1；排除的是商店（ec），不是 DLC 本身。
// ---------- aoc add-on content (Reign of the Warlock, kept) / ec shop (excluded) ----------
// Reign of the Warlock is the only add-on content in the Switch package: AOC 0100726014353001 = add-on content index 1 of the base 0100726014352000.
// This DLC is only an unlock ticket of about 100 KB, the content itself is already in the main RomFS; main does not import MountAddOnContent and only judges by "is index 1 present".
// The reimplementation scope requires keeping Reign of the Warlock, so index 1 is reported as owned; what is excluded is the shop (ec), not the DLC itself.
enum { AOC_WARLOCK = 1 };
// DM_AOC_OWNED=0：测试开关，回到「无 DLC」前提（只用于对拍未装 DLC 时录的真值；缺省 = 拥有）
// DM_AOC_OWNED=0: test switch, back to the "no DLC" premise (only for lockstep comparison against ground truth recorded without the DLC installed; default = owned)
#ifndef __ANDROID__
static int aoc_owned(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_AOC_OWNED"); v = !(e && !strcmp(e, "0")); if (!v) LOG("DM_AOC_OWNED=0：aoc 报告 0 个追加内容（对拍测试用）"); } return v; }
#endif
HLE_FORCE("_ZN2nn3aoc17CountAddOnContentEv", "追加内容数：Android按已验证当前版本内容清单", int, h_aoc_Count, void) {
#ifdef __ANDROID__
    return dm_android_platform_aoc_count();
#else
    return aoc_owned() ? 1 : 0;
#endif
}
// ListAddOnContent(out, 起始下标, 最多个数)：按已拥有序号列表 {1} 从起始下标起写出，返回写出个数
// ListAddOnContent(out, start index, max count): writes out from the owned index list {1} starting at the start index, returns the number written
HLE_FORCE("_ZN2nn3aoc16ListAddOnContentEPiii", "追加内容列表：{1}（术士君临）", int, h_aoc_List, int *out, int off, int n) {
#ifdef __ANDROID__
    return dm_android_platform_aoc_list(out, off, n);
#else
    static const int owned[] = {AOC_WARLOCK};
    int w = 0;
    if (!aoc_owned()) return 0;
    for (int i = off; i < (int)(sizeof owned / sizeof *owned) && w < n; i++) out[w++] = owned[i];
    return w;
#endif
}
// 事件对象按需由 nn::os 旁表建（未触发），游戏等它只会超时 / 取到未触发
// The event object is created on demand by the nn::os side table (not signaled), the game waiting on it only times out / gets not-signaled
HLE_FORCE("_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE", "追加内容变更事件，排除范围；绑内核事件、恒不触发（游戏会 TryWait 轮询，未初始化即中止）", void, h_aoc_GetEv, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn2ec24ShowShopAddOnContentListEv", "商店小程序，排除范围；剪口：空操作", void, h_ec_ShowShop, void) { LOG("ec::ShowShopAddOnContentList（剪）"); }

// ---------- err 错误小程序：宿主打印 ----------
// ---------- err error applet: printed by the host ----------
HLE_FORCE("_ZN2nn3err9ShowErrorENS_6ResultE", "错误显示小程序；宿主打印模块 / 描述号", void, h_err_ShowError, Result r) {
#ifdef __ANDROID__
    dm_android_platform_record_error(r, "nn::err::ShowError");
#endif
    LOG("err::ShowError Result=%#x（模块 %u，描述 %u）", r, r & 0x1ff, (r >> 9) & 0x1fff);
}

// ---------- ngc 敏感词过滤：词库在系统数据存档（经 IPC 挂载）；单机无他人可见文本，恒判无敏感词 ----------
// ---------- ngc profanity filter: the word list is in a system data save (mounted via IPC); single-player has no text visible to others, always judged as no profanity ----------
HLE_FORCE("_ZN2nn3ngc15ProfanityFilterC1Ev", "敏感词过滤，词库走系统存档；宿主不建", void, h_ngc_Ctor, void *self) {
#ifdef __ANDROID__
    dm_android_platform_object_create(self, 30);
#else
    (void)self;
#endif
}
HLE_FORCE("_ZN2nn3ngc15ProfanityFilterD1Ev", "敏感词过滤", void, h_ngc_Dtor, void *self) {
#ifdef __ANDROID__
    dm_android_platform_object_destroy(self, 30);
#else
    (void)self;
#endif
}
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", "离线本地文字直通策略；不声称加载任天堂词库", Result, h_ngc_Init, void *self, void *work, uint64_t sz) {
#ifdef __ANDROID__
    if (!work || !sz || !dm_android_platform_object_exists(self, 30) || !dm_android_platform_network_ready()) return RES_FAIL;
    dm_android_platform_setting("local_text_passthrough", 1);
    dm_android_platform_object_ready(self, 30); return RES_OK;
#else
    (void)self; (void)work; (void)sz; return RES_OK;
#endif
}
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", "词库版本；宿主 0", uint32_t, h_ngc_Ver, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", "敏感词检查；恒无命中", Result, h_ngc_Check, void *self, uint32_t *masks, uint32_t pat, const char **words, uint64_t n) {
#ifdef __ANDROID__
    if (!dm_android_platform_object_is_ready(self, 30) || (n && (!masks || !words))) return RES_FAIL;
    for (uint64_t i = 0; i < n; i++) if (!dm_android_platform_utf8_valid(words[i])) return RES_FAIL;
#endif
    (void)self; (void)pat; (void)words;
    for (uint64_t i = 0; i < n; i++) masks[i] = 0;
    return RES_OK;
}

// ---------- nsd 服务器名解析 / pctl 自由通信：联网，排除范围 ----------
// ---------- nsd server name resolution / pctl free communication: networking, out of scope ----------
HLE_FORCE("_ZN2nn3nsd7ResolveEPNS0_4FqdnERKS1_", "服务器名解析，联网排除；剪口", Result, h_nsd_Resolve, void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
HLE_FORCE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", "服务器名解析，联网排除；剪口", Result, h_nsd_ResolveEx, void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
// bool 参数只看低 8 位；返回 bool
// bool arguments only look at the low 8 bits; returns bool
HLE_FORCE("_ZN2nn4pctl25TryBeginFreeCommunicationEb", "家长控制·与他人通信，联网排除；恒不允许", unsigned, h_pctl_TryBegin, unsigned show_ui) {
#ifdef __ANDROID__
    dm_android_platform_setting("communication_request_denied", 1);
#endif
    (void)show_ui; return 0;
}
HLE_FORCE("_ZN2nn4pctl20EndFreeCommunicationEv", "家长控制·与他人通信", void, h_pctl_End, void) {
#ifdef __ANDROID__
    dm_android_platform_setting("communication_request_active", 0);
#endif
}

// swkbd 软键盘：见 hle_swkbd.c
// swkbd software keyboard: see hle_swkbd.c

// ---- 节奏天国移植新增：nn::pl 共享字体查询（游戏自带 mesg/Font 全套 bfttf，共享字体仅引导期查询）----
// ---- Added for this port: shared-font query; the game ships its own fonts ----
HLE_FORCE("_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE",
    "pl 共享字体；bool 返回，输出数量及24字节类型/地址/大小条目",
    uint32_t, h_pl_GetSharedFont, int *count, void *out, int n, uint64_t lang) {
    (void)lang;
    static void *font; static size_t font_sz;
    if (!font) {
        const char *p = getenv("DM_SHARED_FONT");
        char path[1024]; snprintf(path, sizeof path, "%s", p && *p ? p : "/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
        FILE *f = fopen(path, "rb"); if (!f) die("共享字体打不开：%s（可用 DM_SHARED_FONT 换）", path);
        fseek(f, 0, SEEK_END); font_sz = ftell(f); fseek(f, 0, SEEK_SET);
        font = malloc(font_sz); if (!font || fread(font, 1, font_sz, f) != font_sz) die("共享字体读不了：%s", path);
        fclose(f);
        /* v63：Android 唯一 CJK 字体 NotoSansCJK 是 .ttc 集合（'ttcf' 头）——SDK 字体解析器只认
         * 单体 sfnt，整 .ttc 喂进去表目录全错 → 字体对象度量表 NULL → 文本排版读 [NULL+0x9a]
         * → 渲染环死（FPS 0.0 的根因）。把首个字体提取成独立 .ttf：目录项偏移平移 -off0，
         * 表数据原位保留。Mac 用 Arial Unicode .ttf 无此问题。 */
        if (font_sz >= 16 && !memcmp(font, "ttcf", 4)) {
            unsigned num = (unsigned)((uint8_t *)font)[8] << 24 | (unsigned)((uint8_t *)font)[9] << 16 | (unsigned)((uint8_t *)font)[10] << 8 | (unsigned)((uint8_t *)font)[11];
            if (num < 1) die("共享字体 ttc 空：%s", path);
            uint32_t off0 = (uint32_t)((uint8_t *)font)[12] << 24 | (uint32_t)((uint8_t *)font)[13] << 16 | (uint32_t)((uint8_t *)font)[14] << 8 | (uint32_t)((uint8_t *)font)[15];
            if (off0 >= font_sz || off0 + 12 > font_sz) die("共享字体 ttc 首字体偏移非法：%s", path);
            uint16_t nt = (uint16_t)(((uint8_t *)font)[off0 + 4] << 8 | ((uint8_t *)font)[off0 + 5]);
            size_t dirsz = 12 + 16 * (size_t)nt;
            if (off0 + dirsz > font_sz) die("共享字体 ttc 目录越界：%s", path);
            size_t newsz = font_sz - off0;
            void *nf = malloc(newsz); if (!nf) die("共享字体 ttc 提取分配失败");
            memcpy(nf, (uint8_t *)font + off0, dirsz);                 /* 目录（sfnt 头 12B + 表项） */
            memcpy((uint8_t *)nf + dirsz, (uint8_t *)font + off0 + dirsz, newsz - dirsz);   /* 表数据原位 */
            for (unsigned t = 0; t < nt; t++) {                         /* 每表项 off -= off0（大端 32 位，偏移 8..11 处） */
                uint8_t *e = (uint8_t *)nf + 12 + 16 * t + 8;
                uint32_t o = (uint32_t)e[0] << 24 | (uint32_t)e[1] << 16 | (uint32_t)e[2] << 8 | (uint32_t)e[3];
                if (o < off0) die("共享字体 ttc 表偏移回卷：%s", path);
                o -= off0;
                e[0] = (uint8_t)(o >> 24); e[1] = (uint8_t)(o >> 16); e[2] = (uint8_t)(o >> 8); e[3] = (uint8_t)o;
            }
            free(font); font = nf; font_sz = newsz;
            LOG("pl 共享字体：%s 是 ttc，已提取首字体为独立 ttf（%u 表，%zu 字节）", path, nt, font_sz);
        } else LOG("pl 共享字体：%s %zu 字节", path, font_sz);
    }
    return dm_shared_font_data(count,out,n,font,font_sz);
}
HLE_FORCE("_ZN2nn2pl16IsRunningOnOunceEv", "Ounce(开发机)判定；恒否", unsigned, h_pl_IsOunce, void) { return 0; }

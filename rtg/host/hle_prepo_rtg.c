// 节奏天国移植新增：nn::prepo（游玩数据上报/遥测）全套空桩——宿主不上报任何数据。
// PlayReport/Struct/Array 是纯数据收集容器：构造/析构/清零需要真行为（对象内存来自游戏），
// Add* 记录进容器的数据在不上报的前提下无需保存；Save 返回成功（原版上报失败也只是日志）。
// Added for this port: nn::prepo no-op stubs. The containers get minimal real behavior
// (construction/clearing), the collectors record nothing, Save reports success.
#include "hle.h"
#include <string.h>
#include <stddef.h>

// prepo 对象按最小布局处理：只做零初始化 | minimal: zero-init only
HLE_FORCE("_ZN2nn5prepo10PlayReportC1Ev", "遥测报告构造；清零", void, h_pr_Ctor, void *self) { memset(self, 0, 0x40); }
HLE_FORCE("_ZN2nn5prepo6StructC1Ev", "遥测结构构造；清零", void, h_pr_SCtor, void *self) { memset(self, 0, 0x30); }
HLE_FORCE("_ZN2nn5prepo5ArrayC1Ev", "遥测数组构造；清零", void, h_pr_ACtor, void *self) { memset(self, 0, 0x30); }
HLE_FORCE("_ZN2nn5prepo10PlayReportD1Ev", "遥测报告析构；空", void, h_pr_Dtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn5prepo10PlayReport10SetEventIdEPKc", "事件号；记录在对象头（Save 不上报）", uint64_t, h_pr_SetEvent, void *self, const char *id) { LOG("[prepo] SetEventId(%s)", id ? id : "(null)"); if (self && id) *(const char **)self = id; return 0; }
HLE_FORCE("_ZN2nn5prepo10PlayReport9SetBufferEPvm", "报告缓冲；忽略", uint64_t, h_pr_SetBuf, void *self, void *b, size_t n) { (void)self; (void)b; (void)n; return 0; }
HLE_FORCE("_ZN2nn5prepo10PlayReport3AddEPKcf", "字段（浮点）；忽略", uint64_t, h_pr_AddF, void *self, const char *k, float v) { (void)self; (void)k; (void)v; return 0; }
HLE_FORCE("_ZN2nn5prepo10PlayReport3AddEPKcRKNS0_6StructE", "字段（结构）；忽略", uint64_t, h_pr_AddS, void *self, const char *k, const void *v) { (void)self; (void)k; (void)v; return 0; }
HLE_FORCE("_ZN2nn5prepo10PlayReport3AddEPKcb", "字段（布尔）；忽略", uint64_t, h_pr_AddB, void *self, const char *k, unsigned char v) { (void)self; (void)k; (void)v; return 0; }
HLE_FORCE("_ZN2nn5prepo10PlayReport3AddEPKcS3_", "字段（字符串）；忽略", uint64_t, h_pr_AddStr, void *self, const char *k, const char *v) { (void)self; (void)k; (void)v; return 0; }
HLE_FORCE("_ZN2nn5prepo10PlayReport3AddEPKcl", "字段（整数）；忽略", uint64_t, h_pr_AddI, void *self, const char *k, long v) { (void)self; (void)k; (void)v; return 0; }
HLE_FORCE("_ZN2nn5prepo10PlayReport3AddEPKcRKNS0_5ArrayE", "字段（数组）；忽略", uint64_t, h_pr_AddA, void *self, const char *k, const void *v) { (void)self; (void)k; (void)v; return 0; }
HLE_FORCE("_ZN2nn5prepo5Array5ClearEv", "数组清空；空", uint64_t, h_pr_AClear, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN2nn5prepo5Array9SetBufferEPvm", "数组缓冲；忽略", uint64_t, h_pr_ASetBuf, void *self, void *b, size_t n) { (void)self; (void)b; (void)n; return 0; }
HLE_FORCE("_ZN2nn5prepo5Array3AddEf", "数组元素（浮点）；忽略", uint64_t, h_pr_AAddF, void *self, float v) { (void)self; (void)v;  return 0; }
HLE_FORCE("_ZN2nn5prepo5Array3AddEb", "数组元素（布尔）；忽略", uint64_t, h_pr_AAddB, void *self, unsigned char v) { (void)self; (void)v;  return 0; }
HLE_FORCE("_ZN2nn5prepo5Array3AddEPKc", "数组元素（字符串）；忽略", uint64_t, h_pr_AAddStr, void *self, const char *v) { (void)self; (void)v;  return 0; }
HLE_FORCE("_ZN2nn5prepo5Array3AddEl", "数组元素（整数）；忽略", uint64_t, h_pr_AAddI, void *self, long v) { (void)self; (void)v;  return 0; }
HLE_FORCE("_ZN2nn5prepo5Array3AddERKNS0_6StructE", "数组元素（结构）；忽略", uint64_t, h_pr_AAddS, void *self, const void *v) { (void)self; (void)v;  return 0; }
HLE_FORCE("_ZN2nn5prepo6Struct9SetBufferEPvm", "结构缓冲；忽略", uint64_t, h_pr_SSetBuf, void *self, void *b, size_t n) { (void)self; (void)b; (void)n; return 0; }
HLE_FORCE("_ZN2nn5prepo6Struct3AddEPKcf", "结构字段（浮点）；忽略", uint64_t, h_pr_SAddF, void *self, const char *k, float v) { (void)self; (void)k; (void)v; return 0; }
HLE_FORCE("_ZN2nn5prepo6Struct3AddEPKcb", "结构字段（布尔）；忽略", uint64_t, h_pr_SAddB, void *self, const char *k, unsigned char v) { (void)self; (void)k; (void)v; return 0; }
HLE_FORCE("_ZN2nn5prepo6Struct3AddEPKcS3_", "结构字段（字符串）；忽略", uint64_t, h_pr_SAddStr, void *self, const char *k, const char *v) { (void)self; (void)k; (void)v; return 0; }
HLE_FORCE("_ZN2nn5prepo6Struct3AddEPKcl", "结构字段（整数）；忽略", uint64_t, h_pr_SAddI, void *self, const char *k, long v) { (void)self; (void)k; (void)v; return 0; }
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", "上报（带用户）；返回 0（新式 Result 成功）", uint64_t, h_pr_SaveU, void *self, const void *uid) { (void)self; (void)uid; LOG("[prepo] Save(uid) 被调，返回 0"); return 0; }
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveEv", "上报；返回 0（新式 Result 成功）", uint64_t, h_pr_Save, void *self) { (void)self; LOG("[prepo] Save() 被调，返回 0"); return 0; }
HLE_FORCE("_ZN2nn5prepo10PlayReport5ClearEv", "报告清空；清零", void, h_pr_Clear, void *self) { if (self) memset(self, 0, 0x40); }

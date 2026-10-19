# 通用 NSO 改写器：Switch aarch64 模块 → 可在苹果系统原生执行的映像。
# 用法：patch_nso.py <NSO 文件> <输出目录> <模块名>
# 改写（苹果内核会改 x18 与 tpidr_el0；Horizon 的线程区、svc、计时器在苹果上没有对应物）：
#   x18 读写            → 线程槽 K18（TSD，经 tpidrro_el0）
#   mrs tpidr_el0       → 读线程槽 KTLS；msr tpidr_el0 → 写 KTLS
#   mrs tpidrro_el0     → 读线程槽 KTLR（Horizon ThreadLocalRegion 指针）
#   svc #n、mrs cntpct_el0 → 宿主门（门地址取自线程槽 KGATE，进 C 后按返回点查 gates.tsv）
#   已登记剪口（CUTS，按 NSO 构建号限定）：改写前逐条断言原字节，不符即中止；结果列在 <名>.cuts.tsv
#   已登记观测门（OBS，按构建号限定）：入口一条指令换成「进宿主观测回调 → 执行原指令 → 跳回」，诊断零运行时改代码；列在 <名>.obs.tsv
# 键页只在数据段末尾保存“实际键号 × 8”，补丁代码通过键页间接取线程槽：装载器可使用任意键号。
# 输出：<名>.patched.bin（键页与补丁区接在映像后 PATCH_OFF）、<名>.fixups.tsv（布局与键页下标表头）、<名>.gates.tsv、<名>.sites.tsv、<名>.cuts.tsv、<名>.obs.tsv
# Generic NSO rewriter: Switch aarch64 module → an image that can execute natively on Apple systems.
# Usage: patch_nso.py <NSO file> <output dir> <module name>
# Rewrites (the Apple kernel modifies x18 and tpidr_el0; Horizon's thread area, svc and timers have no counterpart on Apple):
#   x18 read/write      → thread slot K18 (TSD, via tpidrro_el0)
#   mrs tpidr_el0       → read thread slot KTLS; msr tpidr_el0 → write KTLS
#   mrs tpidrro_el0     → read thread slot KTLR (Horizon ThreadLocalRegion pointer)
#   svc #n, mrs cntpct_el0 → host gate (gate address taken from thread slot KGATE; once in C, gates.tsv is looked up by return point)
#   Registered cuts (CUTS, restricted by NSO build ID): before rewriting, the original bytes are asserted one by one and any mismatch aborts; results are listed in <name>.cuts.tsv
#   Registered observation gates (OBS, restricted by build ID): one instruction at the entry is replaced with "enter the host observation callback → execute the original instruction → jump back", so diagnostics modify zero code at runtime; listed in <name>.obs.tsv
# The key page only stores "actual key number × 8" at the end of the data segment, and patch code fetches thread slots indirectly through the key page: the loader may use any key number.
# Output: <name>.patched.bin (key page and patch area appended after the image at PATCH_OFF), <name>.fixups.tsv (layout and key page index header), <name>.gates.tsv, <name>.sites.tsv, <name>.cuts.tsv, <name>.obs.tsv
import struct, re, sys, os
from itertools import combinations
import lz4.block
from capstone import *

src, outdir, NAME = sys.argv[1], sys.argv[2], sys.argv[3]
ANDROID = os.environ.get('DM_TARGET') == 'android'
d = open(src, 'rb').read()
assert d[:4] == b'NSO0'
flags = struct.unpack_from('<I', d, 0xc)[0]
segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
bss = struct.unpack_from('<I', d, 0x3c)[0]
csz = struct.unpack_from('<III', d, 0x60)
IMG = bytearray(segs[2][1] + segs[2][2] + bss)
for i, (fo, mo, sz) in enumerate(segs):
    raw = d[fo:fo + csz[i]]
    if flags >> i & 1: raw = lz4.block.decompress(raw, uncompressed_size=sz)
    IMG[mo:mo + sz] = raw
TEXT_END = segs[0][2]
RO_OFF, DATA_OFF = segs[1][1], segs[2][1]
KEY_PAGE_OFF = (len(IMG) + 0x3FFF) & ~0x3FFF
IMG.extend(bytes(0x4000))
PATCH_OFF = (len(IMG) + 0xFFFF) & ~0xFFFF

# Version-specific RTG cuts; expected bytes are verified before rewriting.
CUTS = {'03ff4a3bec96bde3a2e37ce3aa4caa0c': [
    # 情景注册恒走：0x5b150 情景函数 w22==0 轮的注册（0x5b3b4 bl 0x628a0 写类型表）被
    # [mgr+0x1ce4]≠1 短路（真机由旗标 9/0xa/0xb 检查通过后置 1；我们的运行里旗标全 0）。
    # NOP 掉 b.ne 跳过分支 → 注册恒执行（数据写入、无副作用）
    (0x5b388, '81f10054', '1f2003d5', 'RTG 情景类型注册恒走：b.ne 跳过改 NOP，w22==0 轮的 0x628a0 注册不再被 [mgr+0x1ce4] 短路'),
]}
OBS = {'02688274b7383bdb369d21663d2c3f3f': [
    (0x4dfcb0, 29, 'a9bd7bfd', 'demo std::thread spawn 入口（demo 版偏移 0x4dfcb0）：OBS_SPAWN 记录 boot 线程创建序列'),
], '03ff4a3bec96bde3a2e37ce3aa4caa0c': [
    (0x509570, 23, 'b9001801', 'RTG 场景切换请求 setter 入口（x0=场景管理器, w1=场景号, x2=描述）：'
     'OBS_SCENEREQ 全程请求序列——教程期→黑屏期，谁在写 (场景号,描述)；原指令 str w1,[x0,#0x18]'),
    (0x131f50, 24, 'd10143ff', 'RTG 转场/淡出请求函数入口（x0=场景）：OBS_TRANSREQ 记录转场请求频率'),
    (0x131060, 25, 'a9ba7bfd', 'RTG 活动门卫入口（x0=场景）：OBS_GATE6 记录门卫被调频率与场景'),
    (0x131920, 26, 'a9be7bfd', 'RTG 进度累加入口（x0=场景）：OBS_PROGTICK 记录进度更新频率'),
    (0x509740, 27, 'a9bd7bfd', 'RTG 场景管理器入口（x0=场景管理器）：宿主记录管理器地址，供 DM_SCENEMGR 轮询当前/挂起场景'),
    (0x4dfb20, 28, 'a9be7bfd', 'RTG 作业蹦床入口（x0=作业包 {函数,参数,pool}）：OBS_JOBTRAMP 打每个 VM 作业的函数指针'),
    (0x4dfdf0, 29, 'a9bd7bfd', 'RTG std::thread spawn 入口（x0=线程体函数指针）：OBS_SPAWN 记录 boot 全部线程创建序列（初始化序列图）'),
    (0x4e1fe0, 6, '39403408', 'RTG 挂载链插入入口（x0=条目 this，[this+0xd] 标志门控）：OBS_MNTINS 记插入时序+条目名——安卓首故障空表根因（nnMain 入口有节点→0.5s 被清空）'),
    (0x4e1c60, 9, 'a9be7bfd', 'RTG 挂载链移除入口（x0=条目 this，虚表析构也达此）：OBS_MNTREM 记移除时序+条目名——同上'),
    (0x501eb0, 51, 'a9bc7bfd', 'v22D 检查点1：字体引擎 init 入口（Mac 在此 spawn×30+插入×3，安卓零命中——分叉定位）'),
    (0x4e03f0, 52, 'a9bd7bfd', 'v22D 检查点2：0x501eb0 内首个后续调用（收到=已过 GetSharedFont 重试环）'),
    (0x5144c0, 53, 'a9be7bfd', 'v22D 检查点3：分配器 init（收到=0x4e03f0 已过）'),
    (0x4dff10, 54, 'f9400008', 'v22D 检查点4：虚调用 [obj+0x28] thunk（收到=0x5144c0 已过；spawn 前最后一环）'),
    (0x4e0780, 55, '52884808', 'v22D 检查点5：重试环 sleep 入口（命中=GetSharedFont 失败重试）'),
    (0x50a6b0, 56, 'a9be7bfd', 'v22D 检查点6：FS 上下文函数入口（0x50cff0 第一个调用）'),
    (0x4e3ad0, 57, 'd10203ff', 'v22E：路径解析入口（x0=路径——打印游戏想开的文件；HLE OpenFile 从未被调到之谜）'),
    (0x4e36c0, 58, 'd10543ff', 'v22E：nn::fs::OpenFile 包装入口（x2=路径 w1=mode——确认是否到达）'),
    (0x4e39f0, 59, 'a9be7bfd', 'v22F：路径规范化（拼 mount 前缀）——OpenFile 前一环'),
    (0x4dfd00, 60, '91002000', 'v22F：fs 锁（OpenFile 前最后一步）'),
    (0x514e00, 61, 'a9be7bfd', 'v22H：ReadThread 启停控制虚方法入口（[x0+0x250]==0 才建预读线程——安卓零线程的最终门）'),
    (0x502500, 62, 'd10143ff', 'v22N：字体懒加载器入口（x0=字体 id；查 [0x1106350+id*8] 名字表为空即退出）'),
    (0x5021d0, 63, 'd10143ff', 'v22Q：字体注册器入口（x1=源名字串——名字缓冲空串之谜：源空 vs 拷贝/分配坏）'),
]}
BUILD_ID = d[0x40:0x50].hex()
cuts = list(CUTS.get(BUILD_ID, []))
cuts = [c if len(c) == 5 else (*c, '全') for c in cuts]          # 第 5 项作用域：全 | 仅宿主（仅宿主 = 真值模拟器不应用） | 5th item is the scope: 全 (all) | 仅宿主 (host only) (仅宿主 = not applied by the ground-truth emulator)
for off, ob, nb, why, scope in cuts:
    ob, nb = bytes.fromhex(ob), bytes.fromhex(nb)
    assert len(ob) == len(nb) and IMG[off:off + len(ob)] == ob, f'剪口原字节不符 {off:#x}：{bytes(IMG[off:off + len(ob)]).hex()}'
    IMG[off:off + len(nb)] = nb
# 键页下标固定；键页内容由装载器写入“实际键号 × 8”。
# Key page indices are fixed; the key page content is written by the loader as "actual key number × 8".
KEYS = {'K18': 508, 'KTLS': 509, 'KTLR': 510, 'KGATE': 511, 'KRES0': 506, 'KRES1': 507, 'KASPECT': 505, 'KASPECTUI': 504}   # KRES*：分辨率参数位；KASPECT：画面宽高比上限（见 PARAMS） | KRES*: resolution parameter slots; KASPECT: picture aspect ratio cap (see PARAMS)
KEY_INDEX = {k: i for i, k in enumerate(('K18', 'KTLS', 'KTLR', 'KGATE', 'KRES0', 'KRES1', 'KASPECT', 'KASPECTUI'))}

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
patch = bytearray(); fix = []; gates = []; stats = {}
def stat(k): stats[k] = stats.get(k, 0) + 1
def here(): return PATCH_OFF + len(patch)
def emit(w, kind=None):
    if kind: fix.append((len(patch), kind))
    patch.extend(struct.pack('<I', w & 0xFFFFFFFF))

def emit_key_addr(reg, kind):
    fix.append((len(patch), kind)); emit(ADRP(reg, here(), KEY_PAGE_OFF)); emit(LDRX(reg, reg, KEY_INDEX[kind]))

def emit_slot_addr(base, key_reg, kind):
    if ANDROID:
        # Android Q+: TLS_SLOT_APP contains this thread's host-owned guest slots.
        # TPIDR_EL0 remains the Bionic thread pointer; no private pthread layout.
        emit(LDRX(base, base, 2))
    emit_key_addr(key_reg, kind); emit(ADDR(base, base, key_reg))

# ---- 编码器 ----
# ---- Encoders ----
def B(frm, to):
    off = to - frm; assert -(1 << 27) <= off < (1 << 27) and off % 4 == 0
    return 0x14000000 | ((off >> 2) & 0x3FFFFFF)
def STP(t, t2, n, off): return 0xA9000000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def LDP(t, t2, n, off): return 0xA9400000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def STP_PRE(t, t2, n, off): return 0xA9800000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def LDP_POST(t, t2, n, off): return 0xA8C00000 | (((off // 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t
def STUR(t, n, off): return 0xF8000000 | ((off & 0x1FF) << 12) | (n << 5) | t   # stur xt,[xn,#off]（off 可负） | stur xt,[xn,#off] (off may be negative)
def LDUR(t, n, off): return 0xF8400000 | ((off & 0x1FF) << 12) | (n << 5) | t   # ldur xt,[xn,#off]
def LDRX(t, n, imm12=0): return 0xF9400000 | (imm12 << 10) | (n << 5) | t
def STRX(t, n, imm12=0): return 0xF9000000 | (imm12 << 10) | (n << 5) | t
def MRS_TPIDRRO(t): return (0xD53BD040 if ANDROID else 0xD53BD060) | t
def ADRP(dd, pc, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12; assert -(1 << 20) <= imm < (1 << 20)
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | dd
def ADDI(dd, n, imm12, sh=0): return 0x91000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd
def ADDR(dd, n, m): return 0x8B000000 | (m << 16) | (n << 5) | dd
def SUBI(dd, n, imm12, sh=0): return 0xD1000000 | (sh << 22) | (imm12 << 10) | (n << 5) | dd
def BR(n): return 0xD61F0000 | (n << 5)
def BLR(n): return 0xD63F0000 | (n << 5)
NOP = 0xD503201F

def dis(w, addr):
    l = list(md.disasm(struct.pack('<I', w), addr)); assert len(l) == 1, hex(w)
    return l[0]
def regset(ins):
    r, w = ins.regs_access()
    return {ins.reg_name(x) for x in r}, {ins.reg_name(x) for x in w}
def num(name):
    m = re.fullmatch(r'[xw](\d+)', name or ''); return int(m.group(1)) if m else None
def has18(ins): return re.search(r'\b[xw]18\b', ins.op_str) is not None
def regs_in(ins):
    s = {int(n) for n in re.findall(r'\b[xw](\d+)\b', ins.op_str)}
    try:
        rd, wr = regset(ins); s |= {num(n) for n in rd | wr} - {None}
    except Exception: pass
    return s
def pick(avoid, n=2):
    out = [r for r in range(0, 16) if r not in avoid and r != 18][:n]; assert len(out) == n; return out

def subst(ins, S):
    """x18/w18 → xS/wS：只改 4 个寄存器字段位置，用反汇编文本核对。 | x18/w18 → xS/wS: only the 4 register field positions are changed, verified against the disassembly text."""
    w = struct.unpack('<I', ins.bytes)[0]
    want = re.sub(r'\b([xw])18\b', lambda m: f'{m.group(1)}{S}', ins.op_str)
    pos = [p for p in (0, 5, 10, 16) if (w >> p) & 31 == 18]
    for k in range(len(pos), 0, -1):
        for c in combinations(pos, k):
            nw = w
            for p in c: nw = (nw & ~(31 << p)) | (S << p)
            try: dd = dis(nw, ins.address)
            except AssertionError: continue
            if dd.mnemonic == ins.mnemonic and dd.op_str == want: return nw
    raise RuntimeError(f'subst fail {ins.address:x} {ins.mnemonic} {ins.op_str}')

# ---- 各类改写块 ----
# ---- Rewrite blocks of each kind ----
def sp_delta(ins):
    """指令对 sp 的写回量（前变址 [sp, #i]! / 后变址 [sp], #i）；不写回 sp 返回 0。 | The amount an instruction writes back to sp (pre-index [sp, #i]! / post-index [sp], #i); returns 0 if sp is not written back."""
    m = re.search(r'\[sp, #(-?(?:0x)?[0-9a-f]+)\]!', ins.op_str)
    if m: return int(m.group(1), 0)
    m = re.search(r'\[sp\], #(-?(?:0x)?[0-9a-f]+)$', ins.op_str)
    if m: return int(m.group(1), 0)
    assert not re.match(r'(sp|wsp),', ins.op_str), f'writes sp: {ins.address:x} {ins.op_str}'
    return 0

def subst_sp(ins, word, reg):
    """Retarget original SP operands to a saved original-SP register.

    Register 31 also encodes ZR. Exhaustively verify the disassembly so that
    a ZR operand is never rewritten merely because it shares that encoding.
    """
    original = dis(word, ins.address)
    # MOV Xd,SP is ADD Xd,SP,#0. Retargeting the base yields ADD Xd,Xn,#0
    # (Capstone stops spelling that MOV); use the true register MOV alias.
    if original.mnemonic == 'mov' and original.op_str.endswith(', sp'):
        assert word & 0xffc003e0 == 0x910003e0 and (word >> 10 & 0xfff) == 0
        return 0xaa0003e0 | (reg << 16) | (word & 31)
    want = re.sub(r'\b(w?sp)\b', lambda m: ('w' if m.group(1) == 'wsp' else 'x') + str(reg), original.op_str)
    pos = [p for p in (0, 5, 10, 16) if (word >> p) & 31 == 31]
    for k in range(1, len(pos) + 1):
        for fields in combinations(pos, k):
            value = word
            for p in fields: value = (value & ~(31 << p)) | (reg << p)
            try: out = dis(value, ins.address)
            except AssertionError: continue
            if out.mnemonic == original.mnemonic and out.op_str == want: return value
    raise RuntimeError(f'Android SP substitution unsupported: {ins.address:x} {original.mnemonic} {original.op_str}')

def android_frame(ins, scratch_bytes=32):
    """Keep saved registers below every statically known original SP access."""
    floor = min(0, sp_delta(ins))
    from capstone.arm64 import ARM64_OP_MEM, ARM64_REG_SP, ARM64_REG_INVALID
    for operand in ins.operands:
        if operand.type == ARM64_OP_MEM and operand.mem.base == ARM64_REG_SP:
            assert operand.mem.index == ARM64_REG_INVALID, f'Android dynamic SP-indexed access unsupported: {ins.address:x} {ins.op_str}'
            floor = min(floor, operand.mem.disp)
    frame = (scratch_bytes - floor + 15) & ~15
    assert frame <= 4095, f'Android scratch frame too large: {ins.address:x} {frame}'
    return frame

def save_pair(s, t):
    emit(STP_PRE(s, t, 31, -16) if ANDROID else STP(s, t, 31, -16))

def restore_pair(s, t):
    emit(LDP_POST(s, t, 31, 16) if ANDROID else LDP(s, t, 31, -16))

def site_plain(ins):
    S, T = pick(regs_in(ins))
    dlt = sp_delta(ins)
    if ANDROID:
        assert dlt % 16 == 0, f'Android SP writeback loses 16-byte alignment: {ins.address:x} {dlt}'
        start = here()
        sp_used = re.search(r'\bw?sp\b', ins.op_str) is not None
        if sp_used:
            (U,) = pick(regs_in(ins) | {S, T}, 1)
            frame = android_frame(ins)
            emit(SUBI(31, 31, frame))
            emit(STP(S, T, 31, 0)); emit(STRX(U, 31, 2))
            emit(ADDI(U, 31, frame))
        else:
            frame, U = 16, None
            save_pair(S, T)
        emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
        word = subst(ins, S)
        if U is not None: word = subst_sp(ins, word, U)
        emit(word); emit(STRX(S, T))
        if U is not None: emit(LDRX(U, 31, 2))
        emit(LDP(S, T, 31, 0))
        assert 0 <= frame + dlt <= 4095, f'Android final SP adjustment unsupported: {ins.address:x} {frame + dlt}'
        if frame + dlt: emit(ADDI(31, 31, frame + dlt))
        emit(B(here(), ins.address + 4))
        return start
    save = -16 if dlt == 0 else -112          # 改 sp 的指令会写到 sp 下方，暂存放到保留区深处 | Instructions that modify sp write below sp, so the scratch save is placed deep in the reserved area
    assert dlt == 0 or dlt >= -96, f'sp delta {dlt} at {ins.address:x}'
    start = here()
    emit(STP(S, T, 31, save)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(subst(ins, S))
    emit(STRX(S, T))                 # 一律写回：capstone 会漏报写入（adds w18 实例） | Always write back: capstone can miss reporting a write (the adds w18 instance)
    emit(LDP(S, T, 31, save - dlt)); emit(B(here(), ins.address + 4))
    return start

def site_cond(ins):
    w = struct.unpack('<I', ins.bytes)[0]
    S, T = pick(regs_in(ins))
    tgt = ins.operands[-1].imm
    start = here()
    save_pair(S, T); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    m = ins.mnemonic
    taken = here() + 4 * 3
    off = (taken - here()) >> 2
    nw = ((w & 0xFF000000) | ((off & 0x7FFFF) << 5) | S) if m in ('cbz', 'cbnz') else ((w & 0xFFF80000) | ((off & 0x3FFF) << 5) | S)
    pc = here(); emit(nw)
    dd = dis(nw, pc); assert dd.mnemonic == m and dd.operands[-1].imm == taken
    restore_pair(S, T); emit(B(here(), ins.address + 4))
    assert here() == taken
    restore_pair(S, T); emit(B(here(), tgt))
    return start

def site_pcrel(ins):
    tgt = ins.operands[1].imm
    S, T = pick(set())
    start = here()
    save_pair(S, T); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18')   # S first holds the key-page offset.
    pc = here(); emit(ADRP(S, pc, tgt))
    if ins.mnemonic == 'adr': emit(ADDI(S, S, tgt & 0xFFF))
    emit(STRX(S, T))
    restore_pair(S, T); emit(B(here(), ins.address + 4))
    return start

def site_br(ins, base, count):
    """跳转表 br x18：目标 = base + 4i。x16 暂存红区 → 等距落地垫 → 恢复 x16 → 原目标。 | Jump table br x18: target = base + 4i. Stash x16 in the red zone → equally spaced landing pads → restore x16 → original target."""
    start = here()
    if ANDROID: emit(STP_PRE(16, 17, 31, -16))
    else: emit(STUR(16, 31, -16)); emit(STUR(17, 31, -24))
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16))
    emit(LDRX(17, 31, 1) if ANDROID else LDUR(17, 31, -24))
    padbase = start + 4 * 24
    delta = padbase - base
    op = ADDI if delta >= 0 else SUBI; dl = abs(delta)
    ops = []; hi, lo = dl >> 12, dl & 0xFFF
    while hi: h = min(hi, 0xFFF); ops.append(op(16, 16, h, 1)); hi -= h
    if lo: ops.append(op(16, 16, lo, 0))
    assert len(ops) <= 12
    for o in ops: emit(o)
    emit(BR(16))
    while here() < padbase: emit(NOP)
    tails = padbase + 4 * count
    for i in range(count): emit(B(here(), tails + 8 * i))
    for i in range(count):
        emit(LDP_POST(16, 17, 31, 16) if ANDROID else LDUR(16, 31, -16))
        emit(B(here(), base + 4 * i))
    return start

def site_sysreg_read(ins, kind):
    """mrs xN, tpidr_el0 / tpidrro_el0 → xN = 线程槽值。 | mrs xN, tpidr_el0 / tpidrro_el0 → xN = thread slot value."""
    N = ins.bytes[0] & 31
    if ANDROID and N == 18:
        return site_virtual_tp(ins, kind, 'K18')
    if ANDROID and N == 31:
        start = here(); emit(B(here(), ins.address + 4)); return start
    tmp = 16 if N != 16 else 17
    start = here()
    emit(STP_PRE(tmp, 31, 31, -16) if ANDROID else STUR(tmp, 31, -16))
    emit(MRS_TPIDRRO(N)); emit_slot_addr(N, tmp, kind); emit(LDRX(N, N))
    emit(LDP_POST(tmp, 31, 31, 16) if ANDROID else LDUR(tmp, 31, -16))
    emit(B(here(), ins.address + 4))
    return start

def site_tpidr_write(ins):
    N = struct.unpack('<I', ins.bytes)[0] & 31
    if ANDROID and N == 18:
        return site_virtual_tp(ins, 'K18', 'KTLS')
    (T,) = pick({N}, 1)
    start = here()
    tmp = next(x for x in (16, 17) if x not in (N, T))
    if ANDROID: emit(STP_PRE(T, tmp, 31, -16))
    else: emit(STUR(T, 31, -16)); emit(STUR(tmp, 31, -24))
    emit(MRS_TPIDRRO(T)); emit_slot_addr(T, tmp, 'KTLS'); emit(STRX(N, T))
    if ANDROID: emit(LDP_POST(T, tmp, 31, 16))
    else: emit(LDUR(tmp, 31, -24)); emit(LDUR(T, 31, -16))
    emit(B(here(), ins.address + 4))
    return start

def site_virtual_tp(ins, source_kind, destination_kind):
    """TP reads/writes involving guest x18 must never touch hardware x18."""
    S, T, U = pick({18}, 3)
    start = here()
    emit(STP_PRE(S, T, 31, -32)); emit(STRX(U, 31, 2))
    emit(MRS_TPIDRRO(T)); emit_slot_addr(T, U, source_kind); emit(LDRX(S, T))
    emit(MRS_TPIDRRO(T)); emit_slot_addr(T, U, destination_kind); emit(STRX(S, T))
    emit(LDRX(U, 31, 2)); emit(LDP_POST(S, T, 31, 32))
    emit(B(here(), ins.address + 4))
    return start

def emit_gate_call():
    """门块公共段（紧跟 stp x29,x30,[sp,#-16]! 之后、blr x30 之前）：x30 = 线程槽 KGATE 的门地址。
    键页取槽要借第二个寄存器，借 x29 后立即从栈上恢复：_dm_gate 存下的 x29 必须是客体原值——
    宿主按 [x29+8] 认调用者（技能绑定观测门 6、私有瞄准门 svc86）、崩溃回溯也走 x29 链
    （r24 方案 C 起借了不还，宿主拿到的 x29 是键页里的槽偏移，技能图标退回占位字）。
    Common section of a gate block (right after stp x29,x30,[sp,#-16]! and before blr x30): x30 = the gate address in thread slot KGATE.
        Fetching the slot through the key page needs a second register to be borrowed; x29 is borrowed and then immediately restored from the stack: the x29 saved by _dm_gate must be the guest's original value --
        the host identifies the caller by [x29+8] (skill binding observation gate 6, private aiming gate svc86), and the crash backtrace also walks the x29 chain
        (since r24 plan C it was borrowed and not returned, the x29 the host got was the slot offset from the key page, and skill icons fell back to placeholder text)."""
    emit(MRS_TPIDRRO(30)); emit_slot_addr(30, 29, 'KGATE'); emit(LDRX(30, 30)); emit(LDRX(29, 31))

def site_gate(ins, kind, arg):
    """svc / mrs cntpct_el0 → 宿主门。门地址取自线程槽 KGATE；门按返回点（本块 ldp 的地址）查 gates.tsv。 | svc / mrs cntpct_el0 → host gate. The gate address is taken from thread slot KGATE; the gate looks up gates.tsv by return point (the address of this block's ldp)."""
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(B(here(), ins.address + 4))
    gates.append((ret, kind, arg, ins.address))
    return start

def site_blredir(bl_off, oid):
    """BL 重定向门（中段观测）：bl target → bl 本块 → 宿主记录 → br target。
    LR 链保持：本块先保存 x29/x30 进栈再调宿主，恢复后 br target——目标返回时回到 bl_off+4（原语义）。
    BL-redirect gate (mid-function observation): bl target -> bl this block -> host record -> br target.
    The LR chain is preserved: save x29/x30, call the host, restore, then br target; when the target returns, it comes back to bl_off+4 (the original semantics)."""
    w, = struct.unpack_from('<I', IMG, bl_off)
    assert (w >> 26) == 0x25, f'not a bl at {bl_off:#x}'
    imm = (w & 0x3ffffff)
    if imm >= 0x2000000: imm -= 0x4000000
    target = bl_off + imm * 4
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(B(here(), target))          # 跳原目标 | jump to the original target
    gates.append((ret, 'obs', oid, bl_off))   # 门标识 = 返回点；观测编号 oid
    # BL 重写（不是 B！）：进块时 x30 = bl_off+4（BL 语义），块尾 ldp 恢复它，br target 后目标 ret 回 bl_off+4
    # BL rewrite (not B!): on entry x30 = bl_off+4 (BL semantics); the block-tail ldp restores it, and after br target the callee returns to bl_off+4
    imm26 = (start - bl_off) // 4
    assert -0x2000000 <= imm26 < 0x2000000, f'BLREDIR 超范围 {bl_off:#x}→{start:#x}'
    if imm26 < 0: imm26 += 0x4000000
    struct.pack_into('<I', IMG, bl_off, 0x94000000 | imm26)
    return start

def site_obs(off, oid, word):
    """观测门：入口原指令 → 进宿主门（arg = 观测编号）→ 恢复 x29/x30 → 执行原指令 → 跳回 off+4。
    门块前半与 site_gate 逐字同构；宿主只读寄存器、不改（dm_gate 回写的 x0–x8 即原值）。
    Observation gate: original entry instruction → enter the host gate (arg = observation number) → restore x29/x30 → execute the original instruction → jump back to off+4.
        The first half of the gate block is word-for-word isomorphic with site_gate; the host only reads registers and does not modify them (the x0–x8 written back by dm_gate are the original values)."""
    ins = dis(word, off)
    assert ins.mnemonic not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') and not ins.mnemonic.startswith('b.') \
        and not (ins.mnemonic.startswith('ldr') and '[' not in ins.op_str) and not has18(ins), f'观测门原指令不可搬 {off:#x} {ins.mnemonic} {ins.op_str}'
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(word)
    emit(B(here(), off + 4))
    gates.append((ret, 'obs', oid, off))
    return start

# ---- 独占窗口 ----
# ---- Exclusive windows ----
EXCL_LD = ('ldxr', 'ldaxr', 'ldxp', 'ldaxp', 'ldxrb', 'ldaxrb', 'ldxrh', 'ldaxrh')
EXCL_ST = ('stxr', 'stlxr', 'stxp', 'stlxp', 'stxrb', 'stlxrb', 'stxrh', 'stlxrh')
def find_windows(insl):
    out = []
    for i, ins in enumerate(insl):
        if ins.mnemonic not in EXCL_LD: continue
        j = i
        while j < len(insl) and insl[j].mnemonic not in EXCL_ST: j += 1
        if j >= len(insl) or j - i >= 16: continue
        if j + 1 < len(insl) and insl[j + 1].mnemonic in ('cbz', 'cbnz'): j += 1
        seg = insl[i:j + 1]
        if any(has18(x) for x in seg): out.append(seg)
    return out

def site_window(seg):
    used = set()
    for x in seg: used |= regs_in(x)
    S, T = pick(used)
    lo, hi = seg[0].address, seg[-1].address + 4
    start = here()
    U = None
    if ANDROID and any(re.search(r'\bw?sp\b', x.op_str) for x in seg):
        for x in seg:
            assert sp_delta(x) == 0, f'Android exclusive window writes SP: {x.address:x} {x.op_str}'
            assert android_frame(x) == 32, f'Android exclusive window negative SP access: {x.address:x} {x.op_str}'
        (U,) = pick(used | {S, T}, 1)
        emit(STP_PRE(S, T, 31, -32)); emit(STRX(U, 31, 2)); emit(ADDI(U, 31, 32))
    else: save_pair(S, T)
    emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    copy0 = here(); n = len(seg)
    exits = copy0 + 4 * n
    ex_targets = []; words = []
    for k, x in enumerate(seg):
        w = struct.unpack('<I', x.bytes)[0]
        if has18(x): w = subst(x, S)
        if U is not None and re.search(r'\bw?sp\b', x.op_str): w = subst_sp(x, w, U)
        m = x.mnemonic
        if m in ('cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
            tgt = x.operands[-1].imm
            if lo <= tgt < hi: newt = copy0 + (tgt - lo)
            else: ex_targets.append(tgt); newt = exits + 16 * len(ex_targets)   # 出口桩第 0 个留给顺序落出（见下），窗口外的目标从第 1 个起 | Exit stub 0 is reserved for the sequential fall-out (see below); targets outside the window start from stub 1
            pc = copy0 + 4 * k; off = (newt - pc) >> 2
            if m in ('tbz', 'tbnz'): w = (w & 0xFFF8001F) | ((off & 0x3FFF) << 5)
            else: w = (w & 0xFF00001F) | ((off & 0x7FFFF) << 5)
            dd = dis(w, pc); assert dd.operands[-1].imm == newt and dd.mnemonic == m
        else:
            assert m not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret') and not (m == 'ldr' and '[' not in x.op_str), hex(x.address)
        words.append(w)
    for w in words: emit(w)
    # 出口桩的次序：副本最后一条指令执行完是「顺序落出」，落进紧跟在副本后面的第 0 个出口桩——所以第 0 个必须是回 hi 的落出口，窗口外的跳转目标排在其后。
    # r40 及以前落出口排在最后：窗口里只要有跳到窗口外的条件跳转（比较交换的「比较不等」出口），写成功后就落进第一个外部出口 = 当成比较失败处理
    # （一次操作做两遍 / 无锁表被多弹）。暗黑有 6 个这样的窗口（main 0x1848eb8 / 0x1848f28 / 0x1848f84，sdk 0x2ea0fc / 0x2ea4cc / 0x2ea644），
    # 只有跳回窗口内的 cbnz 的窗口不受影响。等价验证原先随机抽不到「比较相等」那一向，所以一直是绿的（equiv.py 已补窗口分支双向强制）。
    # Order of the exit stubs: when the last instruction of the copy has executed, control "falls out sequentially" into exit stub 0, which immediately follows the copy -- so stub 0 must be the fall-out exit back to hi, and jump targets outside the window come after it.
    # Up to r40 the fall-out exit was placed last: whenever a window contained a conditional jump to outside the window (the "compare not equal" exit of a compare-and-swap), a successful write fell into the first external exit = handled as a failed compare
    # (one operation done twice / a lock-free list popped more than once). The game has 6 such windows (main 0x1848eb8 / 0x1848f28 / 0x1848f84, sdk 0x2ea0fc / 0x2ea4cc / 0x2ea644);
    # windows whose only branch is a cbnz back into the window are not affected. The equivalence check used to never draw the "compare equal" direction at random, so it stayed green (equiv.py now forces both directions of window branches).
    for t in [hi] + ex_targets:
        emit(STRX(S, T))
        if U is not None:
            emit(LDRX(U, 31, 2)); emit(LDP_POST(S, T, 31, 32)); emit(B(here(), t))
        else:
            restore_pair(S, T); emit(B(here(), t)); emit(NOP)
    return start, lo, hi

# ---- 函数边界（eh_frame_hdr）----
# ---- Function boundaries (eh_frame_hdr) ----
m0 = struct.unpack_from('<I', IMG, 4)[0]
assert IMG[m0:m0 + 4] == b'MOD0'
ehs = m0 + struct.unpack_from('<i', IMG, m0 + 16)[0]
def rd(o, enc):
    fmt, app = enc & 0xF, enc & 0x70
    v, sz = {0x3: (struct.unpack_from('<I', IMG, o)[0], 4), 0xB: (struct.unpack_from('<i', IMG, o)[0], 4),
             0x4: (struct.unpack_from('<Q', IMG, o)[0], 8)}[fmt]
    if app == 0x10: v += o
    if app == 0x30: v += ehs
    return v, sz
_, penc, cenc, tenc = IMG[ehs:ehs + 4]
o = ehs + 4
_, s = rd(o, penc); o += s
nfde, s = rd(o, cenc); o += s
starts = set()
for _ in range(nfde):
    a, s = rd(o, tenc); o += s
    _, s = rd(o, tenc); o += s
    starts.add(a)
starts = sorted(x for x in starts if x < TEXT_END)

# ---- 线性扫描找全部改写点（不靠寄存器读写报告）----
# ---- Linear scan to find all rewrite points (not relying on register read/write reports) ----
kinds = {}
for o in range(0, TEXT_END, 4):
    w = struct.unpack_from('<I', IMG, o)[0]
    if w & 0xFFE0001F == 0xD4000001: kinds[o] = ('svc', (w >> 5) & 0xFFFF); continue
    if w & 0xFFFFFFE0 == 0xD53BD040: kinds[o] = ('tpidr_r', None); continue
    if w & 0xFFFFFFE0 == 0xD51BD040: kinds[o] = ('tpidr_w', None); continue
    if w & 0xFFFFFFE0 == 0xD53BD060: kinds[o] = ('tpidrro_r', None); continue
    if w & 0xFFFFFFE0 == 0xD53BE020: kinds[o] = ('cntpct', w & 31); continue
    if w & 0xFFFFFFE0 == 0xD53BE040: kinds[o] = ('cntvct', w & 31); continue
    for ins in md.disasm(IMG[o:o + 4], o):
        if has18(ins): kinds[o] = ('x18', None)

# 按函数取指令表，找窗口与跳转表
# Take the instruction table per function, find the windows and jump tables
import bisect
def func_of(a):
    i = bisect.bisect_right(starts, a) - 1
    return (starts[i], starts[i + 1] if i + 1 < len(starts) else TEXT_END) if i >= 0 else (a, a + 4)
done = set(); windows = []; jumps = {}
funcs = sorted({func_of(a) for a, (k, _) in kinds.items() if k == 'x18'})
for fa, fb in funcs:
    insl = list(md.disasm(bytes(IMG[fa:fb]), fa))
    ws = find_windows(insl)
    for wseg in ws:
        windows.append(wseg); done |= {x.address for x in wseg}
    for j, ins in enumerate(insl):
        if ins.mnemonic == 'br' and ins.op_str == 'x18':
            base = width = None
            ctx = insl[max(0, j - 8):j]
            for p in reversed(ctx):
                if p.mnemonic == 'adr' and p.op_str.startswith('x18,'): base = p.operands[1].imm; break
            for p in reversed(ctx):
                if p.mnemonic == 'add' and p.op_str.startswith('x18, x18,') and 'lsl #2' in p.op_str:
                    idx = num(p.reg_name(p.operands[2].reg))
                    for q in reversed(ctx):
                        if q.mnemonic in ('ldrb', 'ldrsb') and num(q.reg_name(q.operands[0].reg)) == idx: width = 256
                        if q.mnemonic in ('ldrh', 'ldrsh') and num(q.reg_name(q.operands[0].reg)) == idx: width = 65536
                        if width: break
                    break
            assert base and width, f'jump table {ins.address:x}'
            jumps[ins.address] = (base, width)
    # 不许有跳转落进窗口中间
    # No jump may land in the middle of a window
    inwin = {x.address for wseg in ws for x in wseg}; heads = {wseg[0].address for wseg in ws}
    for x in insl:
        if x.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or x.mnemonic.startswith('b.'):
            t = x.operands[-1].imm
            assert not (t in inwin and t not in heads), f'branch into window {x.address:x}'

def put_branch(at, to): struct.pack_into('<I', IMG, at, B(at, to))
for wseg in windows:
    st, lo, hi = site_window(wseg); stat('window'); put_branch(lo, st)
for a in sorted(kinds):
    if a in done: continue
    k, arg = kinds[a]
    ins = dis(struct.unpack_from('<I', IMG, a)[0], a)
    if k == 'x18':
        m = ins.mnemonic
        if m == 'br': st = site_br(ins, *jumps[a]); stat('br')
        elif m in ('cbz', 'cbnz', 'tbz', 'tbnz'): st = site_cond(ins); stat('cond')
        elif m in ('adr', 'adrp'): st = site_pcrel(ins); stat('pcrel')
        elif m in ('blr', 'ret', 'b', 'bl') or (m == 'ldr' and '[' not in ins.op_str): raise RuntimeError(f'{a:x} {m} {ins.op_str}')
        else: st = site_plain(ins); stat('plain')
    elif k == 'tpidr_r': st = site_sysreg_read(ins, 'KTLS'); stat(k)
    elif k == 'tpidrro_r': st = site_sysreg_read(ins, 'KTLR'); stat(k)
    elif k == 'tpidr_w': st = site_tpidr_write(ins); stat(k)
    elif k == 'svc': st = site_gate(ins, 'svc', arg); stat(k)
    elif k in ('cntpct', 'cntvct'): st = site_gate(ins, k, arg); stat(k)
    put_branch(a, st)

obs = list(OBS.get(BUILD_ID, []))
# BLREDIR 表：主循环关键中段调用点（90 目标里非函数头的）→ OID 30+（demo diff 用）
BLREDIR = {'03ff4a3bec96bde3a2e37ce3aa4caa0c': [
    (0x6a00, 30, '主循环 tick A（调 0x50d0b0）'),
    (0x6c0c, 31, '子系统循环首调用'),
    (0x6c64, 32, '0x3f10 消息查询调用'),
    (0x6a78, 33, '主循环转态查询'),
    # 类型表注册链：工厂 0x488940 五调用点（w0=23 → 造含 0x250670 处理器的对象）+ 0x588e0 两调用点（填表）
    (0x2ab4a4, 34, '工厂调用 A（w0=类型）'),
    (0x2ab8f0, 35, '工厂调用 B'),
    (0x2abd50, 36, '工厂调用 C'),
    (0x2ac1b0, 37, '工厂调用 D'),
    (0x2ac604, 38, '工厂调用 E'),
    (0x25076c, 39, '0x588e0(8) 类型表注册'),
    (0x25077c, 40, '0x588e0(w21) 类型表注册'),
    (0x507f8, 41, '0x62db0 场景类型查询（w1=场景号）'),
    (0x31fa0c, 42, '0x320640 装载开始（x0=manager x1=x19）'),
    (0x5846c, 43, 'manager 经 0x636c0/0x636e4 表初始化后的下一条 bl：宿主补注册类型（DM_TYPEREG）'),
    # 旗标门：0x250670 处理器查 flags[9]/[0xa]/[0xb]（0x506c50 bit0）全失败 → [mgr+0x1ce4] 永不置 1
    # → 情景注册（0x628a0 写类型表）被跳过 → 场景 2 造基类空壳 → 黑屏。门放在检查的 BL 上，
    # 回调先置位旗标字节再放行原调用——时序天然正确（DM_FLAGREG 开）
    (0x250710, 44, '旗标[9] 检查前置位（DM_FLAGREG）'),
    (0x25072c, 45, '旗标[0xa] 检查前置位（DM_FLAGREG）'),
    (0x250748, 46, '旗标[0xb] 检查前置位（DM_FLAGREG）'),
    (0x2506a0, 47, '情景处理器 0x250670 入口（bl 0x636c0）：x1=消息里的情景号'),
    (0x5b568, 48, '情景注册 0x628a0 调用（x0=表 x1=项 x2=槽 x3=载荷）'),
    (0x5b21c, 49, '情景表重置 bl 0x626c0'),
    (0x2ab600, 50, '基类空壳场景 ctor 0x2ab4d0 调用（x0=新场景对象）——抓空壳创建现场'),
]}
for bl_off, oid, why in BLREDIR.get(BUILD_ID, []):
    w, = struct.unpack_from('<I', IMG, bl_off)
    assert (w >> 26) == 0x25, f'BLREDIR {bl_off:#x} 不是 bl'
    site_blredir(bl_off, oid); stat('blredir')   # BL 重写在 site_blredir 内部 | the BL rewrite happens inside site_blredir

for off, oid, ow, why in obs:
    w = struct.unpack_from('<I', IMG, off)[0]
    assert f'{w:08x}' == ow, f'观测门原指令不符 {off:#x}：{w:08x}'
    # 只许函数入口：dm_gate 只回写 x0–x8 / x30，宿主 C 代码可能改 x9–x17；入口处这些按 AAPCS64 不承载值（参数只在 x0–x7 / x8）
    # Function entries only: dm_gate writes back only x0–x8 / x30, and host C code may modify x9–x17; at entry these carry no values under AAPCS64 (arguments are only in x0–x7 / x8)
    assert off in set(starts) or (oid == 6 and off == 0x43ecc8), f'private obs point not approved {off:#x}'
    assert off < TEXT_END and off not in kinds and not any(lo <= off < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'观测门与改写点冲突 {off:#x}'
    put_branch(off, site_obs(off, oid, w)); stat('obs')

os.makedirs(outdir, exist_ok=True)
P = lambda ext: os.path.join(outdir, f'{NAME}.{ext}')
open(P('patched.bin'), 'wb').write(IMG + bytearray(PATCH_OFF - len(IMG)) + patch)
import hashlib
with open(os.path.join(outdir, 'build_src.sha256'), 'w') as f:
    f.write(hashlib.sha256(open(os.path.abspath(__file__), 'rb').read()).hexdigest() + '  patch_nso.py\n')
with open(P('fixups.tsv'), 'w') as f:
    if ANDROID: f.write('# ANDROID_APP_SLOT\t0x2\n')
    f.write(f'# PATCH_OFF\t{PATCH_OFF:#x}\n# KEY_PAGE_OFF\t{KEY_PAGE_OFF:#x}\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t{TEXT_END:#x}\n# RO_OFF\t{RO_OFF:#x}\n# DATA_OFF\t{DATA_OFF:#x}\n')
    for k in KEY_INDEX: f.write(f'# KEY_{k}_IDX\t{KEY_INDEX[k]}\n')
with open(P('gates.tsv'), 'w') as f:
    for ret, k, arg, orig in gates: f.write(f'{ret:#x}\t{k}\t{arg}\t{orig:#x}\n')
with open(P('sites.tsv'), 'w') as f:
    for a in sorted(kinds):
        if a not in done: f.write(f'{a:#x}\t{kinds[a][0]}\n')
    for wseg in windows: f.write(f'{wseg[0].address:#x}\tW\t{wseg[-1].address + 4:#x}\n')
    for a, (b, c) in jumps.items(): f.write(f'{a:#x}\tJ\t{b:#x}\t{c}\n')
with open(P('cuts.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, ob, nb, why, scope in cuts: f.write(f'{off:#x}\t{ob}\t{nb}\t{scope}\t{why}\n')   # 列：偏移 原字节 新字节 作用域 理由 | Columns: offset, original bytes, new bytes, scope, reason
with open(P('obs.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, oid, ow, why in obs: f.write(f'{off:#x}\t{oid}\t{ow}\t{why}\n')
print(NAME, stats, 'cuts', len(cuts), 'obs', len(obs), 'patch', len(patch), 'PATCH_OFF', hex(PATCH_OFF), 'slot_refs', len(fix), 'gates', len(gates))

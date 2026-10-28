#!/usr/bin/env python3
"""Generates modules/livecomp/raw14-max-asm.h (LCB2 select-max kernels in ARM
assembly). Run it after changing the tables; the result is committed."""
# SWAR select-max for packed RAW14 (2-block pairs), ARMv5TE.
P=[[0,14,28],[10,24],[6,20],[2,16,30],[12,26],[8,22],[4,18]]   # field start offsets (from MSB) per stream word
H=[sum(1<<(31-p) for p in ps) for ps in P]
def gen():
    L=[]
    A=L.append
    # r0 d, r1 s, r2 pairs, r3 any, r4 a, r5 b, r6..r10 tmp, r11 changed, r12 tmp, lr table, sp: L[7]
    A("push {r4-r11,lr}")
    A("sub sp,sp,#32")
    A("mov r11,#0")
    A("cmp r2,#0")
    A("beq 9f")
    A("1:")
    A("adr lr,8f")
    A("mov r3,#0")
    # fast path: words 6..0, borrow chain
    for n,i in enumerate(range(6,-1,-1)):
        A("ldmia lr!,{r9,r10}")                     # H_i, ~H_i (stream domain)
        A(f"ldr r4,[r0,#{4*i}]"); A(f"ldr r5,[r1,#{4*i}]")
        A("orr r6,r9,r4,ror #16")                   # t = A|H
        A("and r7,r10,r5,ror #16")                  # u = B&~H
        A("subs r8,r6,r7" if n==0 else "sbcs r8,r6,r7")
        A("eor r6,r4,r5")                           # e = a^b (native)
        A("bic r7,r5,r4")                           # f = b&~a (native)
        A("orr r8,r8,r6,ror #16")                   # n = d | e
        A("bic r8,r8,r7,ror #16")                   # k = n & ~f
        A("bic r12,r9,r8")                          # L = H & ~k: field b>a
        A(f"str r12,[sp,#{4*i}]")
        A("orr r3,r3,r12")
    A("cmp r3,#0")
    A("beq 3f")
    # slow path: M = (L<<1) - (L>>13) over the 224-bit stream (mod 2^224), select
    A("add r11,r11,#1")
    A("mov r9,#0")                                  # L_{i+1} (none for word 6)
    A("ldr r10,[sp,#24]")                           # L_6
    for n,i in enumerate(range(6,-1,-1)):
        # r10 = L_i, r9 = L_{i+1}; load L_{i-1} into r8
        if i>0: A(f"ldr r8,[sp,#{4*(i-1)}]")
        A("mov r6,r10,lsl #1"); A("orr r6,r6,r9,lsr #31")      # (L<<1)_i
        A("mov r7,r10,lsr #13")
        if i>0: A("orr r7,r7,r8,lsl #19")                       # (L>>13)_i
        A("subs r12,r6,r7" if n==0 else "sbcs r12,r6,r7")       # M_i (stream domain)
        A("teq r12,#0")
        A(f"ldrne r4,[r0,#{4*i}]"); A(f"ldrne r5,[r1,#{4*i}]")
        A("eorne r6,r4,r5"); A("andne r6,r6,r12,ror #16"); A("eorne r6,r4,r6")
        A(f"strne r6,[r0,#{4*i}]")
        if i>0: A("mov r9,r10"); A("mov r10,r8")
    A("3:")
    A("add r0,r0,#28"); A("add r1,r1,#28")
    A("subs r2,r2,#1")
    A("bne 1b")
    A("9:")
    A("mov r0,r11")
    A("add sp,sp,#32")
    A("pop {r4-r11,pc}")
    A("8:")
    for i in range(6,-1,-1):
        A(f".word 0x{H[i]:08x}"); A(f".word 0x{(~H[i])&0xffffffff:08x}")
    return L
def gen_field():
    # per-field compare, branch-free (conditional execution), 150 instr/pair
    L=[];A=L.append
    A("push {r4-r11,lr}");A("mov r11,#0");A("cmp r2,#0");A("beq 9f")
    A("mov r3,#0x3F00");A("orr r3,r3,#0xFF")
    plan=[([0,14],28),([10],24),([6],20),([2,16],30),([12],26),([8],22),([4,18],None)]
    A("1:");A("mov r10,#0")
    A("ldr r4,[r0]");A("ldr r5,[r1]");A("mov r4,r4,ror #16");A("mov r5,r5,ror #16")
    cur=('r4','r5');nxt=('r6','r7');m='r8';mn='r9';first=True
    for i,(inner,cross) in enumerate(plan):
        a,b=cur
        if cross is not None:
            na,nb=nxt
            A(f"ldr {na},[r0,#{4*(i+1)}]");A(f"ldr {nb},[r1,#{4*(i+1)}]")
            A(f"mov {na},{na},ror #16");A(f"mov {nb},{nb},ror #16")
        for p in inner:
            sh=18-p;op=f"r3,lsl #{sh}" if sh else "r3"
            if first: A(f"cmp {a},{b}");A(f"movlo {m},{op}");A(f"movhs {m},#0");first=False
            else:
                A(f"mov r12,{a},lsl #{p}");A(f"cmp r12,{b},lsl #{p}");A(f"orrlo {m},{m},{op}")
        if cross is not None:
            p=cross
            A(f"mov r12,{a},lsl #{p}");A(f"cmp r12,{b},lsl #{p}");A(f"cmpeq {na},{nb}")
            A(f"orrlo {m},{m},r3,lsr #{p-18}");A(f"movlo {mn},r3,lsl #{50-p}");A(f"movhs {mn},#0")
        A(f"teq {m},#0")
        A(f"eorne r12,{a},{b}");A(f"andne r12,r12,{m}");A(f"eorne r12,{a},r12");A("movne r12,r12,ror #16")
        A(f"strne r12,[r0,#{4*i}]");A(f"orr r10,r10,{m}")
        if cross is not None: cur,nxt=nxt,cur;m,mn=mn,m
    A("cmp r10,#0");A("addne r11,r11,#1")
    A("add r0,r0,#28");A("add r1,r1,#28");A("subs r2,r2,#1");A("bne 1b")
    A("9:");A("mov r0,r11");A("pop {r4-r11,pc}")
    return L
def fn(name,L,comment):
    body="\n".join('        "'+l+'\\n"' for l in L)
    return comment+'static __attribute__((naked,noinline,unused)) unsigned '+name+'(uint32_t *d,const uint32_t *s,unsigned pairs)\n{\n    asm volatile(\n'+body+'\n    );\n}\n'
L=gen()
SWAR_DOC='''/* SWAR: see the file comment. */\n'''
FIELD_DOC='''/* Per field: CMP of the field shifted to the top (cross fields: CMP of the
 * high part, CMPEQ of the next word on a tie), conditional ORR of the
 * field mask (F=0x3FFF in r3, shifted operand), conditional select and
 * store per word. No branches except the loop; 150 instructions per pair. */\n'''
src='''/* LCB2 select-max kernels in ARM assembly (ARMv5TE, ARM state), two methods.
 * lc_raw14_max_swar (whole-pair SWAR) and lc_raw14_max_field (per field).
 * lc_raw14_max_swar:
 * Same result as lc_raw14_max_sel; different method: SWAR over the whole
 * 224-bit stream of a 2-block pair (7 words, 16 fields of 14 bits).
 *   H = top bit of every field (stream domain, word i = ror16(native i)).
 *   D = (A|H) - (B&~H)  as a 224-bit subtraction (SUBS/SBCS from the last
 *       word): no borrow crosses a field, the H bit of D is 1 iff the low
 *       13 bits of a >= those of b.
 *   L = H & (b&~a | ~(a^b)&~D): the top bit of every field where b > a.
 *       Bitwise steps run on the native words (rotation commutes with
 *       them), only the subtraction needs the stream order.
 * Fast path: L == 0 for the whole pair (no sample brighter): nothing is
 * written. Otherwise M = (L<<1) - (L>>13) over the stream (mod 2^224) sets
 * all 14 bits of every such field, and a ^= (a^b)&M per word. Returns the
 * number of pairs that changed (as lc_raw14_max_sel). Generated by
 * build_tools/gen_raw14_max_asm.py; checked bit-exact against the reference
 * on an ARM interpreter by test_livecomp_raw14_asm.py.
 * Camera build only; host builds use the C kernels. */
#if defined(__ARM__) || defined(__arm__)
'''+fn('lc_raw14_max_swar',L,SWAR_DOC)+fn('lc_raw14_max_field',gen_field(),FIELD_DOC)+'''#endif
'''
import sys,pathlib
out=pathlib.Path(__file__).resolve().parents[1]/'modules/livecomp/raw14-max-asm.h'
out.write_text(src);print('wrote',out)
fast=0;inl=False


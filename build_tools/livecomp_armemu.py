"""Minimal ARM (ARMv5TE, ARM state) interpreter for kernel checks: data
processing, LDR/STR (word/byte/halfword imm), LDM/STM, B/BL/BX, MUL/MLA,
CLZ. Counts executed instructions, cycles (rough ARM946 model) and taken
branches."""
import struct
M=0xffffffff
class CPU:
    def __init__(s,mem,base):
        s.mem=mem;s.base=base;s.r=[0]*16;s.N=s.Z=s.C=s.V=0;s.count=0;s.taken=0;s.cycles=0
    def rd(s,a):
        o=a-s.base;return struct.unpack_from('<I',s.mem,o)[0]
    def wr(s,a,v):
        struct.pack_into('<I',s.mem,a-s.base,v&M)
    def rdh(s,a): return struct.unpack_from('<H',s.mem,a-s.base)[0]
    def wrh(s,a,v): struct.pack_into('<H',s.mem,a-s.base,v&0xffff)
    def cond(s,c):
        N,Z,C,V=s.N,s.Z,s.C,s.V
        return [Z,not Z,C,not C,N,not N,V,not V,C and not Z,(not C) or Z,N==V,N!=V,(not Z) and N==V,Z or N!=V,True,False][c]
    def shift(s,v,t,a,cin):
        if t==0:
            if a==0: return v,cin
            if a>=32: return 0,(v&1 if a==32 else 0)
            return (v<<a)&M,(v>>(32-a))&1
        if t==1:
            if a==0 or a==32: return 0,(v>>31)&1
            if a>32: return 0,0
            return v>>a,(v>>(a-1))&1
        if t==2:
            if a==0 or a>=32: return (M if v>>31 else 0),(v>>31)&1
            sv=v-(1<<32) if v>>31 else v
            return (sv>>a)&M,(v>>(a-1))&1
        if a==0: return v,cin
        a&=31
        if a==0: return v,(v>>31)&1
        return ((v>>a)|(v<<(32-a)))&M,(v>>(a-1))&1
    def reg(s,n): return (s.r[15]+8)&M if n==15 else s.r[n]
    def run(s,pc,lr_stop=0xdead0000,maxn=10**9):
        s.r[15]=pc;s.r[14]=lr_stop
        while s.r[15]!=lr_stop:
            i=s.rd(s.r[15]);s.count+=1;s.cycles+=1
            c=i>>28;npc=s.r[15]+4
            if not s.cond(c): s.r[15]=npc;continue
            if (i>>25)&7==5:   # B/BL
                off=i&0xffffff
                if off&0x800000: off-=1<<24
                if (i>>24)&1: s.r[14]=npc
                s.r[15]=(s.r[15]+8+off*4)&M;s.taken+=1;s.cycles+=2;continue
            if i&0x0ffffff0==0x012fff10:  # BX
                s.r[15]=s.r[i&15]&~1;s.taken+=1;s.cycles+=2;continue
            if i&0x0fff0ff0==0x016f0f10:  # CLZ
                v=s.r[i&15];s.r[(i>>12)&15]=32-v.bit_length();s.r[15]=npc;continue
            if i&0x0fc000f0==0x00000090:  # MUL/MLA
                a=(i>>21)&1;rd=(i>>16)&15;rn=(i>>12)&15;rs=(i>>8)&15;rm=i&15
                v=(s.r[rm]*s.r[rs]+(s.r[rn] if a else 0))&M;s.r[rd]=v
                if (i>>20)&1: s.N=v>>31;s.Z=int(v==0)
                s.r[15]=npc;s.cycles+=2;continue
            if (i>>25)&7==4:   # LDM/STM
                P=(i>>24)&1;U=(i>>23)&1;W=(i>>21)&1;L=(i>>20)&1;rn=(i>>16)&15;rl=i&0xffff
                regs=[k for k in range(16) if rl>>k&1];n=len(regs);base=s.r[rn]
                if U: a=base+4 if P else base
                else: a=base-4*n+(0 if P else 4)
                for k in regs:
                    if L: s.r[k]=s.rd(a)
                    else: s.wr(a,s.reg(k))
                    a+=4
                if W: s.r[rn]=(base+4*n if U else base-4*n)&M
                s.cycles+=n
                if L and rl>>15&1: s.r[15]&=~1;s.taken+=1;continue
                s.r[15]=npc;continue
            if (i>>26)&3==1:   # LDR/STR
                I=(i>>25)&1;P=(i>>24)&1;U=(i>>23)&1;B=(i>>22)&1;W=(i>>21)&1;L=(i>>20)&1
                rn=(i>>16)&15;rd=(i>>12)&15
                if I: off,_=s.shift(s.r[i&15],(i>>5)&3,(i>>7)&31,s.C)
                else: off=i&0xfff
                base=s.reg(rn);ea=(base+off if U else base-off)&M
                a=ea if P else base
                if L:
                    if B: v=s.mem[a-s.base]
                    else: v=s.rd(a)
                    s.r[rd]=v;s.cycles+=1
                else:
                    if B: s.mem[a-s.base]=s.reg(rd)&0xff
                    else: s.wr(a,s.reg(rd))
                if (not P) or W:
                    if not (L and rd==rn): s.r[rn]=ea
                if L and rd==15: s.taken+=1;continue
                s.r[15]=npc;continue
            if i&0x0e000090==0x00000090 and (i>>5)&3:  # LDRH/STRH etc
                P=(i>>24)&1;U=(i>>23)&1;Ib=(i>>22)&1;W=(i>>21)&1;L=(i>>20)&1;rn=(i>>16)&15;rd=(i>>12)&15
                sh=(i>>5)&3
                off=((i>>4)&0xf0)|(i&15) if Ib else s.r[i&15]
                base=s.reg(rn);ea=(base+off if U else base-off)&M;a=ea if P else base
                if sh==1:
                    if L: s.r[rd]=s.rdh(a)
                    else: s.wrh(a,s.reg(rd))
                elif sh==2 and L: v=s.mem[a-s.base];s.r[rd]=(v-256)&M if v&0x80 else v
                elif sh==3 and L: v=s.rdh(a);s.r[rd]=(v-65536)&M if v&0x8000 else v
                elif sh==2 and not L:  # LDRD
                    s.r[rd]=s.rd(a);s.r[rd+1]=s.rd(a+4)
                elif sh==3 and not L:  # STRD
                    s.wr(a,s.r[rd]);s.wr(a+4,s.r[rd+1])
                else: raise Exception('halfword op %08x'%i)
                if (not P) or W: s.r[rn]=ea
                s.r[15]=npc;continue
            if (i>>26)&3==0:   # data processing
                I=(i>>25)&1;op=(i>>21)&15;S=(i>>20)&1;rn=(i>>16)&15;rd=(i>>12)&15
                if I:
                    imm=i&0xff;rot=((i>>8)&15)*2
                    b=((imm>>rot)|(imm<<(32-rot)))&M if rot else imm
                    sc=(b>>31)&1 if rot else s.C
                else:
                    rm=s.reg(i&15);t=(i>>5)&3
                    if (i>>4)&1: a=s.r[(i>>8)&15]&0xff;b,sc=s.shift(rm,t,a,s.C) if a else (rm,s.C)
                    else:
                        a=(i>>7)&31
                        if a==0 and t==3: b=((s.C<<31)|(rm>>1));sc=rm&1
                        else: b,sc=s.shift(rm,t,a if a or t==0 else 32,s.C)
                a=s.reg(rn);C=s.C;V=s.V
                if op==0: v=a&b;C=sc
                elif op==1: v=a^b;C=sc
                elif op==2: r=a-b;v=r&M;C=int(a>=b);V=((a^b)&(a^v))>>31
                elif op==3: r=b-a;v=r&M;C=int(b>=a);V=((b^a)&(b^v))>>31
                elif op==4: r=a+b;v=r&M;C=int(r>M);V=((~(a^b))&(a^v)&M)>>31
                elif op==5: r=a+b+s.C;v=r&M;C=int(r>M);V=((~(a^b))&(a^v)&M)>>31
                elif op==6: r=a-b-(1-s.C);v=r&M;C=int(r>=0);V=((a^b)&(a^v))>>31
                elif op==7: r=b-a-(1-s.C);v=r&M;C=int(r>=0);V=((b^a)&(b^v))>>31
                elif op==8: v=a&b;C=sc
                elif op==9: v=a^b;C=sc
                elif op==10: r=a-b;v=r&M;C=int(a>=b);V=((a^b)&(a^v))>>31
                elif op==11: r=a+b;v=r&M;C=int(r>M);V=((~(a^b))&(a^v)&M)>>31
                elif op==12: v=a|b;C=sc
                elif op==13: v=b;C=sc
                elif op==14: v=a&~b&M;C=sc
                else: v=~b&M;C=sc
                if S or 8<=op<=11:
                    s.N=v>>31;s.Z=int(v==0);s.C=C;s.V=V
                if not (8<=op<=11):
                    if rd==15: s.r[15]=v&~3;s.taken+=1;s.cycles+=2;continue
                    s.r[rd]=v
                s.r[15]=npc;continue
            raise Exception('unknown %08x at %08x'%(i,s.r[15]))

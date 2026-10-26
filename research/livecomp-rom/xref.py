import struct,sys
d=open('/tmp/claude-0/rom/ROM1.BIN','rb').read(); base=0xff000000
n=len(d)//4; W=struct.unpack('<%dI'%n,d[:n*4])
def ror(v,r): return ((v>>r)|(v<<(32-r)))&0xffffffff
def pcrel_targets():
    out={}
    for i,w in enumerate(W):
        if (w&0x0fff0000)==0x028f0000 or (w&0x0fff0000)==0x024f0000:   # ADD/SUB rd, pc, #imm
            imm=ror(w&0xff,2*((w>>8)&0xf)); pc=base+4*i+8
            t=pc+imm if (w&0x0fff0000)==0x028f0000 else pc-imm
            out.setdefault(t&0xffffffff,[]).append(base+4*i)
        if (w&0x0f7f0000)==0x051f0000:  # LDR rd,[pc,#+-imm]
            imm=w&0xfff; pc=base+4*i+8; a=pc+imm if w&0x00800000 else pc-imm
            if base<=a<base+len(d): out.setdefault(W[(a-base)//4],[]).append(base+4*i)
    return out
if __name__=='__main__':
    import pickle; pickle.dump(pcrel_targets(),open('/tmp/claude-0/re/xref.pkl','wb'))

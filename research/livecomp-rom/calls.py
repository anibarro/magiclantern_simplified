import struct,sys
d=open('/tmp/claude-0/rom/ROM1.BIN','rb').read(); base=0xff000000
n=len(d)//4; W=struct.unpack('<%dI'%n,d[:n*4])
def bl_target(i,w):
    if (w&0x0f000000)!=0x0b000000: return None
    off=w&0xffffff
    if off&0x800000: off-=0x1000000
    return (base+4*i+8+off*4)&0xffffffff
def callers(t):
    return [i for i,w in enumerate(W) if bl_target(i,w)==t]
def regval(i,reg,back=8):
    # find last 'mov reg,#imm' before i
    for k in range(i-1,max(0,i-back),-1):
        w=W[k]
        if (w&0x0fef0000)==0x03a00000 and ((w>>12)&15)==reg:
            rot=(w>>8)&15; imm=w&0xff
            return ((imm>>(2*rot))|(imm<<(32-2*rot)))&0xffffffff
        if ((w>>12)&15)==reg and (w&0x0c000000)==0: return None
    return None
if __name__=='__main__':
    t=int(sys.argv[1],16)
    for i in callers(t):
        print(hex(base+4*i),'r0=',regval(i,0),'r1=',regval(i,1))

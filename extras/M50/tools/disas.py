import sys,capstone
rom=open(sys.argv[1],'rb').read()
a=int(sys.argv[2],16); n=int(sys.argv[3],16)
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_THUMB)
for i in md.disasm(rom[a-0xE0000000:a-0xE0000000+n],a):
    print("%08X %-8s %s"%(i.address,i.mnemonic,i.op_str))

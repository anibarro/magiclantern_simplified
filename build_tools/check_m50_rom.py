#!/usr/bin/env python3
"""Check an M50 XPan build against a ROM dump of the camera it is meant for.

  check_m50_rom.py ROM0.BIN platform/M50.110/stubs.S

* The firmware signature ML checks at boot (sum of 0x10000 words from
  E0040000) must equal SIG_M50_110 in src/fw-signature.h.
* Every THUMB_FN stub that points into ROM0 must start like a function
  (PUSH, PUSH.W or STR.W to the stack). Leaf functions that do not are
  listed in KNOWN_NO_PUSH after checking them by hand; a new one fails the
  check until someone looks at it.
Exit code 0 = all good, 1 = mismatch.
"""
import re
import struct
import sys

ROM0_BASE = 0xE0000000
SIG_M50_110 = 0x05EDBC80

# Checked by hand on 2026-11-08 (README-XPAN-M50.md): leaf routines or
# functions that load a register before their PUSH.
KNOWN_NO_PUSH = {
    "bzero32", "dcache_clean", "dcache_clean_multicore", "icache_invalidate",
    "dcache_invalidate", "dcache_invalidate_multicore", "dcache_clean_invalidate",
    "send_software_interrupt", "change_mmu_tables", "_FreeMemory",
    "GetMemoryInformation", "GetSizeOfMaxRegion", "gui_main_task",
    "GetFirstChunkFromSuite", "GetMemoryAddressOfMemoryChunk",
}


def main():
    rom_path, stubs_path = sys.argv[1], sys.argv[2]
    rom = open(rom_path, "rb").read()
    ok = True

    sig = sum(struct.unpack_from("<65536I", rom, 0xE0040000 - ROM0_BASE)) & 0xFFFFFFFF
    if sig == SIG_M50_110:
        print("ROM check: firmware signature %08X = SIG_M50_110 (1.1.0)" % sig)
    else:
        print("ROM check: ERROR firmware signature %08X, expected %08X" % (sig, SIG_M50_110))
        ok = False

    checked = 0
    for line in open(stubs_path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*THUMB_FN\(\s*0x([0-9a-fA-F]+)\s*,\s*(\w+)\s*\)", line)
        if not m:
            continue
        addr, name = int(m.group(1), 16) & ~1, m.group(2)
        if not ROM0_BASE <= addr < ROM0_BASE + len(rom):
            continue
        checked += 1
        h0 = struct.unpack_from("<H", rom, addr - ROM0_BASE)[0]
        if (h0 & 0xFE00) == 0xB400 or h0 in (0xE92D, 0xF84D):
            continue
        if name in KNOWN_NO_PUSH:
            continue
        print("ROM check: ERROR stub %s at %08X does not start like a function (%04X)" % (name, addr, h0))
        ok = False
    print("ROM check: %d THUMB stubs in ROM0 %s" % (checked, "look right" if ok else "checked, see errors"))

    # XPan UI event filter (src/xpan-m50.c): the dial table, the dispatcher
    # that jumps through RAM 0x5A24+0x30, and Canon's callback it wraps.
    dial_ok = True
    w = lambda a: struct.unpack_from("<I", rom, a - ROM0_BASE)[0]
    h = lambda a: struct.unpack_from("<H", rom, a - ROM0_BASE)[0]
    if [w(0xE0AA7598 + 4 * i) for i in range(4)] != [3, 0, 0x80000407, 0x407]:
        print("ROM check: ERROR RotaryEncoder table at E0AA7598 is not the dial (0x80000407 / 0x407)")
        dial_ok = False
    if w(0xE01E4720) != 0x5A24 or h(0xE01E466A) != 0x6B12 or h(0xE01E466C) != 0x4710:
        print("ROM check: ERROR UI event dispatcher (E01E4668) no longer jumps through [0x5A24+0x30]")
        dial_ok = False
    if h(0xE03D8D9A) & 0xFE00 != 0xB400:
        print("ROM check: ERROR Canon's UI event callback E03D8D9A does not start like a function")
        dial_ok = False
    if dial_ok:
        print("ROM check: dial table, UI event dispatcher and callback as expected")
    ok = ok and dial_ok

    # Second core at startup (reboot.c, boot-d678.c; README-XPAN-M50.md,
    # "Startup hang: the second core"): the bootloader code the fix relies on.
    core1 = {
        # wait loop: until [0xBFE01FC4] == 0x10, clean cache, blx [0xBFE01FC8]+1
        0xE0004910: "41f6c476cbf6e07631681039f8d100f0adf941f6c876cbf6e076316801f10101884701f0",
        # autoexec start: [0xBFE01FC8] = entry, [0xBFE01FC4] = 0x10, jump
        0xE0019746: "41f6c876cbf6e076306041f6c476cbf6e07640f21001c0f20001316000f101000047",
        # normal start: [0xBFE01FC8] = 0xE0040000, [0xBFE01FC4] = 0x10
        0xE00049AE: "41f6c876cbf6e07640f20001cef20401316041f6c476cbf6e07640f21001c0f200013160",
        # firmware entry: core number from MPIDR, other cores branch away
        0xE0040014: "10eeb05f15f00f000ad140f20440cdf211000168012904bf03210160",
    }
    core1_ok = True
    for addr, hexbytes in core1.items():
        want = bytes.fromhex(hexbytes)
        got = rom[addr - ROM0_BASE: addr - ROM0_BASE + len(want)]
        if got != want:
            print("ROM check: ERROR second-core boot code at %08X differs" % addr)
            core1_ok = False
    if core1_ok:
        print("ROM check: bootloader second-core mailbox and wait loop as expected")
    ok = ok and core1_ok

    # Core 1 start (reboot.c parks it, init.c m50_release_core1 releases it;
    # README-XPAN-M50.md, "Startup hang: core 1 and its start entry"):
    # DryOS writes the marker 0x20101231 into the start table 0x56520 in
    # E0558648, called first from E01A436C, which create_init_task calls;
    # core 1 checks its entry in E00403B8.
    c1 = {
        0xE0558A34: "31121020",            # marker literal 0x20101231
        0xE0558A38: "20650500",            # table literal 0x56520 (E0558674)
        0xE0558E4C: "20650500",            # table literal 0x56520 (E0558D8C check)
        0xE0558672: "f04af049e0600838a060002001eb0013401c02281a60f9db",  # writes marker for cores 0, 1
        0xE01A436E: "b4f36bf9",            # E01A436C: bl E0558648 first
        0xE0143D2E: "60f01dfb",            # create_init_task: bl E01A436C
        0xE00403B8: "08b5684618f196f54ff48050806818f1e1f428b1009818f1d1f54ff0ff3008bd18f1e1f4",
    }
    c1_ok = True
    for addr, hexbytes in c1.items():
        want = bytes.fromhex(hexbytes)
        got = rom[addr - ROM0_BASE: addr - ROM0_BASE + len(want)]
        if got != want:
            print("ROM check: ERROR core-1 start code at %08X differs" % addr)
            c1_ok = False
    if c1_ok:
        print("ROM check: core-1 start marker and create_init_task order as expected")
    ok = ok and c1_ok

    # XPan colour-table hook (mmu_patches.h, xpan-m50.c xpan_m50_lucky_load;
    # README-XPAN-M50.md "17-point grid"): Canon's E00E73BC and the two calls
    # the replacement makes, plus the loader's size -> grid choice.
    lk = {
        0xE00E73BC: "2de9fc411f4615460e46044637f391f24ff48051",  # push; movs; bl E081E8EE; mov.w r1,#0x1000
        0xE081E8EE: "1a4808387047",                              # ldr r0,=E1202784; subs #8; bx lr
        0xE011E194: "2de9f8438146",                              # push.w {r3-r9,lr}; mov r9,r0
        0xE011E1AC: "a4f59840c43831d0",                          # size 0x4CC4 -> grid 17 (beq)
        0xE0082CC0: "64f07cfb",                                  # ShootDevelop: bl E00E73BC
    }
    lk_ok = True
    for addr, hexbytes in lk.items():
        want = bytes.fromhex(hexbytes)
        got = rom[addr - ROM0_BASE: addr - ROM0_BASE + len(want)]
        if got != want:
            print("ROM check: ERROR colour-table code at %08X differs (%s)" % (addr, got.hex()))
            lk_ok = False
    if lk_ok:
        print("ROM check: colour-table loader E00E73BC and its calls as expected")
    ok = ok and lk_ok

    # Creative-filter shot decision (README-XPAN-M50.md, "Hardware grain"):
    # XPan calls E01E340A from the JPEG hook (read-only); it reads
    # [[0x5408]+0x40] +0xE8C/+0xAC/+0 and returns 0/1.
    cf = {
        0xE01E340A: "6348006cd0f88c1e49b1d0f8ac10032903d000681e38052801d80120704700207047",
        0xE01E3598: "08540000",          # literal 0x5408
    }
    cf_ok = True
    for addr, hexbytes in cf.items():
        want = bytes.fromhex(hexbytes)
        got = rom[addr - ROM0_BASE: addr - ROM0_BASE + len(want)]
        if got != want:
            print("ROM check: ERROR creative-filter decision code at %08X differs (%s)" % (addr, got.hex()))
            cf_ok = False
    if cf_ok:
        print("ROM check: creative-filter decision E01E340A as expected")
    ok = ok and cf_ok

    # Grain stage probe (mmu_patches.h, xpan-m50.c xpan_m50_cf_grain_run;
    # README-XPAN-M50.md "Grain stage probe"): the 8 bytes the trampoline
    # replays and StartRoughMonoPath's call to the driver run.
    gs = {
        0xE086DA10: "30b5044697b01821",  # push {r4,r5,lr}; mov r4,r0; sub sp,#0x5c; movs r1,#0x18
        0xE086216E: "0bf04ffc",          # StartRoughMonoPath: bl E086DA10 (job)
        0xE0545D8A: "42f21072",          # stage call waits 10000 ms (movw r2,#0x2710)
        0xE086DCBC: "01db86e0",          # literal E086DB01 (posted by E086DB6A)
        0xE086DCB8: "bc830c00",          # literal 0xC83BC (E086DAF6 copies 32 bytes there)
        # Live View filter map (README "Live View without Canon's filter")
        0xE0776CC6: "10b5082804460ad3",  # push {r4,lr}; cmp r0,#8; mov r4,r0; bcc E0776CE4
        0xE0776CE4: "36489830",          # trampoline target kind < 8: ldr r0,[pc,#216]; adds r0,#152
        0xE0776CCE: "03460621",          # trampoline target kind >= 8: mov r3,r0; movs r1,#6
        0xE048CB4C: "eaf2bbf8",          # E048CB24 (Live View filter apply): bl E0776CC6, its only caller
        # 65:24 JPEG (README "65:24 JPEG via Canon's 3:2 size")
        0xE0FCFF88: "701700000000000000000000",  # encoder table 3:2 L: width 6000, left 0, top 0
        0xE0FCFFB0: "f3010000",          # w10 = 4000/8-1
        0xE0FD0488: "88fffce0",          # table pointer aspect 0 size L -> E0FCFF88
        0xE0082208: "d4b40000",          # size getter E0081E4E: table pointer at 0xB4D4
        0xE0081EE0: "9068806c",          # aspect 0: ldr r0,[r2,#8]; ldr r0,[r0,#0x48] (width)
        # Display image layer setters (README "Live View crop: the display's image layer")
        0xE01389AC: "2de9f04304461430",  # panel: push.w {r4-r8,sb,lr}; mov r4,r0; adds r0,#0x14
        0xE01390E0: "2de9f04304461430",  # EVF: same
        0xE0138AAA: "1cf05bf8",          # panel -> image layer setter E0154B64
        0xE0104234: "33a2",              # "set vram Panel addr" log, after the panel call
        # Touch -> AF frame converter (README "Touch AF and Canon's AF box")
        0xE0423406: "2de9ff4f002685b0",  # push.w {r0-r11,lr}; movs r6,#0; sub sp,#0x14
        0xE0426A68: "44710000",          # picture area table pointer 0x7144 (+0x28: area in sensor units)
        0xE042667C: "fa48806a01894089",  # centre row getter: ldr r0,=0x7144; ldr r0,[r0,#0x28]; ldrh r1,[r0,#8]; ldrh r0,[r0,#0xa]
        0xE0426684: "01eb500080b27047",  # add.w r0,r1,r0,lsr #1; uxth r0,r0; bx lr (where the trampoline continues)
        0xE04266A4: "70b50546fff7e8ff04462846faf7ecfaa4eb500080b270bd",  # top getter: bl E042667C; bl E0420C8C; sub.w r0,r4,r0,lsr #1; uxth (unsigned: Bottom cannot shift the area)
        0xE042268A: "2de9fc5f04469846",  # AF box on screen: push.w {r2-r11,ip,lr}; mov r4,r0; mov r8,r3
    }
    gs_ok = True
    for addr, hexbytes in gs.items():
        want = bytes.fromhex(hexbytes)
        got = rom[addr - ROM0_BASE: addr - ROM0_BASE + len(want)]
        if got != want:
            print("ROM check: ERROR grain stage code at %08X differs (%s)" % (addr, got.hex()))
            gs_ok = False
    if gs_ok:
        print("ROM check: grain driver run E086DA10, its caller and the 10 s stage wait as expected")
    ok = ok and gs_ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

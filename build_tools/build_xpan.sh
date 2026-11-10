#!/bin/bash
# Build the XPan firmware. Run from anywhere (Linux or WSL); the toolchain
# must be on PATH or given with ARM_BINPATH.
#   build_tools/build_xpan.sh              -> EOS M 2.0.2: platform/EOSM.202/build_xpan/magiclantern.zip
#   CAM=6D.116 build_tools/build_xpan.sh   -> Canon 6D 1.1.6: platform/6D.116/build_xpan/magiclantern.zip
#   CAM=M50.110 build_tools/build_xpan.sh  -> EOS M50 1.1.0: platform/M50.110/build_xpan/magiclantern.zip (phase 1: framing and HUD)
#   B=my_dir build_tools/build_xpan.sh     -> custom build directory
#   CLEAN=0 build_tools/build_xpan.sh      -> incremental build (default: clean)
#
# After the build, every check below must pass or the script fails and the
# package must not go on a card. A verified package is archived in
# platform/$CAM/xpan_builds/ under its date and hash, so a build that worked
# on the camera can always be put back (build_tools/xpan_update_card.ps1).
set -euo pipefail
cd "$(dirname "$0")/.."
CAM=${CAM:-EOSM.202}
case "$CAM" in
  EOSM.202|6D.116|M50.110) ;;
  *) echo "XPan supports CAM=EOSM.202, CAM=6D.116 or CAM=M50.110, not $CAM" >&2; exit 2 ;;
esac
B=${B:-build_xpan}
D=platform/$CAM/$B
LOG=platform/$CAM/$B.log
NM=${ARM_BINPATH:+$ARM_BINPATH/}arm-none-eabi-nm
# Clean by default: WSL on an NTFS drive shows clock skew, and make then
# keeps objects that are older than their sources.
if [ "${CLEAN:-1}" != 0 ]; then rm -rf "$D"; fi
make -C platform/$CAM -j"$(nproc)" BUILD_DIR=$B ZIP_DIR=$B/zip CONFIG_QEMU=n XPAN=y \
  ${ARM_BINPATH:+ARM_BINPATH=$ARM_BINPATH} 2>&1 | tee "$LOG"

fail() { echo "CHECK FAILED: $*"; exit 1; }
{
  echo "--- checks"
  # 1. WSL/NTFS sometimes writes 0-byte objects; a binary linked from them is
  #    broken even when make succeeds.
  EMPTY=$(find "$D" -name '*.o' -size 0)
  [ -z "$EMPTY" ] || fail "empty object files: $EMPTY"
  echo "ok: no empty objects"
  # 2. autoexec.bin must carry this magiclantern.bin unchanged.
  python3 -I build_tools/verify_embedded_firmware.py "$D/autoexec.bin" "$D/magiclantern.bin" || fail "embedded image"
  # 3. The zip must hold this autoexec.bin and no modules.
  unzip -p "$D/magiclantern.zip" autoexec.bin | cmp -s - "$D/autoexec.bin" || fail "zip autoexec.bin differs from the build"
  ZIPLIST=$(unzip -Z1 "$D/magiclantern.zip")
  if grep -qiE '\.mo$' <<< "$ZIPLIST"; then fail "zip contains modules"; fi
  echo "ok: zip holds this autoexec.bin and no modules"

  if [ "$CAM" = M50.110 ]; then
    # 4. Memory: boot-d678.c reserves _bss_end - RESTARTSTART from Canon's
    #    user memory and stops at "fail:" if less than MINIMUM_USER_MEM_LEFT
    #    (0x80010) is left. Canon's user memory before ML: 0x114920 bytes from
    #    0xDC870 (M50 RAM dump, 2026-11-08). Require 32 KB of headroom.
    # Read the symbol table once: "nm | grep -q" fails under pipefail when
    # grep finds a match and exits early (nm gets SIGPIPE).
    SYMS=$($NM "$D/magiclantern")
    BSS_END=$(awk '$3=="_bss_end"{print $1}' <<< "$SYMS")
    RESERVED=$(( 0x$BSS_END - 0xDC870 ))
    LEFT=$(( 0x114920 - RESERVED ))
    printf 'M50 memory: ML reserves 0x%X, Canon keeps 0x%X (minimum 0x80010, margin %d bytes)\n' $RESERVED $LEFT $(( LEFT - 0x80010 ))
    [ $(( LEFT - 0x80010 )) -ge 32768 ] || fail "less than 32 KB above the M50 boot minimum"
    # 5. Features that hung the first M50 boot must stay out.
    if grep -qE ' (backup_rom_task|my_assert_handler|save_crash_log)$' <<< "$SYMS"; then
      fail "CONFIG_AUTOBACKUP_ROM or CONFIG_CRASH_LOG is back in the M50 build"
    fi
    echo "ok: no ROM backup / crash log code"
    # 6. The battery-door (check-mode start) timeout must be compiled in.
    grep -q " m50_gui_wait_timeout_ms$" <<< "$SYMS" || fail "init.c GUI-wait timeout (battery-door fix) missing"
    echo "ok: GUI-wait timeout present"
    # 6a. MMU remap is on again (2026-11-09; it was off while hunting the startup
    #     hang, which was the card). It carries XPan's colour-table hook, which
    #     must replace Canon's E00E73BC with the original bytes as its check
    #     (README "17-point grid"). No core-1 start hijack: the M50 parks and
    #     releases core 1 itself (init.c m50_release_core1).
    grep -qE ' mmu_init$' <<< "$SYMS" && grep -qE ' apply_normal_patches$' <<< "$SYMS" || fail "MMU remap missing from the M50 build"
    grep -qE ' xpan_m50_lucky_load$' <<< "$SYMS" || fail "XPan colour-table hook missing"
    grep -q "\.patch_addr = 0xe00e73bc" platform/M50.110/include/platform/mmu_patches.h || fail "colour-table hook address changed"
    grep -q "0x2d, 0xe9, 0xfc, 0x41, 0x1f, 0x46, 0x15, 0x46" platform/M50.110/include/platform/mmu_patches.h || fail "colour-table hook lost its original-bytes check"
    if grep -q "CONFIG_INIT1_HIJACK" platform/M50.110/*.h; then fail "core-1 start hijack enabled on the M50 (conflicts with m50_release_core1)"; fi
    echo "ok: MMU remap with the checked colour-table hook, no core-1 hijack"
    # 6a2. Custom LUT step 1 (README "Custom LUT, step 1"): the hook passes a
    #      10-point table (size 0x1000) until the 17-point layout is known, XPan's
    #      table only when the EOS M format check accepted Canon's, written back
    #      to RAM and passed by its uncached address.
    grep -q "canon_load(\*(void \*\*)canon_ctx(),a1,a2,use,0x1000,a3)" src/xpan-m50.c || fail "colour-table hook no longer passes size 0x1000"
    grep -q "if (xpan_native_table_prepare_cube(m50_look_table," src/xpan-m50.c || fail "Custom LUT table not built by the checked EOS M conversion"
    grep -q "dcache_clean((uint32_t)m50_look_table,sizeof(m50_look_table));" src/xpan-m50.c || fail "Custom LUT table not cleaned to RAM before the develop reads it"
    grep -q "use=UNCACHEABLE(m50_look_table);" src/xpan-m50.c || fail "Custom LUT table not passed by its uncached address"
    echo "ok: Custom LUT passes a checked, cleaned 10-point table"
    # 6a3. XPan's working hooks plus the read-only display probe (camera test 2026-11-10: photos slow,
    #      clean-up of the JPEG setup, Live View grain and resize probes).
    HOOKS=$(grep -o "\.patch_addr = 0x[0-9a-f]*" platform/M50.110/include/platform/mmu_patches.h | sed 's/.*0x//' | sort | tr '\n' ' ')
    [ "$HOOKS" = "e00e73bc e01389ac e01390e0 e042268a e0423406 e042667c e0776cc6 e086da10 " ] || fail "M50 MMU hooks changed: $HOOKS (expected colour table, display panel/EVF, AF converter probe, Live View filter map, grain stage)"
    # AF converter probe (README "Touch AF and Canon's AF box"): read only,
    # Canon's converter always runs with the same arguments.
    AFC=$(sed -n '/^int xpan_m50_af_conv(void \*frame/,/^}/p' src/xpan-m50.c)
    # Canon's converter always runs; only the tap row (s2) may change, only
    # for a tap XPan passed to Canon in the last 500 ms (Bottom layout).
    grep -q 'int ret=xpan_m50_af_conv_tramp(frame,r1,r2,r3,s0,s1,ty,s3,s4,s5,s6);' <<< "$AFC" && grep -q '1:  .word 0xE042340F' src/xpan-m50.c || fail "AF converter does not always run Canon's converter"
    grep -q 'if (td>0 && !af_tap_blocked && get_ms_clock()-af_tap_ms<500) ty=s2>td ? s2-td : 0;' <<< "$AFC" || fail "Bottom tap correction lost its limits"
    # The area top is unsigned in Canon's code: never shifted below 0.
    grep -q 'if (c-ds-H/2<0) { area_delta_s=0; touch_delta_s=ds; return c; }' src/xpan-m50.c || fail "AF picture area may go below 0 (Canon wraps it, Bottom layout)"
    if grep -nE '\(\(uint16_t \*\)frame\)\[[^]]*\] *=[^=]|msleep|take_semaphore|FIO_|snprintf|malloc' <<< "$AFC"; then fail "AF converter probe writes Canon's frame, blocks or allocates"; fi
    # AF picture area (README "Touch AF and Canon's AF box"): Canon's getter
    # always runs; the result moves only by the band's own move, in sensor
    # units, only while the display moves the band; nothing is written.
    ACY=$(sed -n '/^int xpan_m50_area_cy(void)/,/^}/p' src/xpan-m50.c)
    grep -q 'int c=xpan_m50_area_cy_tramp();' <<< "$ACY" && grep -q 'if (!disp_band(&s,&d,&h)) { area_delta_s=0; touch_delta_s=0; return c; }' <<< "$ACY" && grep -q 'int ds=(d-s)\*H/480;' <<< "$ACY" || fail "AF picture area hook changed"
    grep -q '1:  .word 0x00007144' src/xpan-m50.c && grep -q '2:  .word 0xE0426685' src/xpan-m50.c || fail "AF picture area trampoline changed"
    if grep -nE '\ba\[[^]]*\] *=[^=]|msleep|take_semaphore|FIO_|snprintf|malloc' <<< "$ACY"; then fail "AF picture area hook writes, blocks or allocates"; fi
    # AF box (Bottom): Canon's function always runs; only the rectangle it
    # returns moves, by the Bottom correction, nothing else is written.
    AFB=$(sed -n '/^int xpan_m50_af_box(void \*out/,/^}/p' src/xpan-m50.c)
    grep -q 'int ret=xpan_m50_af_box_tramp(out,r1,r2,r3,s0,s1,s2);' <<< "$AFB" && grep -q 'if (td>0 && lr!=0xE04227CB && cfg_ram((uint32_t)out)) {' <<< "$AFB" && grep -q '1:  .word 0xE0422693' src/xpan-m50.c || fail "AF box hook changed (Canon always runs; only out+6, only in Bottom, not for the AF area calculation)"
    # Taps passed to Canon in XPan's view: only on the moved picture, never on
    # an XPan field or where Canon's hidden touch buttons are.
    TFC=$(sed -n '/^static int m50_tap_for_canon/,/^}/p' src/xpan-m50.c)
    grep -q 'if (!disp_band(&s,&d,&h)' <<< "$TFC" && grep -q 'if (xpan_photo_hud_hit_test(x,y)>=0) return 0;' <<< "$TFC" && grep -q 'if (y>=420) return 0;' <<< "$TFC" || fail "touch AF pass-through lost its limits"
    # Display probe (README "Live View crop: the display's image layer"): read
    # only, Canon's setter always runs, trampolines replay its first 8 bytes.
    for H in panel evf; do
      DP=$(sed -n "/^int xpan_m50_disp_$H(void \*p/,/^}/p" src/xpan-m50.c)
      grep -q "return xpan_m50_disp_${H}_tramp(p,a1,a2,a3,s0,s1);" <<< "$DP" || fail "display probe ($H) does not always run Canon's setter"
      # The 65:24 placement changes only XPan's copy (q), never Canon's block.
      if grep -nE '(^|[^>.a-z_])p\[[^]]*\] *=[^=]' <<< "$DP"; then fail "display hook ($H) writes Canon's parameter block"; fi
    done
    # 65:24 live picture (README "Live View crop: the display's image layer"):
    # only the exact LCD block, source/destination row and height (words 6, 8,
    # 10) from XPan's geometry, checked to stay inside 480 rows.
    DPP=$(sed -n '/^int xpan_m50_disp_panel(void \*p/,/^}/p' src/xpan-m50.c)
    grep -q 'if (disp_lcd_block((const uint32_t \*)p) && disp_band(&src_y,&dst_y,&h)) {' <<< "$DPP" && grep -q 'q\[6\]=src_y; q\[8\]=dst_y; q\[10\]=h;' <<< "$DPP" || fail "65:24 live picture placement changed"
    grep -q 'if (x || w!=720 || y<0 || hh<2 || y+hh>480 || d<0 || d+hh>480) return 0;' src/xpan-m50.c || fail "65:24 live picture rows not bounded to the 480-row panel"
    grep -q 'return cfg_ram((uint32_t)p) && p\[0\]==1 && p\[3\]==736 && p\[4\]==480 && !p\[5\] && !p\[6\] && !p\[7\] && !p\[8\]' src/xpan-m50.c || fail "65:24 live picture no longer limited to Canon's LCD block"
    grep -q '1:  .word 0xE01389B5' src/xpan-m50.c && grep -q '2:  .word 0xE01390E9' src/xpan-m50.c && [ "$(grep -c 'adds r0, #0x14\\n"' src/xpan-m50.c)" = 2 ] || fail "display probe trampolines changed"
    DREC=$(sed -n '/^static void disp_record/,/^}/p' src/xpan-m50.c)
    if grep -nE '(^|[^>.a-z_])p\[[^]]*\] *=[^=]|msleep|take_semaphore|FIO_|snprintf|malloc' <<< "$DREC"; then fail "display probe writes Canon's block, blocks or allocates"; fi
    # No probe or study code left in the shot path (README "XPan log").
    if grep -nE 'XM50LUT|lutp_scan_save|lutp_auto|cache_facts|grain_bench|jpeg_calls|cfg_calls|lucky_calls|rsz_recs|lvg_count|M50_GRAIN_AVAILABLE|grain_worker\(' src/xpan-m50.c; then fail "probe/study code is back in xpan-m50.c"; fi
    PAGES=$(grep -o "\.patch_addr = 0x[0-9a-f]*" platform/M50.110/include/platform/mmu_patches.h | sed 's/.*0x//' | cut -c1-4 | sort -u | wc -l)
    # Each hooked 64 KB ROM page needs a reserved page (CONFIG_MMU_REMAP_PAGES,
    # patch_mmu_space.c; README "Memory for a second hooked page").
    MMU_PAGES=$(grep -oE "^#define CONFIG_MMU_REMAP_PAGES [0-9]+" platform/M50.110/internals.h | awk '{print $3}')
    [ -n "$MMU_PAGES" ] && [ "$PAGES" -le "$MMU_PAGES" ] || fail "MMU hooks span $PAGES ROM pages, only ${MMU_PAGES:-1} reserved (CONFIG_MMU_REMAP_PAGES)"
    grep -q " generic_mmu_space$" <<< "$SYMS" || fail "generic_mmu_space missing"
    if grep -q "^static uint8_t generic_mmu_space" src/patch_mmu.c; then fail "generic_mmu_space back in patch_mmu.c (its 64 KB alignment pads patch_mmu.o's .bss twice)"; fi
    grep -q "MMU_L2_TABLE_SIZE \* num_64k_pages;" src/patch_mmu.c || fail "L2 table space check uses sizeof() again"
    grep -q '\.bss : { \*patch_mmu_space\.o(\.bss) \*(\.bss) }' src/magiclantern.lds.S || fail "linker script no longer puts generic_mmu_space first in .bss (64 KB padding gap)"
    BSS_START=$(awk '$3=="_bss_start"{print $1}' <<< "$SYMS")
    MMU_SPACE=$(awk '$3=="generic_mmu_space"{print $1}' <<< "$SYMS")
    # 4 KB pages (README "4 KB pages"): the space needs only the L1 table's 16 KB
    # alignment, so the padding before it stays under 16 KB whatever the code size.
    grep -q "^#define CONFIG_MMU_REMAP_SMALL_PAGES" platform/M50.110/internals.h || fail "M50 no longer maps remapped pages with 4 KB pages"
    [ $(( 0x$MMU_SPACE )) -eq $(( (0x$BSS_START + 0x3FFF) & ~0x3FFF )) ] || fail "generic_mmu_space (0x$MMU_SPACE) not at the first 16 KB boundary of .bss (0x$BSS_START)"
    grep -q 'uint32_t small = (flags & 0x0e3c) | (((flags >> 12) & 7) << 6) | ((flags >> 15) & 1) | 2;' src/mmu_utils.c || fail "small-page flag conversion changed (large: XN 15, TEX 14:12; small: XN 0, TEX 8:6)"
    grep -q 'replace_large_page_with_small_pages_in_l2_table(romaddr, ramaddr, l2addr, flags);' src/mmu_utils.c || fail "remapped pages no longer use small pages"
    grep -q 'int res = calc_mmu_globals_small((uint32_t)generic_mmu_space,' src/patch_mmu.c || fail "small-page layout not used"
    # Heap-backed page copies need malloc: only normal patches, never early ones.
    for L in early_data_patches early_code_patches; do
        [ -z "$(sed -n "/^struct [a-z_]* $L\[\] *=/,/^};/p" platform/M50.110/include/platform/mmu_patches.h | grep -E 'patch_addr|\.addr')" ] || fail "M50 has $L entries: heap-backed MMU pages need malloc (normal patches only)"
    done
    # malloc only in reserve_heap_pages(), called before cli() in patch_memory_rom();
    # nothing allocates inside the critical section (interrupts off, cpu1 parked).
    [ "$(grep -v '^ *//' src/patch_mmu.c | grep -c 'malloc(')" = 1 ] && grep -q 'void \*mem = malloc(MMU_PAGE_SIZE + 0x1000);' <<< "$(sed -n '/^static void reserve_heap_pages/,/^}/p' src/patch_mmu.c)" || fail "MMU heap pages allocated outside reserve_heap_pages()"
    # apply_patches() disables interrupts before apply_patch(): the pages are reserved
    # in apply_normal_patches() (task context) before any apply_patches().
    if sed -n '/^static int patch_memory_rom/,/^}/p' src/patch_mmu.c | grep -q 'reserve_heap_pages'; then fail "heap pages reserved inside the patch critical section"; fi
    [ "$(tr -d '\r\n ' <<< "$(sed -n '/^int apply_normal_patches/,/^}/p' src/patch_mmu.c)" | grep -o '^intapply_normal_patches(void){#ifdefined(CONFIG_MMU_REMAP_SMALL_PAGES)&&!defined(CONFIG_ALLOCATE_MEMORY_POOL)reserve_heap_pages();' | wc -l)" = 1 ] || fail "heap pages not reserved first in apply_normal_patches()"
    [ "$(grep -c 'reserve_heap_pages();' src/patch_mmu.c)" = 1 ] || fail "reserve_heap_pages() called from more than one place"
    # Live View filter map (README "Live View without Canon's filter"): Canon's
    # function always runs; the trampoline reproduces its first 8 bytes, the
    # bcc included; XPan's case returns Canon's own kind-0 value.
    grep -q "\.patch_addr = 0xe0776cc6" platform/M50.110/include/platform/mmu_patches.h || fail "Live View filter map hook address changed"
    grep -q "0x10, 0xb5, 0x08, 0x28, 0x04, 0x46, 0x0a, 0xd3" platform/M50.110/include/platform/mmu_patches.h || fail "Live View filter map hook lost its original-bytes check"
    grep -q '1:  .word 0xE0776CE5' src/xpan-m50.c && grep -q '3:  .word 0xE0776CCF' src/xpan-m50.c && grep -q 'bcs 2f' src/xpan-m50.c || fail "Live View filter map trampoline changed"
    LVM=$(sed -n '/^int xpan_m50_lv_cfilter_map/,/^}/p' src/xpan-m50.c)
    grep -q 'return xpan_m50_lv_cfilter_tramp(0);' <<< "$LVM" && grep -q 'return xpan_m50_lv_cfilter_tramp(kind);' <<< "$LVM" || fail "Live View filter map does not return Canon's own values"
    grep -q 'if (kind==1 && film_stage_wanted() && m50_pasm())' <<< "$LVM" || fail "Live View filter map condition changed"
    if grep -nE 'msleep|take_semaphore|FIO_|snprintf|malloc|_prop_request_change' <<< "$LVM"; then fail "Live View filter map blocks, writes settings or allocates"; fi
    echo "ok: 4 KB remap pages (space at the first 16 KB boundary), Live View filter map returns Canon's own values"
    # 65:24 JPEG (README "65:24 JPEG via Canon's 3:2 size"): only m50_crop_poll
    # patches, through apply_patches (old values checked), after checking both
    # tables, never while half-pressed/busy/recording; undone on any error.
    CRP=$(sed -n '/^static void m50_crop_poll/,/^}/p' src/xpan-m50.c)
    [ "$(grep -c 'apply_patches(' src/xpan-m50.c)" = 1 ] && grep -q 'crop_err=apply_patches(crop_patch,3);' <<< "$CRP" || fail "Canon tables patched outside m50_crop_poll"
    grep -q 'if (get_halfshutter_pressed() || lens_info.job_state || RECORDING) return;' <<< "$CRP" || fail "65:24 crop lost its busy guard"
    grep -q 'if (crop_err) { crop_undo(); crop_refused=1; return; }' <<< "$CRP" || fail "65:24 crop not undone on a partial failure"
    grep -q "enc\[0\]!=6000 || enc\[1\]!=0 || enc\[2\]!=0 || enc\[10\]!=(CROP_FULL_H/8u-1u)" <<< "$CRP" || fail "65:24 crop lost its encoder table check"
    grep -q 'if (t\[0x48/4\]!=6000 || t\[0x4C/4\]!=CROP_FULL_H || t\[0x58/4\]!=6000 || t\[0x5C/4\]!=3368) return 0;' src/xpan-m50.c || fail "65:24 crop lost its size table check"
    grep -q '^#define CROP_H 2216u' src/xpan-m50.c && grep -q '^#define CROP_TOP ((CROP_FULL_H-CROP_H)/2u)' src/xpan-m50.c || fail "65:24 crop geometry changed"
    echo "ok: 65:24 JPEG crop through ML's patch API, checked tables, undone on failure"
    grep -q 'if (kind==1 && film_stage_wanted() && m50_pasm())' src/xpan-m50.c && grep -q 'if (!m50_pasm()) return;' src/xpan-m50.c || fail "Film grain switch / Live View map not in P/Tv/Av/M"
    # Hooked 1 MB sections: untouched ROM pages keep their own attributes (TEX 5,
    # outer cacheable); only the remapped page gets RAM attributes (README "ROM
    # attributes in hooked sections": Live View stutter otherwise).
    grep -q 'flags_section);' src/patch_mmu.c && grep -q 'uint32_t flags_section = get_l2_largepage_flags_from_l1_section((uint32_t)patch->addr, CANON_ORIG_MMU_TABLE_ADDR);' src/patch_mmu.c || fail "hooked sections no longer keep Canon's ROM attributes"
    echo "ok: six hooks (display placement, AF converter probe read only), no old probe code; $PAGES hooked ROM pages of $MMU_PAGES reserved (one more for the 65:24 encoder table)"
    # XPan off = Canon (camera test 2026-11-10: photos slow with XPan off): grain,
    # photo LUT and Live LUT only while XPan is on, in photo mode, as on the EOS M.
    grep -q 'static int film_stage_wanted(void) { return M50_STAGE_GRAIN && film_grain && grain_strength>0 && xpan_photo_enabled() && !is_movie_mode(); }' src/xpan-m50.c || fail "film grain runs with XPan off"
    grep -A1 'go=m50_look_on && m50_cube_valid' src/xpan-m50.c | grep -q 'xpan_photo_enabled() && !is_movie_mode()' || fail "photo LUT applied with XPan off"
    grep -A1 'int want=m50_live_on && m50_look_on' src/xpan-m50.c | grep -q 'xpan_photo_enabled() && !is_movie_mode()' || fail "Live LUT preview active with XPan off"
    echo "ok: grain, photo LUT and Live LUT only while XPan is on"
    # EVF crashes (README "EVF crashes"): ML writes Canon's RGBA layer only when it
    # is Canon's current one and holds at least 960x540 pixels, under winsys_sem,
    # taken once per refresh (DryOS semaphores are not recursive).
    RFR=$(sed -n '/^void refresh_yuv_from_rgb(void)/,/^}/p' src/bmp.c)
    grep -q 'struct MARV \*m = _rgb_vram_info;' src/bmp.c && grep -q 'if (!m || !data || w < BMPPITCH || w \* h < BMP_VRAM_SIZE) return 0;' src/bmp.c || fail "bitmap layer guard missing (Canon's current layer, at least 960x540)"
    grep -q 'if (!rgb_layer_current())' <<< "$RFR" || fail "bitmap refresh writes Canon's layer without checking it"
    # Taken with a timeout on the M50 (a skipped refresh is retried, never a
    # wait forever), once per refresh: XimrExe runs inside the same hold.
    grep -q 'if (!winsys_sem || take_semaphore(winsys_sem, RGB_LAYER_LOCK_MS))' <<< "$RFR" && grep -q '#elif defined(CONFIG_M50)' <<< "$RFR" || fail "M50 refresh does not hold winsys_sem (with a timeout) around the copy"
    [ "$(grep -c 'take_semaphore(winsys_sem' <<< "$RFR")" = 2 ] || fail "winsys_sem taken twice in one M50 refresh"
    [ "$(grep -c 'give_semaphore(winsys_sem);' <<< "$RFR")" = 4 ] || fail "winsys_sem not given back on every refresh path"
    grep -q 'if (rgb_layer_current())' <<< "$(sed -n '/^void rgba_buffer_clear()/,/^}/p' src/bmp.c)" || fail "layer clear writes without checking the layer"
    # Live LUT preview writes a library table only while it still holds Canon's
    # original or XPan's last write (Canon reuses that memory with the EVF on).
    LPUT=$(sed -n '/^static void lib_put/,/^}/p' src/xpan-m50.c)
    grep -q 'if (now!=lib_hash_orig\[i\] && now!=lib_hash_put\[i\]) { lib_addr\[i\]=0; live_lost++; return; }' <<< "$LPUT" || fail "Live LUT writes library tables without checking them"
    [ "$(grep -c 'memcpy((void \*)lib_addr\[' src/xpan-m50.c)" = 1 ] || fail "library tables written outside lib_put"
    # XPan log: hooks only fill rings; the card is written by XPan's task when idle.
    if grep -nE 'FIO_|snprintf|msleep|take_semaphore' <<< "$(sed -n '/^static void m50_ring_put/,/^static void m50_log_grain/p' src/xpan-m50.c)"; then fail "XPan log ring prints, blocks or writes the card"; fi
    grep -q 'if (get_halfshutter_pressed() || lens_info.job_state || RECORDING) return;' <<< "$(sed -n '/^static void m50_log_poll/,/^}/p' src/xpan-m50.c)" || fail "XPan log written while Canon is busy"
    echo "ok: bitmap layer checked under winsys_sem, Live LUT library writes checked, log written only when idle"
    # EVF (README "EVF screen"): Canon's 3:2 picture fills the EVF's top 480
    # of 540 rows, ML's -30..450: the whole layout moves up by the extra rows
    # (read from Canon's Ximr output size, bounded by ML's bitmap area), the
    # band is placed by the display hardware from the same LCD geometry scaled
    # to 1024, the HUD accepts rows down to BMP_H_MINUS, marks "nothing drawn"
    # with XPAN_HUD_NONE, and the clear and the dim cover those rows.
    grep -q 'if (w!=720 || h<=480 || h>480-2\*BMP_H_MINUS) return 0;' src/xpan-m50.c || fail "EVF extra rows not bounded by ML's bitmap area"
    grep -q 'return (xpan_frame_align == 1 ? 0 : panoramic_height) - xpan_m50_screen_extra_top();' src/cropmarks.c && grep -q '\*y -= xpan_m50_screen_extra_top();' src/cropmarks.c || fail "EVF layout no longer moves up as a whole"
    if grep -n 'crop_height += extra' src/cropmarks.c; then fail "EVF frame stretched again (the band has a fixed 65:24 height)"; fi
    grep -q 'bmp_fill(COLOR_EMPTY,0,BMP_H_MINUS,720,480-BMP_H_MINUS);' src/xpan-photo-hud.inc || fail "HUD clear leaves the EVF top rows"
    grep -q 'xpan_hud_last_clipped=(y<BMP_H_MINUS || y+XPAN_PHOTO_HUD_HEIGHT>480);' src/xpan-photo-hud.inc || fail "HUD refuses EVF rows above 0"
    if grep -nE 'last_hud_y *(>=|<) *0|last_hud_y=-1|xpan_hud_last_y *< *0' src/xpan-photo-hud.inc; then fail "HUD uses a negative row as 'nothing drawn' (EVF panels start at -30)"; fi
    grep -q 'for (int y=BMP_H_MINUS; y<480; y++) {' <<< "$(sed -n '/^static void canon_ui_enter/,/^}/p' src/xpan-m50.c)" || fail "Canon UI dim leaves the EVF top rows bright"
    DPE=$(sed -n '/^int xpan_m50_disp_evf(void \*p/,/^}/p' src/xpan-m50.c)
    grep -q 'if (disp_evf_block((const uint32_t \*)p) && disp_band(&src_y,&dst_y,&h)) {' <<< "$DPE" && grep -q 'if (ed+eh>682) ed=682-eh;' <<< "$DPE" && grep -q 'q\[6\]=es; q\[8\]=ed; q\[10\]=eh;' <<< "$DPE" || fail "EVF band placement changed or unbounded"
    grep -q '&& p\[9\]==1024 && p\[10\]==682;' src/xpan-m50.c || fail "EVF band no longer limited to Canon's EVF block"
    echo "ok: EVF layout on Canon's EVF picture, band placed by the display, bounded and cleared"
    # 6a3b. Grain stage probe (README "Grain stage probe"): read only, Canon's
    #       driver always runs through the trampoline, which replays E086DA10's
    #       first 8 bytes and resumes at E086DA18.
    grep -q "\.patch_addr = 0xe086da10" platform/M50.110/include/platform/mmu_patches.h || fail "grain stage probe address changed"
    grep -q "0x30, 0xb5, 0x04, 0x46, 0x97, 0xb0, 0x18, 0x21" platform/M50.110/include/platform/mmu_patches.h || fail "grain stage probe lost its original-bytes check"
    grep -q 'push {r4, r5, lr}\\n"' src/xpan-m50.c && grep -q 'sub sp, #0x5c\\n"' src/xpan-m50.c && grep -q 'movs r1, #0x18\\n"' src/xpan-m50.c && grep -q '\.word 0xE086DA19' src/xpan-m50.c || fail "grain stage trampoline does not replay E086DA10's first 8 bytes"
    CFG=$(sed -n '/^int xpan_m50_cf_grain_run/,/^}/p' src/xpan-m50.c)
    [ "$(grep -c 'xpan_m50_cf_grain_tramp(job)' <<< "$CFG")" = 2 ] || fail "grain stage probe does not always run Canon's driver"
    if grep -nE '(job|out|in)\)\[[^]]*\] *=[^=]' <<< "$CFG"; then fail "grain stage probe writes to Canon's job or image"; fi
    if grep -nE 'msleep|take_semaphore|FIO_|NotifyBox|snprintf|malloc' <<< "$CFG"; then fail "grain stage probe blocks or allocates"; fi
    echo "ok: grain stage probe read only unless XPan's grain runs, trampoline replays E086DA10's 8 bytes"
    # 6a3c. XPan's grain in Canon's grain stage (README "XPan's own grain in Canon's
    #       filter stage"): checked job, worker does it while ShtVfx waits blocked
    #       (bounded, under Canon's 10 s), grain capped then plain copies, cache
    #       order, no lock leak, abort stops writes.
    SG=$(sed -n '/^static int m50_stage_grain/,/^}/p' src/xpan-m50.c)
    SA=$(sed -n '/^static unsigned film_stage_apply/,/^}/p' src/xpan-m50.c)
    grep -q 'if (!film_stage_wanted()) return 0;' <<< "$SG" || fail "stage grain runs without Film grain"
    grep -q '(ci+size<=co || co+size<=ci)' <<< "$SG" && grep -q 'job\[5\]==width && job\[6\]==width' <<< "$SG" || fail "stage grain lost its job checks"
    grep -q 'int left=STAGE_WAIT_MS-(int)(get_ms_clock()-t0);' <<< "$SG" && grep -q 'got\[k\]=!take_semaphore(stage_parts\[k\].done,MAX(left,1));' <<< "$SG" || fail "stage grain wait not bounded (both workers within STAGE_WAIT_MS)"
    grep -q 'give_semaphore(grain_lock);' <<< "$SG" || fail "stage grain does not give the grain lock back"
    # Two workers, one per CPU, each its own half (README "Grain speed").
    grep -q 'static const int cpus\[2\]={1,0};' src/xpan-m50.c && grep -q 'task_create_ex(names\[k\],0x1c,0x2000,stage_worker,(void \*)(uintptr_t)k,cpus\[k\]);' src/xpan-m50.c || fail "grain stage workers not one per CPU"
    grep -q 'const unsigned half=n==2 ? (rows/2)&~(FILM_BAND_ROWS-1) : rows;' <<< "$SG" || fail "grain stage split not on whole bands"
    [ "$(grep -oE '^#define STAGE_WAIT_MS [0-9]+' src/xpan-m50.c | awk '{print $3}')" -lt 10000 ] || fail "stage wait not under Canon's 10 s"
    [ "$(grep -oE '^#define FILM_STAGE_BUDGET_US [0-9]+' src/xpan-m50.c | awk '{print $3}')" -le 7000000 ] || fail "stage grain budget too long"
    grep -q 'if (locked) give_semaphore(grain_lock);' <<< "$SG" || fail "stage grain fallback can leak the grain lock"
    if grep -nE 'msleep|snprintf|NotifyBox|FIO_|malloc' <<< "$SG$SA"; then fail "stage grain blocks, prints or allocates"; fi
    [ "$(tr -d '\n' <<< "$SA" | grep -o 'dcache_invalidate(src,bytes);        dcache_invalidate(dst,bytes);.*dcache_clean_invalidate(dst,bytes);        dcache_invalidate(src,bytes);' | wc -l)" = 1 ] || fail "stage grain cache order changed"
    grep -q 'if (grain_abort) break;' <<< "$SA" || fail "stage grain keeps writing after Canon stopped waiting"
    grep -q 'return xpan ? 0 : xpan_m50_cf_grain_tramp(job);' src/xpan-m50.c || fail "Canon's driver not run when XPan's grain does not"
    echo "ok: XPan grain in Canon's grain stage: checked job, bounded wait, budget, cache order"
    # 6a3d. Grain speed (README "Grain speed"): worker on CPU 1, texture written back
    #       after building, the stage row compiled with O2.
    grep -q 'dcache_clean((uint32_t)film_tex,FILM_TEX\*FILM_TEX);' src/xpan-m50.c && grep -q 'dcache_clean((uint32_t)film_amp,sizeof(film_amp));' src/xpan-m50.c || fail "grain texture not written back for the CPU 1 worker"
    grep -B1 '^static void film_row_copy' src/xpan-m50.c | grep -q 'optimize("O2")' || fail "stage row no longer compiled with O2"
    if grep -nE 'malloc\((1|2)u<<20\)' src/xpan-m50.c; then fail "MB-sized malloc (ML serves 512 KB+ from Canon's shooting pool)"; fi
    echo "ok: texture written back, O2 stage row"
    # 6a4. Nested submenus (camera test 2026-11-09: PLAY on Custom LUT went back
    #      instead of opening it): every M50 entry with children opens with
    #      select_Q = menu_open_submenu, as on the EOS M.
    NCHILD=$(grep -c "\.children=(struct menu_entry\[\])" src/xpan-m50.c)
    NOPEN=$(grep -c "\.select_Q=menu_open_submenu" src/xpan-m50.c)
    [ "$NCHILD" -le "$NOPEN" ] || fail "an M50 XPan submenu has no select_Q=menu_open_submenu (PLAY would go back)"
    echo "ok: M50 XPan submenus open with PLAY"
    # 6a5. Grain buffers stay under 512 KB: ML serves larger allocations from
    #      shoot_malloc, Canon's shooting pool (src/mem.c; camera test 2).
    grep -q "^#define FILM_TEX 256u" src/xpan-m50.c || fail "film grain texture size changed (512 makes a 512 KB buffer: Canon's shooting pool)"
    grep -q "#define XPAN_CUBE_MAX_COMPONENTS (33u\*33u\*33u\*3u)" src/xpan-cube.h || fail "cube buffer size changed (check it stays under 512 KB)"
    if grep -q 'film_grain=0' src/xpan-m50.c; then fail "film grain switches itself off during a shot (settings save)"; fi
    # Review 2026-11-09: cli() masks only the current core; tasks run on both M50
    # cores. Shared state between tasks uses DryOS semaphores or a seqlock.
    if grep -qE 'grain_busy|m50_hook_busy' src/xpan-m50.c; then fail "cli()-guarded busy flag is back (not safe across cores)"; fi
    grep -q 'take_semaphore_now(grain_lock)' src/xpan-m50.c || fail "grain lock missing"
    grep -q 'take_semaphore_now(m50_look_lock)' src/xpan-m50.c || fail "look lock missing"
    grep -q 'consistent=!(s1&1) && s1==c->seq;' src/xpan-m50.c || fail "options seqlock missing"
    # The abort path waits for the late worker before Canon's stage goes on.
    grep -q 'for (int k=0;k<n;k++) if (!got\[k\]) take_semaphore(stage_parts\[k\].done,1000);' src/xpan-m50.c || fail "grain abort no longer waits for the workers"
    # Canon grain test (README "Hardware grain"): PROP_LV_CFILTER is written only by
    # m50_cgrain_write_now, only in M, only from/to values Canon itself sends, never
    # PROP_MOVIE_CFILTER; menu requests through the sequence counter.
    if grep -nE 'prop_request_change[^;]*(MOVIE_CFILTER|8004006E|8004004F)' src/xpan-m50.c; then fail "creative filter property written outside the checked writer"; fi
    CGW=$(sed -n '/^static void m50_cgrain_write_now/,/^}/p' src/xpan-m50.c)
    # Two writes, both in the checked writer: XPan's value, and zeros for the
    # Live View refresh (README "Live View without Canon's filter").
    [ "$(grep -c '_prop_request_change(PROP_M50_LV_CFILTER' src/xpan-m50.c)" = 2 ] && [ "$(grep -c '_prop_request_change(PROP_M50_LV_CFILTER' <<< "$CGW")" = 2 ] || fail "PROP_LV_CFILTER written outside m50_cgrain_write_now"
    grep -q '_prop_request_change(PROP_M50_LV_CFILTER,z,sizeof(z));' <<< "$CGW" && grep -q 'uint32_t z\[10\]={0};' <<< "$CGW" || fail "Live View refresh writes something other than zeros"
    grep -q '_prop_request_change(PROP_M50_LV_CFILTER,v,sizeof(v));' <<< "$CGW" || fail "PROP_LV_CFILTER write not in m50_cgrain_write_now"
    grep -q 'if (!m50_pasm()) return;' <<< "$CGW" || fail "Canon grain writer lost its P/Tv/Av/M guard"
    grep -q 'if (!cf_lv_canon_value(cur))' <<< "$CGW" || fail "Canon grain writer lost its Canon-value guard"
    grep -q 'get_halfshutter_pressed() || lens_info.job_state || RECORDING' <<< "$CGW" || fail "Canon grain writer lost its busy guard"
    grep -q 'return w\[0\]==1 && w\[3\]<=2;' src/xpan-m50.c || fail "Canon value check changed (only kind 1, strength 0..2)"
    if grep -n 'cgrain_req' src/xpan-m50.c; then fail "old lossy Canon grain request flag"; fi
    # Test 6: take_semaphore(sem,0) waits forever in DryOS (src/dryos.h); in Canon's
    # ShtEncode task it froze the camera ("saving..." forever). Never in XPan M50 code.
    if grep -nE 'take_semaphore\([^,()]+, *0\)' src/xpan-m50.c; then fail "take_semaphore with timeout 0 waits forever (use take_semaphore_now)"; fi
    echo "ok: grain buffers under 512 KB, cross-core locks, PROP_LV_CFILTER only through its checked writer"
    if grep -qE ' tskmon_init$' <<< "$SYMS"; then fail "CONFIG_TSKMON is back in the M50 build"; fi
    echo "ok: no task monitor"
    # 6b. The boot log (ML/LOGS/XBOOT.LOG) stays in while M50 hangs are open.
    grep -q " m50_boot_log_start$" <<< "$SYMS" || fail "boot log (m50_boot_log_start) missing"
    echo "ok: boot log present"
    # 6c. Card swap after power-off needed a battery pull (2026-11-08): XPan
    #     stops card writes and ML's startup once Canon signals shutdown.
    grep -q " m50_shutdown_seen$" <<< "$SYMS" || fail "shutdown guard (m50_shutdown_seen) missing"
    grep -q " m50_wait_for_real_start$" <<< "$SYMS" || fail "early shutdown wait (m50_wait_for_real_start) missing"
    echo "ok: shutdown guard present"
    # 6c2. Dead after every power-off (2026-11-08): Canon's check start is caught by
    #      its GUI command, XPan's card writes stop at power-off, Canon's dial
    #      callback goes back, and the PROP_ISO slave can be turned off.
    grep -q " m50_check_start$" <<< "$SYMS" || fail "check-start flag (m50_check_start) missing"
    grep -q "m50_check_start = cmd" src/gui.c || fail "gui.c no longer flags Canon's check command"
    grep -q " xpan_m50_card_ok$" <<< "$SYMS" || fail "power-off card guard (xpan_m50_card_ok) missing"
    grep -q "xpan_m50_card_ok()) return" src/xpan-hud-controls.c || fail "key log writes without the power-off guard"
    grep -q "xpan_m50_card_ok()) continue" src/xpan-m50.c || fail "XPan M50 task works without the power-off guard"
    grep -q "FIO_RemoveFile(ISO_GUARD);" src/xpan-m50.c && [ "$(grep -c 'FIO_RemoveFile(ISO_GUARD)' src/xpan-m50.c)" = 1 ] || fail "ISO guard removed from more than one place (one may run at power-off)"
    grep -q "ui_hook_remove(0)" src/xpan-m50.c || fail "dial callback not restored at power-off"
    grep -q '"xpan.m50.iso_reader"' src/xpan-m50.c || fail "ISO reader on/off setting missing"
    grep -q '!M50_CHECK_CMD() && gui_wait' src/init.c || fail "GUI wait no longer ends on Canon's check command"
    echo "ok: check start caught, no XPan card writes at power-off"
    # 6c4. Switch-on after a door close hung inside Canon's init_task (2026-11-08):
    #      no ML/XPan GUI handler may run before ML has started.
    grep -q "if (ml_handlers_on && !magic_is_off())" src/gui.c || fail "gui.c runs ML handlers before ML has started"
    grep -q "if (!ml_started) return 1; }" src/xpan-hud-controls.c || fail "XPan key handler runs before ML has started"
    echo "ok: no GUI handlers before ML has started"
    # 6c6. Settings are saved within 2 s of a change, so a hang cannot lose them.
    #      ML's own power-off is upstream again: skipping it left the module lock
    #      file behind ("Camera was not shut down cleanly", 2026-11-08).
    grep -q " config_save_if_changed$" <<< "$SYMS" || fail "settings save-on-change (config_save_if_changed) missing"
    grep -q "config_save_if_changed();" src/xpan-m50.c || fail "XPan M50 task no longer saves settings on change"
    if grep -q "CONFIG_M50" src/tasks.c; then fail "tasks.c has M50 power-off changes again (module lock file stays behind)"; fi
    echo "ok: settings saved on change, upstream power-off"
    # 6c6b. No test or probe code left from the startup-hang hunt (2026-11-08).
    if grep -qE "M50_TEST|M50_TRACE_INIT_TASK|M50_LED_STAGE|m50_heartbeat|m50_engine_ch0|Memory-retention probe" src/*.c src/*.h platform/M50.110/Makefile; then fail "startup-hang test/probe code is back"; fi
    echo "ok: no startup-hang test or probe code"
    # 6c6c. Core 1 is parked in Canon's bootloader by reboot.c and released only in
    #       boot_pre_init_task, after DryOS wrote its start marker (README "Startup
    #       hang: core 1 and its start entry"). Releasing it earlier (the first
    #       parking attempt, in copy_and_restart) did not work: it must not come back.
    grep -q '"m50_boot_wait: .word 0xE0004911' src/reboot.c || fail "reboot.c no longer parks core 1"
    grep -q " m50_core1_released$" <<< "$SYMS" || fail "core-1 release (m50_core1_released) missing"
    grep -q "m50_release_core1();" src/init.c || fail "boot_pre_init_task no longer releases core 1"
    if grep -q "BFE01FC4" src/boot-d678.c; then fail "boot-d678.c releases core 1 (too early: before DryOS sets the marker)"; fi
    grep -q "!= 0x20101231) return;" src/init.c || fail "core-1 release no longer checks the DryOS marker"
    echo "ok: core 1 parked, released after DryOS sets its start marker"
    # 6d. Property writes: only XPan's checked ISO/shutter/aperture writers (xpan-m50.c); ML's
    #     general prop_request_change stays off on the M50.
    grep -qE "^#undef CONFIG_PROP_REQUEST_CHANGE" platform/M50.110/features.h || fail "CONFIG_PROP_REQUEST_CHANGE is no longer undefined for the M50"
    grep -q " m50_iso_write_request$" <<< "$SYMS" || fail "checked ISO writer missing"
    # 6d2. Learned settings (EV, WB, drive, metering, format): XPan writes only option
    #      bytes it recorded from Canon (README "Controls, step 6"); the writer must
    #      keep its recorded-option guard and stay out of busy/half-press states.
    grep -q " m50_ctl_write_now$" <<< "$SYMS" || grep -q "static void m50_ctl_write_now" src/xpan-m50.c || fail "learned-settings writer missing"
    grep -q "if (idx>=c->nopts || get_halfshutter_pressed() || lens_info.job_state || RECORDING)" src/xpan-m50.c || fail "learned-settings writer lost its guard"
    if grep -n "_prop_request_change(" src/xpan-m50.c | grep -vqE "PROP_ISO|PROP_SHUTTER|PROP_APERTURE|PROP_M50_LV_CFILTER|c->props\[p\]|extern void"; then fail "a property write outside the checked writers in xpan-m50.c"; fi
    echo "ok: property writes limited to the checked ISO/shutter/aperture writers and learned options"
    # 6d3. Second test (README "Controls, step 6b"): PROP_AE has the compensation in
    #      byte 1 (thirds), and not all PICQ aliases report on the M50. The EV key and
    #      HUD must read byte 1, an option must not need every property, and empty
    #      properties must never be written.
    grep -q "(int16_t)(v\[0\].b\[0\] | (v\[0\].b\[1\]<<8))" src/xpan-m50.c || fail "EV sort key does not read PROP_AE byte 1"
    grep -q "m50_ev_thirds()" src/xpan-photo-hud.inc || fail "M50 HUD EV does not use m50_ev_thirds (lens_info.ae is always 0 there)"
    grep -q "if (!reported || m50_opt_find(c,snap)>=0" src/xpan-m50.c || fail "learning still needs every property of a setting (format never learned)"
    grep -q "if (!v->len || v->len>M50_OPT_BYTES) continue;" src/xpan-m50.c || fail "learned writer may write an empty (unreported) property"
    grep -q "#define M50_OPT_MAX 24" src/xpan-m50.c || fail "learned options limit below 24 (EV needs 19)"
    echo "ok: EV byte 1, partial PICQ options, empty properties never written"
    # 6d4. M50 PICQ layout (XM50OPTS.LOG): kind in byte 1 (1 JPEG, 6 RAW, 7 RAW+JPEG),
    #      not the EOS M's 0x60000 bits. Format step and HUD label must use it.
    grep -q "#define M50_PICQ_KIND(q)   (((q)>>8)&0xFF)" src/xpan-m50.c || fail "M50 PICQ kind not read from byte 1"
    if sed -n '/^static int m50_fmt_picq/,/^int m50_ev_thirds/p' src/xpan-m50.c | grep -q "0x60000"; then fail "M50 format code uses the EOS M PICQ layout (0x60000)"; fi
    grep -q "m50_format_label()" src/xpan-photo-hud.inc || fail "M50 HUD FMT label does not use m50_format_label"
    echo "ok: M50 PICQ layout used for the format step and label"
    # 7. Against the camera's own ROM, when the dump is on disk.
    ROM0=extras/M50/ROMS/ROM0.BIN
    if [ -f "$ROM0" ]; then
      python3 -I build_tools/check_m50_rom.py "$ROM0" platform/M50.110/stubs.S || fail "ROM check"
    else
      echo "note: $ROM0 not found, ROM check skipped"
    fi
  fi

  # Archive the verified package under its date and hash.
  HASH=$(sha256sum "$D/autoexec.bin" | cut -c1-12)
  ARCH=platform/$CAM/xpan_builds/$(date +%Y%m%d-%H%M%S)-$HASH
  mkdir -p "$ARCH"
  cp "$D/magiclantern.zip" "$D/autoexec.bin" "$ARCH/"
  sha256sum "$D/magiclantern.zip" "$D/autoexec.bin" "$D/magiclantern.bin"
  echo "archived: $ARCH"
} 2>&1 | tee -a "$LOG"
echo "XPAN BUILD DONE ($CAM)"

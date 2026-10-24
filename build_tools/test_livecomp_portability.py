#!/usr/bin/env python3
"""Check runtime rejection, the Q adapter and shared ARM structure layouts."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
from livecomp_test_utils import function

ROOT=Path(__file__).resolve().parents[1]
MOD=ROOT/'modules/livecomp'

def main():
    for p in list(MOD.glob('*.c'))+list(MOD.glob('*.h')):
        s=p.read_text()
        assert not re.search(r'#\s*include.*platform[/\\]',s),p
        assert not re.search(r'#\s*define\s+CONFIG_(EOSM|DIGIC|DRYOS|VXWORKS|3_2_SCREEN)',s),p
    assert 'platform/' not in (MOD/'Makefile').read_text()
    lifecycle=(MOD/'livecomp.c').read_text()
    code=r'''
#include <assert.h>
#include <string.h>
#include "module.h"
static const char *model="5D3",*fw="1.2.3";
int is_camera(const char *m,const char *f) { return !strcmp(model,m) && !strcmp(fw,f); }
#include "camera-profile.h"
struct lc_camera_profile livecomp_camera;
static unsigned initialized,polls,keys,shutdowns,last_key;
struct event { int type,param; void *obj; int arg; };
static void *get_ml_card(void) { return 0; }
static void livecomp_lv_poll(void *p) { (void)p;polls++; }
static unsigned livecomp_lv_key(unsigned k) { keys++;last_key=k;return 0; }
static void livecomp_lv_shutdown(void) { shutdowns++; }
'''
    for name in ('livecomp_init(', 'livecomp_poll(', 'livecomp_key(', 'livecomp_raw_key(', 'livecomp_deinit('):
        text=function(lifecycle,name)
        if name=='livecomp_raw_key(':
            text=text.replace('unsigned context','uintptr_t context')
        code+=text
    code+=r'''
int main(void) {
    assert(livecomp_init()==1 && !initialized && !polls);
    livecomp_poll(0);assert(livecomp_key(MODULE_KEY_Q)==1);
    assert(livecomp_raw_key(0)==1);livecomp_deinit();
    assert(!polls && !keys && !shutdowns && !livecomp_camera.cache_alias_mask);
    model="EOSM";assert(livecomp_init()==1); /* wrong firmware */
    fw="2.0.2";assert(livecomp_init()==0 && polls==1 && initialized);
    assert(livecomp_camera.raw_channel==18 && livecomp_camera.cache_alias_mask==0x40000000u);
    assert(livecomp_key(MODULE_KEY_PRESS_SET)==0 && last_key==MODULE_KEY_PRESS_SET);
    struct event e={0,0x1d,0,0};
    assert(livecomp_raw_key((uintptr_t)&e)==0 && last_key==MODULE_KEY_Q);
    e.param=105;unsigned before=keys;assert(livecomp_raw_key((uintptr_t)&e)==1 && keys==before);
    livecomp_deinit();assert(shutdowns==1);
    return 0;
}
'''
    # Check every member the module accesses in core-owned/shared structures.
    layouts={
        'lens_info':'raw_iso raw_aperture raw_shutter wb_mode kelvin WBGain_R WBGain_B wbs_gm wbs_ba job_state aperture focal_len',
        'raw_info':'buffer width height pitch frame_size bits_per_pixel black_level white_level active_area dng_active_area color_matrix1 calibration_illuminant1 cfa_pattern',
        'card_info':'drive_letter',
        'display_filter_buffers':'src_buf dst_buf',
        'event':'param',
    }
    entries=[]
    for struct,fields in layouts.items():
        entries.append(f'sizeof(struct {struct})')
        entries.extend(f'offsetof(struct {struct},{field})' for field in fields.split())
    abi='#include "dryos.h"\n#include "lens.h"\n#include "raw.h"\n#include <stddef.h>\n'
    abi+='const unsigned layout[] __attribute__((section(".lc_abi")))={'+','.join(entries)+'};\n'
    with tempfile.TemporaryDirectory() as tmp:
        p=Path(tmp);(p/'test.c').write_text(code)
        subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',
                        '-I'+str(ROOT/'src'),'-I'+str(MOD),str(p/'test.c'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
        (p/'abi.c').write_text(abi)
        for name,flags in [('module',['-DMODULE']),('core',['-DCONFIG_EOSM=1'])]:
            subprocess.run(['arm-none-eabi-gcc','-std=gnu99','-march=armv5te','-DCONFIG_MAGICLANTERN',
                '-I'+str(ROOT/'src'),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'platform/EOSM.202'),
                *flags,'-c',str(p/'abi.c'),'-o',str(p/(name+'.o'))],check=True)
            subprocess.run(['arm-none-eabi-objcopy','-O','binary','-j','.lc_abi',str(p/(name+'.o')),str(p/(name+'.bin'))],check=True)
        assert (p/'module.bin').read_bytes()==(p/'core.bin').read_bytes(), 'Shared structure ABI differs'
    print('PASS: unsupported model/firmware rejected, guarded Q adapter, no platform includes/macros, shared ARM layouts match EOS M core')

if __name__=='__main__': main()

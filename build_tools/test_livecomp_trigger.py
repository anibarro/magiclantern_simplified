#!/usr/bin/env python3
"""Exercise the real module key gate without camera hardware."""
from pathlib import Path
import os
import subprocess
import tempfile
from livecomp_test_utils import function

ROOT = Path(__file__).resolve().parents[1]

def main():
    source = (ROOT/'modules/livecomp/livecomp-lv.c').read_text()
    code = r'''
#include <assert.h>
#include <stdint.h>
#include "module.h"
int is_camera(const char *a,const char *b) { (void)a;(void)b;return 1; }
#include "camera-profile.h"
struct lc_camera_profile livecomp_camera;
struct event { int type,param; void *obj; int arg; };
#define IS_FAKE(e) ((e)->arg == -123456)
static unsigned initialized=1,set_release_blocked;
#include "fullres-lv.h"
static unsigned enabled,attempted,running,requested,trigger_ms,now,stops,frlv_phase,frlv_go;
static int ml_shutdown_requested,menu,gui_state,lv,LV_PAUSED,movie,lv_dispsize,RECORDING,half;
static struct { int job_state; } lens_info;
static const char *status;
#define GUISTATE_IDLE 0
static int gui_menu_shown(void) { return menu; }
static int is_movie_mode(void) { return movie; }
static int get_halfshutter_pressed(void) { return half; }
static unsigned get_ms_clock(void) { return now; }
static void stop(unsigned kind) { assert(kind==1);stops++; }
'''
    code += function(source, 'trigger_ready(')+function(source.replace('int livecomp_lv_key(', 'static int livecomp_lv_key('), 'livecomp_lv_key(')
    lifecycle=(ROOT/'modules/livecomp/livecomp.c').read_text()
    assert lifecycle.index('MODULE_CBR(CBR_KEYPRESS_RAW') < lifecycle.index('MODULE_CBR(CBR_KEYPRESS,')
    code += function(lifecycle,'livecomp_raw_key(').replace('unsigned context','uintptr_t context')
    code += r'''
static int dispatch(int param,int fake) {
    struct event e={0,param,0,fake ? -123456:0};
    if (!livecomp_raw_key((uintptr_t)&e)) return 0;
    return livecomp_lv_key(param==4 ? MODULE_KEY_PRESS_SET:param==5 ? MODULE_KEY_UNPRESS_SET:0);
}
int main(void) {
    assert(lc_camera_configure(&livecomp_camera));
    lv=movie=lv_dispsize=1;
    assert(livecomp_lv_key(MODULE_KEY_Q)==1 && !requested); /* Off */
    enabled=1;assert(!requested); /* enabling alone never queues */
    int *blocked[]={&menu,&gui_state,&LV_PAUSED,&RECORDING,&half,&lens_info.job_state,&ml_shutdown_requested};
    for (unsigned i=0;i<sizeof(blocked)/sizeof(*blocked);i++) {
        *blocked[i]=1;assert(livecomp_lv_key(MODULE_KEY_Q)==1 && !requested);*blocked[i]=0;
    }
    lv=0;assert(!trigger_ready());lv=1;
    movie=0;assert(!trigger_ready());movie=1;
    lv_dispsize=5;assert(!trigger_ready());lv_dispsize=1;
    assert(livecomp_lv_key(MODULE_KEY_PRESS_HALFSHUTTER)==1 && !requested);
    assert(livecomp_lv_key(MODULE_KEY_UNPRESS_SET)==1 && !requested);
    now=1000;assert(livecomp_lv_key(MODULE_KEY_Q)==0 && requested && trigger_ms==1000);
    assert(livecomp_lv_key(MODULE_KEY_PRESS_SET)==0 && requested && !stops);
    running=attempted=1;requested=0;
    now=1100;assert(livecomp_lv_key(MODULE_KEY_PRESS_SET)==0 && !stops);
    now=1600;assert(livecomp_lv_key(MODULE_KEY_Q)==0 && stops==1);
    assert(livecomp_lv_key(MODULE_KEY_INFO)==0);   /* other keys are held while running */
    /* Exposure preview: SET starts the composite (once), half-shutter
     * cancels; during the composite SET stops and half-shutter does nothing. */
    stops=0;frlv_phase=LC_FRLV_PREVIEW;frlv_go=0;
    now=2200;assert(livecomp_lv_key(MODULE_KEY_PRESS_SET)==0 && frlv_go && !stops && trigger_ms==2200);
    now=2300;assert(livecomp_lv_key(MODULE_KEY_PRESS_SET)==0 && !stops);      /* same press */
    assert(livecomp_lv_key(MODULE_KEY_PRESS_HALFSHUTTER)==0 && !stops);       /* already starting */
    frlv_go=0;assert(livecomp_lv_key(MODULE_KEY_PRESS_HALFSHUTTER)==0 && stops==1);
    stops=0;frlv_phase=LC_FRLV_COMPOSE;
    assert(livecomp_lv_key(MODULE_KEY_PRESS_HALFSHUTTER)==0 && !stops);
    now=3000;assert(livecomp_lv_key(MODULE_KEY_PRESS_SET)==0 && stops==1 && !frlv_go);
    frlv_phase=LC_FRLV_SETUP;stops=1;
    assert(livecomp_lv_key(0)==1);
    running=0;assert(livecomp_lv_key(MODULE_KEY_Q)==1 && !requested); /* reboot guard */
    /* Real physical down in idle LV was consumed upstream. Release survives. */
    attempted=running=requested=stops=set_release_blocked=0;now=5000;
    assert(dispatch(5,0)==0 && requested && trigger_ms==5000);
    running=attempted=1;requested=0;now=5100;
    dispatch(5,0);assert(!stops); /* duplicate release cannot immediately stop */
    now=5600;dispatch(5,0);assert(stops==1); /* next short press stops */
    attempted=running=requested=stops=set_release_blocked=0;now=7000;
    dispatch(0x1d,1);assert(requested && set_release_blocked); /* long fallback */
    running=attempted=1;requested=0;
    now=7100;dispatch(5,1);assert(!stops && set_release_blocked);
    now=9000;dispatch(5,0);assert(!stops && !set_release_blocked);
    now=9600;dispatch(5,0);assert(stops==1);
    /* A press begun in either menu cannot start on release after closing it. */
    attempted=running=requested=stops=set_release_blocked=0;
    menu=1;dispatch(4,0);assert(!requested && set_release_blocked);
    menu=0;dispatch(5,0);assert(!requested && !set_release_blocked);
    gui_state=1;dispatch(0x1d,0);assert(!requested && set_release_blocked);
    gui_state=0;dispatch(5,0);assert(!requested);
    dispatch(5,1);assert(!requested); /* synthetic release alone is not a tap */
    enabled=0;dispatch(5,0);assert(!requested);
    enabled=1;half=1;dispatch(5,0);assert(!requested);half=0;
    struct event internal={2,5,0,0};
    assert(livecomp_raw_key((uintptr_t)&internal)==1 && !requested);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        p=Path(tmp);(p/'test.c').write_text(code)
        subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',
                        '-I'+str(ROOT/'src'),'-I'+str(ROOT/'modules/livecomp'),str(p/'test.c'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
    print('PASS: short physical SET release, long synthetic/physical release suppression, menu-exit isolation, trigger/Stop gates, preview start/cancel keys')

if __name__ == '__main__':
    main()

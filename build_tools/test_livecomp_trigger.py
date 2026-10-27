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
#include <string.h>
#include "module.h"
int is_camera(const char *a,const char *b) { (void)a;(void)b;return 1; }
#include "camera-profile.h"
struct lc_camera_profile livecomp_camera;
struct event { int type,param; void *obj; int arg; };
#define IS_FAKE(e) ((e)->arg == -123456)
static unsigned initialized=1,set_release_blocked;
#include "fullres-lv.h"
static unsigned enabled,attempted,running,requested,trigger_ms,now,stops;
static int preview_only,live_mode;
static int bulb(void) { return live_mode==(int)LC_MODE_BULB; }
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
    code += 'static int session_key(unsigned code);\nstatic unsigned keylog_n;\nvoid livecomp_frlv_key_log(unsigned c,unsigned p,unsigned r){(void)c;(void)p;(void)r;keylog_n++;}\n'
    code += 'static int livecomp_lv_running(void) { return running; }\n'
    code += function(source, 'trigger_ready(')+function(source.replace('int livecomp_lv_key(', 'static int livecomp_lv_key('), 'livecomp_lv_key(')
    code += function(source.replace('static int session_key(unsigned code);\n',''), 'session_key(')
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
    assert(!strcmp(status,"Starting the composite"));
    requested=0;live_mode=1;assert(livecomp_lv_key(MODULE_KEY_Q)==0 && requested && !strcmp(status,"Starting the bulb"));
    live_mode=0;
    assert(livecomp_lv_key(MODULE_KEY_PRESS_SET)==0 && requested && !stops);
    running=attempted=1;requested=0;
    now=1100;assert(livecomp_lv_key(MODULE_KEY_PRESS_SET)==0 && !stops);
    now=1600;assert(livecomp_lv_key(MODULE_KEY_Q)==0 && stops==1);
    assert(livecomp_lv_key(MODULE_KEY_INFO)==0);   /* other keys are held while running */
    /* While running only SET/Q act (stop); every other key is held. */
    stops=0;now=2200;
    assert(livecomp_lv_key(MODULE_KEY_MENU)==0 && livecomp_lv_key(MODULE_KEY_PRESS_HALFSHUTTER)==0 &&
           livecomp_lv_key(MODULE_KEY_WHEEL_RIGHT)==0 && livecomp_lv_key(MODULE_KEY_PRESS_LEFT)==0 && !stops);
    assert(livecomp_lv_key(MODULE_KEY_PRESS_SET)==0 && stops==1);
    stops=1;
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
    /* While running, Canon never receives SET/Q: press and release are both
     * consumed, one click is one action (press if seen, else release). */
    attempted=running=1;requested=stops=set_release_blocked=0;now=20000;trigger_ms=0;
    { struct event e={0,4,0,0};assert(livecomp_raw_key((uintptr_t)&e)==0 && stops==1); }
    { struct event e={0,5,0,-123456};assert(livecomp_raw_key((uintptr_t)&e)==0 && stops==1); }  /* fake release */
    { struct event e={0,5,0,0};assert(livecomp_raw_key((uintptr_t)&e)==0 && stops==1 && !set_release_blocked); }
    now=21000;{ struct event e={0,5,0,0};assert(livecomp_raw_key((uintptr_t)&e)==0 && stops==2); } /* release only */
    now=22000;{ struct event e={0,0x1d,0,0};assert(livecomp_raw_key((uintptr_t)&e)==0 && stops==3); }
    { struct event e={0,0x50,0,0};assert(livecomp_raw_key((uintptr_t)&e)==1); }   /* other keys: portable path */
    assert(keylog_n>=6);
    attempted=running=0;set_release_blocked=0;
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
    print('PASS: short physical SET release, long synthetic/physical release suppression, menu-exit isolation, trigger/Stop gates, keys held while running')

if __name__ == '__main__':
    main()

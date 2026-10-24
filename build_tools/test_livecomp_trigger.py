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
static struct { int gui_idle; } livecomp_camera;
static unsigned enabled,attempted,running,requested,trigger_ms,display_setup,now,stops;
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
    code += r'''
int main(void) {
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
    display_setup=1;assert(livecomp_lv_key(MODULE_KEY_INFO)==1);display_setup=0;
    assert(livecomp_lv_key(0)==1);
    running=0;assert(livecomp_lv_key(MODULE_KEY_Q)==1 && !requested); /* reboot guard */
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        p=Path(tmp);(p/'test.c').write_text(code)
        subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',
                        '-I'+str(ROOT/'src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
    print('PASS: Off/menu/playback/mode/shutter gates, intentional start, duplicate press, Stop and reboot guard')

if __name__ == '__main__':
    main()

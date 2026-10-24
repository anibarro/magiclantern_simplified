#!/usr/bin/env python3
"""Exercise actual session retirement and filename selection across retries."""
from pathlib import Path
import os
import subprocess
import tempfile
from livecomp_test_utils import function

ROOT=Path(__file__).resolve().parents[1]

def main():
    camera=(ROOT/'modules/livecomp/livecomp-lv.c').read_text()
    code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "lv-ring.h"
#define COUNT(x) (sizeof(x)/sizeof((x)[0]))
struct card_info { const char *drive_letter; } card={"A"};
struct lv_session { unsigned quarantined; struct lc_ring ring;
    struct card_info *card; char path[100]; };
static struct lv_session *session;
static unsigned attempted,requested,running,armed,preview_enabled,preview_ready;
static unsigned preview_writing,display_setup,stop_requested,capture_reading,preview_reading;
static unsigned frees,occupied;
static unsigned cli(void) { return 0; }
static void sei(unsigned i) { (void)i; }
static void checked_free(void *p) {
    assert(!session && running && !armed && !preview_enabled);
    assert(!capture_reading && !preview_reading);
    frees++;free(p);
}
#define free checked_free
static int FIO_GetFileSize(const char *path,uint32_t *size) {
    unsigned n;assert(sscanf(path,"A:/LVCP%4u",&n)==1);*size=10;
    /* Existing DNG, failed-save LOG and CSV-only evidence all reserve names. */
    const char *ext=strrchr(path,'.');
    const char *types[]={".DNG",".LOG",".CSV"};
    return n<occupied && !strcmp(ext,types[n%3]) ? 0:-1;
}
'''
    code+=function(camera,'finish_session(')+function(camera,'choose_path(')
    code+=r'''
int main(void) {
    for (unsigned i=0;i<1000;i++) {
        struct lv_session *s=calloc(1,sizeof(*s));assert(s);
        session=s;s->card=&card;lc_ring_init(&s->ring);
        assert(s->ring.pending==-1 && !s->ring.issued);
        assert(choose_path(s));char expected[100];
        snprintf(expected,sizeof(expected),"A:/LVCP%04u.DNG",i);
        assert(!strcmp(s->path,expected));occupied++;
        int slot=lc_ring_arm(&s->ring);assert(slot>=0 && s->ring.issued==1);
        assert(lc_ring_complete(&s->ring)==slot && lc_ring_take(&s->ring)==slot);
        assert(lc_ring_release(&s->ring,slot));
        attempted=running=armed=preview_enabled=preview_ready=preview_writing=display_setup=stop_requested=1;
        finish_session(s);
        assert(!session && !running && !attempted && !requested && !stop_requested);
        assert(!armed && !preview_enabled && !preview_ready && !preview_writing && !display_setup);
        assert(frees==i+1);
    }
    /* Memory/task creation failures have no session and must permit retry. */
    attempted=running=1;finish_session(NULL);assert(!attempted && !running && frees==1000);
    for (unsigned kind=0;kind<3;kind++) {
        struct lv_session *s=calloc(1,sizeof(*s));session=s;
        s->quarantined=kind==0;capture_reading=kind==1;preview_reading=kind==2;
        attempted=running=1;finish_session(s);
        assert(attempted && !running && session==s && s->quarantined && frees==1000);
        /* Simulate reboot only inside the host fixture to test the next case. */
        capture_reading=preview_reading=0;s->quarantined=0;running=1;
        finish_session(s);frees=1000;
    }
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        p=Path(tmp);(p/'test.c').write_text(code)
        subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',
                        '-I'+str(ROOT/'modules/livecomp'),str(p/'test.c'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
    print('PASS: 1000 session resets, first-frame ownership, unique DNG/LOG/CSV names, allocation retry and quarantine retention')

if __name__=='__main__': main()

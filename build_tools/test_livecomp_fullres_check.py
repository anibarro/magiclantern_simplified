#!/usr/bin/env python3
"""Exercise real diagnostic allocation/retirement with mocked native jobs."""
from pathlib import Path
import os
import subprocess
import tempfile
from livecomp_test_utils import function

ROOT=Path(__file__).resolve().parents[1]
MOD=ROOT/'modules/livecomp'


def main():
    source=(MOD/'fullres-check.c').read_text()
    fields=source[source.index('struct check_result {'):source.index('static unsigned physical')]
    code=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "fullres-layout.h"
#include "native-max.h"
#define FIO_MAX_PATH_LENGTH 128
#define UILOCK_SHUTTER 1
struct card_info { int x; } card;
static unsigned observing,vsyncs,last_ms,max_gap_ms,icu_uilock,ml_shutdown_requested;
static unsigned pic_quality=0x4060000;
int lc_native_resources(struct lc_resource_snapshot *s) { memset(s,0,sizeof(*s));return 0; }
static unsigned allocated,freed,powered,closed,prepared,jobs,reports,last_stage,last_unsafe;
static unsigned cpu_saved,report_fail_at;
static unsigned fail_after=4,fail_close,fail_job,unsafe_job,bad_guard,cancelled,context_bad,rom_bad;
static unsigned char *arena,*first;
static unsigned is_camera(const char *a,const char *b) { (void)a;(void)b;return 1; }
static unsigned cli(void) { return 0; }
static void sei(unsigned x) { (void)x; }
static unsigned physical(void *p) { return (unsigned)(uintptr_t)p; }
static unsigned get_us_clock(void) { static unsigned t;return t+=100; }
#define MIN(a,b) ((a)<(b)?(a):(b))
static void lc_raw14_max(void *a,void *b,unsigned n) { assert(a && b && n); }
static unsigned get_ms_clock(void) { static unsigned t;return t+=10; }
static void msleep(unsigned x) { (void)x; }
static void sync_caches(void) {}
static void gui_uilock(unsigned x) { icu_uilock=x; }
static void powersave_prohibit(void) { powered++; }
static void powersave_permit(void) { powered--; }
static struct card_info *get_ml_card(void) { return &card; }
static int valid(const volatile unsigned *c,struct card_info *p) { return !*c && p==&card; }
static int choose_path(char *p,struct card_info *c) { (void)c;strcpy(p,"check.LOG");return 1; }
static void *fio_malloc(unsigned bytes) {
    if (bytes==1024*1024) return NULL;
    if (bytes==128) return arena+4*LC_FR_ALLOCATION+1024;
    if (allocated==fail_after) return 0;
    void *p=arena+allocated*LC_FR_ALLOCATION;allocated++;return p;
}
static void fio_free(void *p) { assert(p);freed++; }
static void fill(uint8_t *p,unsigned i) { if (!i) first=p; }
int lc_native_snapshot(uint32_t s[12]) {
    memset(s,0,48);s[1]=s[4]=1;s[6]=40;s[7]=2;s[8]=41;s[9]=4;s[10]=5;
    if (context_bad) s[1]=0;
    return !rom_bad;
}
unsigned lc_native_engine_prepare(uint32_t *p) { assert(p);prepared++;return 0; }
unsigned lc_native_engine_close(void) { closed++;return fail_close; }
unsigned lc_native_engine_max(const struct lc_native_plane *a,const struct lc_native_plane *b,
    const struct lc_native_plane *out,struct lc_native_engine_result *r) {
    assert(b->pixels!=a->pixels && reports>=2 && cpu_saved);
    if(!lc_fr_case_inplace(jobs)) assert(out->pixels>=a->storage+a->bytes);
    else assert(out->pixels==a->pixels);
    jobs++;memset(r,0,sizeof(*r));r->resume_safe=!unsafe_job;r->elapsed_us=100;
    if (bad_guard) first[-1]=0;
    return fail_job;
}
'''+fields+r'''
static int report(const char *p,const struct check_result *r) {
    (void)p;reports++;last_stage=r->stage;last_unsafe=r->unsafe;cpu_saved=r->cpu_checked;return reports!=report_fail_at;
}
static void verify(uint8_t *a,uint8_t *b,struct check_result *r,unsigned output_row,unsigned rows) {
    assert(a && b && rows && output_row);r->source_ok=1;r->checked=LC_FR_WIDTH*LC_FR_HEIGHT;
}
'''+function(source,'probe_segments(')+function(source,'guards_intact(')+function(source.replace('unsigned livecomp_fullres_check(', 'static unsigned livecomp_fullres_check('),'livecomp_fullres_check(')+r'''
static void reset(void) {
    allocated=freed=powered=closed=prepared=jobs=reports=last_stage=last_unsafe=0;
    cpu_saved=report_fail_at=0;icu_uilock=0;fail_after=4;fail_close=fail_job=unsafe_job=bad_guard=cancelled=context_bad=rom_bad=0;
}
int main(void) {
    arena=mmap((void *)0x01000000,4*LC_FR_ALLOCATION+4096,PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(arena==(void *)0x01000000);const char *status=0;
    reset();assert(!livecomp_fullres_check(&card,&cancelled,&status));
    assert(allocated==4 && freed==5 && closed==1 && jobs==LC_FR_CASES && !powered && !icu_uilock);
    assert(last_stage==6 && !last_unsafe);
    reset();report_fail_at=5;assert(!livecomp_fullres_check(&card,&cancelled,&status));
    assert(!jobs && freed==5 && closed==1 && last_stage==4);
    reset();report_fail_at=6;assert(!livecomp_fullres_check(&card,&cancelled,&status));
    assert(jobs==1 && freed==5 && closed==1 && last_stage==4);
    reset();context_bad=1;assert(!livecomp_fullres_check(&card,&cancelled,&status));
    assert(allocated==4 && freed==4 && !prepared && !jobs && !powered && !icu_uilock);
    reset();fail_after=1;assert(!livecomp_fullres_check(&card,&cancelled,&status));
    assert(allocated==1 && freed==1 && !prepared && !jobs);
    reset();fail_job=LC_NATIVE_BUSY;assert(!livecomp_fullres_check(&card,&cancelled,&status));
    assert(freed==5 && closed==1 && jobs==1 && !last_unsafe);
    reset();fail_job=LC_NATIVE_STOP;unsafe_job=1;
    assert(livecomp_fullres_check(&card,&cancelled,&status));
    assert(!freed && !closed && jobs==1 && last_unsafe && !powered && !icu_uilock);
    reset();fail_close=1;assert(livecomp_fullres_check(&card,&cancelled,&status));
    assert(!freed && closed==1 && last_unsafe);
    reset();bad_guard=1;assert(livecomp_fullres_check(&card,&cancelled,&status));
    assert(!freed && !closed && last_unsafe && jobs==1);
    reset();rom_bad=1;assert(!livecomp_fullres_check(&card,&cancelled,&status));
    assert(!allocated && !prepared && !powered && !icu_uilock);
    reset();cancelled=1;assert(!livecomp_fullres_check(&card,&cancelled,&status));
    assert(!allocated && !reports && !powered);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        p=Path(tmp);(p/'check.c').write_text(code)
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-O2','-no-pie',
            '-Wall','-Wextra','-Werror','-Wno-pointer-to-int-cast','-Wno-int-to-pointer-cast',
            '-I'+str(MOD),str(p/'check.c'),'-o',str(p/'check')],check=True)
        subprocess.run([str(p/'check')],check=True)
    print('PASS: full-res diagnostic clean retirement, busy/context/memory refusal, cancellation, unsafe DMA/hook/guard retention')


if __name__=='__main__': main()

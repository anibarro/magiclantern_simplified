#!/usr/bin/env python3
"""Fault-inject the native job's real ownership/cleanup code; no hardware emulation."""
from pathlib import Path
import os
import subprocess
import tempfile
from livecomp_test_utils import function

ROOT = Path(__file__).resolve().parents[1]
MOD = ROOT/'modules/livecomp'


def main():
    native = (MOD/'native-max.c').read_text()
    # Keep the actual job/plane code, replacing only hardware boundaries.
    native = native[native.index('#define NATIVE_STATE'):]
    native = native.replace('#define NATIVE_STATE 0x54fc8u', '#define NATIVE_STATE ((uintptr_t)state_data)')
    native = native.replace(function(native, 'rom_valid('),
                            'static int rom_valid(void) { return camera_ok && rom_ok; }\n')
    native = native.replace(function(native, 'clean_table('),
                            'static void clean_table(uint32_t *r,uint32_t *s,uint32_t p) {(void)r;(void)s;(void)p;}\n')
    native = native.replace(native[native.index('int lc_native_resources('):],
        'int lc_native_resources(struct lc_resource_snapshot *s) { static const unsigned ids[10]={5,0x1000c,0x1000d,0x50002,0x50003,0x50006,0x80000,0x20001,0x30000,0x30001}; s->count=10;memcpy(s->ids,ids,sizeof(ids));if(bad_snapshot)s->ids[9]++;return 1; }\n')
    calls = {
        '((void (*)(void *))0xff429d28)': 'mock_complete_a',
        '((void (*)(void *))0xff429d00)': 'mock_complete_b',
        '((void (*)(void *))0xff429d50)': 'mock_complete_out',
        '((unsigned (*)(uint32_t *,unsigned))0xff2c5848)': 'mock_create_resource',
        '((unsigned (*)(unsigned))0xff2c5a60)': 'mock_delete_resource',
        '((unsigned (*)(unsigned))0xff2c6228)': 'mock_lock',
        '((unsigned (*)(const char *,unsigned))0x1e5f8)': 'mock_event',
        '((unsigned (*)(uint32_t *))0xff429e68)': 'mock_setup',
        '((void (*)(unsigned,unsigned))0x1c914)': 'mock_start',
        '((void (*)(unsigned))0xff2c19ac)': 'mock_engio',
        '((unsigned (*)(unsigned,unsigned,unsigned))0x1e7d4)': 'mock_wait',
        '((void (*)(unsigned))0x1cb3c)': 'mock_unregister',
        '((unsigned (*)(unsigned))0x1e644)': 'mock_delete',
        '((unsigned (*)(unsigned))0xff2c5eb4)': 'mock_unlock',
        '((void (*)(unsigned))0x1ca60)': 'mock_abort',
    }
    for old, new in calls.items():
        assert old in native
        native = native.replace(old, new)
    prefix = r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include "native-max.h"
#include "fullres-layout.h"
static unsigned state_data[256],old_geometry[16];
static int camera_ok=1,rom_ok=1;
static unsigned fail_lock,fail_event,fail_setup,fail_wait,fail_delete,fail_unlock;
static unsigned bad_snapshot,change_context;
static unsigned fail_resource,fail_resource_delete,resources_deleted;
static void (*callbacks[3])(void *);
static unsigned forwarded[3],trace_reads,addresses[3],waited,displaced,pre_busy;
static unsigned busy_channel,missing_table,locks,unlocks,setups,stops,deletes;
#define EDMAC_DIR_WRITE 1
static unsigned conn_reg[64],foreign_ch,foreign_armed,fail_restore,out_stuck,abort_ignored,aborts,conn_during_wait;
static unsigned edmac_get_connection(unsigned ch,unsigned dir) { assert(dir==EDMAC_DIR_WRITE && ch<64);return conn_reg[ch]; }
static void ConnectWriteEDmac(unsigned ch,unsigned s) {
    assert(ch<64 && ch!=3);if (fail_restore && s==1) return;conn_reg[ch]=s;
}
static void mock_abort(unsigned ch) { assert(ch==3);aborts++; }
static int is_camera(const char *m,const char *f) { (void)m;(void)f;return camera_ok; }
static unsigned cli(void) { return 0; }
static void sei(unsigned x) { (void)x; }
static unsigned get_us_clock(void) { static unsigned t;return t+=100; }
static unsigned patch_hook_function(unsigned a,unsigned b,void *c,const char *d)
{ (void)a;(void)b;(void)c;(void)d;return 0; }
static unsigned unpatch_memory(unsigned x) { (void)x;return 0; }
static unsigned edmac_get_state(unsigned x) {
    if (x==40) return waited ? busy_channel:pre_busy;
    if (x==3) return waited && out_stuck && (!aborts || abort_ignored);
    return foreign_ch && x==foreign_ch ? foreign_armed:0;
}
static unsigned channel_slot(unsigned ch) {
    assert(ch==40 || ch==41 || ch==3);return ch==40 ? 0:ch==41 ? 1:2;
}
static void RegisterEDmacCompleteCBR(unsigned ch,void (*cb)(void *),void *ctx)
{ assert(ctx==NULL);callbacks[channel_slot(ch)]=cb; }
static void mock_complete_a(void *ctx) { assert(ctx==NULL);forwarded[0]++; }
static void mock_complete_b(void *ctx) { assert(ctx==NULL);forwarded[1]++; }
static void mock_complete_out(void *ctx) { assert(ctx==NULL);forwarded[2]++; }
static unsigned edmac_get_flags(unsigned ch) { trace_reads++;return 100+channel_slot(ch); }
static unsigned edmac_get_address(unsigned ch) { if (foreign_ch && ch==foreign_ch) return 0x0d119c00;return waited && displaced && ch==3 ? 0x0d2dec00:addresses[channel_slot(ch)]; }
static unsigned edmac_get_pointer(unsigned ch) {
    if (foreign_ch && ch==foreign_ch) return 0x0d119c00;
    if (ch!=40 && ch!=41 && ch!=3) return 0;
    return addresses[channel_slot(ch)]+(waited ? 128:0); }
static unsigned mock_create_resource(uint32_t *ids,unsigned count) {
    assert(count==10 && ids[0]==3 && ids[1]==0x1000c && ids[9]==0x30001);
    return fail_resource ? 0:789;
}
static unsigned mock_delete_resource(unsigned x) {
    assert(x==789);resources_deleted++;return fail_resource_delete;
}
static unsigned mock_lock(unsigned x) { assert(x==789);locks++;if(change_context)state_data[10]=4;return fail_lock; }
static unsigned mock_event(const char *s,unsigned x) { (void)s;(void)x;return fail_event ? 0:456; }
static unsigned mock_setup(uint32_t *job);
static void mock_start(unsigned a,unsigned b) { assert(a==3 || a==40 || a==41);(void)b; }
static void mock_engio(unsigned x) { (void)x; }
static unsigned mock_wait(unsigned a,unsigned b,unsigned c)
{ assert(a==456 && b==7 && c==5000);
  conn_during_wait=foreign_ch ? conn_reg[foreign_ch]:0;
  waited=1;callbacks[0](NULL);callbacks[1](NULL);if(!fail_wait)callbacks[2](NULL);
  return fail_wait; }
static void mock_unregister(unsigned x) { callbacks[channel_slot(x)]=NULL;stops++; }
static unsigned mock_delete(unsigned x) { assert(x==456);deletes++;return fail_delete; }
static unsigned mock_unlock(unsigned x) { assert(x==789);unlocks++;return fail_unlock; }
'''
    suffix = r'''
static unsigned mock_setup(uint32_t *job) {
    assert(job[2]==10 && job[6]==0x8888dddd && job[9]==job[0]);
    assert(state_data[10]==3 && state_data[1]==123);
    waited=0;addresses[0]=(job[0]&~0x40000000u)-9240;addresses[1]=(job[1]&~0x40000000u)-9240;addresses[2]=job[9]&~0x40000000u;
    conn_reg[3]=1;                           /* FF429E68: ConnectWriteEDmac(out,1) */
    setups++;table_calls=missing_table ? 0:1;return fail_setup;
}
static void reset(void) {
    memset(callbacks,0,sizeof(callbacks));memset(forwarded,0,sizeof(forwarded));trace_reads=waited=displaced=pre_busy=0;
    memset(state_data,0,sizeof(state_data));
    state_data[1]=123;state_data[4]=1;state_data[5]=(unsigned)(uintptr_t)old_geometry;
    state_data[6]=40;state_data[7]=2;state_data[8]=41;state_data[9]=4;state_data[10]=5;
    memset(old_geometry,0,sizeof(old_geometry));
    installed=running=quarantined=0;private_geometry=0;
    camera_ok=rom_ok=1;fail_lock=fail_event=fail_setup=fail_wait=fail_delete=fail_unlock=0;
    bad_snapshot=change_context=fail_resource=fail_resource_delete=resources_deleted=0;
    busy_channel=missing_table=locks=unlocks=setups=stops=deletes=0;
    memset(conn_reg,0,sizeof(conn_reg));foreign_ch=foreign_armed=fail_restore=out_stuck=abort_ignored=aborts=conn_during_wait=0;
    lc_native_set_divert(1);
    assert(lc_native_engine_prepare((void *)0x41000000)==0);
}
int main(void) {
    void *mem=mmap((void *)0x41000000,4096,PROT_READ|PROT_WRITE,
                  MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(mem==(void *)0x41000000);
    struct lc_native_plane a={0x42000000,LC_FR_STORAGE,
        0x42000000+lc_fr_pixel_offset(0),LC_FR_WIDTH,LC_FR_BAND_ROWS,LC_FR_PITCH};
    struct lc_native_plane b=a;b.storage+=0x02000000;b.pixels+=0x02000000;
    struct lc_native_engine_result r;
    assert(LC_FR_BAND_ROWS*2==LC_FR_HEIGHT && LC_FR_BAND_ROWS<=1930);
    assert(LC_FR_ALLOCATION<0x1f24000-0x1000); /* standard EOS M SRM allowance */
    reset();assert(plane_valid(&a,1));
    a.pixels+=LC_FR_BAND_ROWS*LC_FR_PITCH;assert(plane_valid(&a,1));
    a.pixels-=LC_FR_BAND_ROWS*LC_FR_PITCH;
    assert(!lc_native_engine_max(&a,&b,&a,&r));
    assert(r.resume_safe && r.context_restored && locks==1 && unlocks==1 && setups==1);
    assert(stops==3 && deletes==1 && !state_data[2]);
    assert(trace_reads==6 && r.completed[0]==1 && r.completed[1]==1 && r.completed[2]==1);
    assert(forwarded[0]==1 && forwarded[1]==1 && forwarded[2]==1);
    assert(r.before_start[1]==100 && r.after_wait[11]==addresses[2]+128 && !callbacks[0] && !callbacks[2]);
    assert(state_data[10]==5 && state_data[1]==123 && resources_deleted==1 && !r.resource_delete);
    assert(state_data[5]==(unsigned)(uintptr_t)old_geometry && !running);
    assert(!lc_native_engine_max(&a,&b,&a,&r)); /* second band/session uses restored context */
    assert(state_data[10]==5 && resources_deleted==2 && unlocks==2);
    assert(!lc_native_engine_close());
    reset();bad_snapshot=1;assert(lc_native_engine_max(&a,&b,&a,&r)==LC_NATIVE_CONTEXT);
    assert(!locks && !setups);
    reset();change_context=1;assert(lc_native_engine_max(&a,&b,&a,&r)==LC_NATIVE_CONTEXT);
    assert(!setups && unlocks==1 && resources_deleted==1 && state_data[10]==4);
    reset();camera_ok=0;assert(lc_native_engine_max(&a,&b,&a,&r)==LC_NATIVE_ROM && !locks);
    reset();state_data[6]=99;assert(lc_native_engine_max(&a,&b,&a,&r)==LC_NATIVE_CONTEXT && !locks);
    reset();fail_lock=13;assert(lc_native_engine_max(&a,&b,&a,&r)==LC_NATIVE_BUSY);
    assert(r.resume_safe && !setups && !unlocks && !stops && resources_deleted==1 && !trace_reads);
    assert(state_data[10]==5);
    reset();fail_resource=1;assert(lc_native_engine_max(&a,&b,&a,&r)==LC_NATIVE_RESOURCE);
    assert(!locks && !resources_deleted && r.resume_safe);
    reset();fail_resource_delete=1;assert(lc_native_engine_max(&a,&b,&a,&r)&LC_NATIVE_RESOURCE);
    assert(!r.resume_safe && quarantined && unlocks==1);
    reset();fail_event=1;assert(lc_native_engine_max(&a,&b,&a,&r)==LC_NATIVE_EVENT);
    assert(r.resume_safe && r.context_restored && unlocks==1 && !setups && !stops);
    reset();fail_setup=1;assert(lc_native_engine_max(&a,&b,&a,&r)&LC_NATIVE_SETUP);
    assert(r.resume_safe && stops==3 && deletes==1 && unlocks==1);
    reset();fail_wait=9;assert(lc_native_engine_max(&a,&b,&a,&r)&LC_NATIVE_WAIT);
    assert(r.resume_safe && stops==3 && deletes==1 && unlocks==1);
    assert(r.completed[0]==1 && r.completed[1]==1 && r.completed[2]==0 && trace_reads==6);
    reset();missing_table=1;assert(lc_native_engine_max(&a,&b,&a,&r)&LC_NATIVE_TABLE);
    assert(r.resume_safe && r.context_restored);
    reset();busy_channel=1;assert(lc_native_engine_max(&a,&b,&a,&r)&LC_NATIVE_STOP);
    assert(!r.resume_safe && quarantined && !deletes && !unlocks && !resources_deleted);
    assert(lc_native_engine_close()==LC_NATIVE_BUSY);
    reset();displaced=1;assert(lc_native_engine_max(&a,&b,&a,&r)&LC_NATIVE_OWNERSHIP);
    assert(!r.resume_safe && quarantined && !stops && !deletes && !unlocks && !resources_deleted);
    assert(r.after_wait[10]==0x0d2dec00 && lc_native_engine_close()==LC_NATIVE_BUSY);
    reset();pre_busy=1;assert(lc_native_engine_max(&a,&b,&a,&r)&LC_NATIVE_OWNERSHIP);
    assert(!waited && !setups && !stops && !deletes && !unlocks && !r.resume_safe);
    reset();fail_delete=1;assert(lc_native_engine_max(&a,&b,&a,&r)&LC_NATIVE_DELETE);
    assert(!r.resume_safe && quarantined && !unlocks);
    reset();fail_unlock=1;assert(lc_native_engine_max(&a,&b,&a,&r)&LC_NATIVE_UNLOCK);
    assert(!r.resume_safe && quarantined);
    reset();struct lc_native_plane bad=a;bad.pixels=bad.storage;
    assert(lc_native_engine_max(&bad,&b,&bad,&r)==LC_NATIVE_ARGUMENT && !locks);
    bad=a;bad.height=1932;assert(!plane_valid(&bad,1));
    bad=a;bad.pixels+=14;assert(!plane_valid(&bad,1)); /* word alignment */
    bad=a;bad.bytes=LC_FR_PITCH*4;assert(!plane_valid(&bad,1));
    /* LCM23 router: register map from RAM 0x1C6A8. */
    assert(lc_native_write_route_reg(3)==0xC0F0500C && lc_native_write_route_reg(16)==0xC0F0501C);
    assert(lc_native_write_route_reg(0x12)==0xC0F05204 && lc_native_write_route_reg(0x20)==0xC0F05218);
    { uint32_t c[16]={0};c[1]=1;c[5]=2;c[2]=3;assert(lc_native_free_source(c)==4); }
    reset();assert(!lc_native_engine_max(&a,&b,&a,&r));
    assert(r.out_conn==1 && !r.listeners && !r.diverted && !r.aborted);
    assert(r.all_ptr0[3]==addresses[2] && r.all_ptr1[3]==addresses[2]+128); /* LCM27 snapshot, index 3 = ch3 */
    /* Live View channel 2 armed on source 1: moved away for the job only. */
    reset();foreign_ch=2;conn_reg[2]=1;foreign_armed=1;conn_reg[5]=2;
    assert(!lc_native_engine_max(&a,&b,&a,&r));
    assert(r.listeners==1 && r.lst_ch[0]==2 && r.lst_state[0]==1 && r.diverted==1 && r.divert_source==3);
    assert(conn_during_wait==3 && conn_reg[2]==1 && r.route_restored && r.resume_safe);
    reset();foreign_ch=2;conn_reg[2]=1;lc_native_set_divert(0);
    assert(!lc_native_engine_max(&a,&b,&a,&r) && r.listeners==1 && !r.diverted && conn_during_wait==1);
    reset();foreign_ch=0x11;conn_reg[0x11]=1;fail_restore=1;
    assert(lc_native_engine_max(&a,&b,&a,&r)&LC_NATIVE_ROUTE);
    assert(!r.resume_safe && quarantined && !r.route_restored);
    /* Output stuck at our own address (LCM22): aborted, session continues. */
    reset();fail_wait=9;out_stuck=1;r.aborted=99;
    assert(lc_native_engine_max(&a,&b,&a,&r)==LC_NATIVE_WAIT);
    assert(r.aborted==1 && aborts==1 && r.resume_safe && deletes==1 && unlocks==1 && !quarantined);
    reset();fail_wait=9;out_stuck=1;abort_ignored=1;
    assert(lc_native_engine_max(&a,&b,&a,&r)&LC_NATIVE_STOP);
    assert(r.aborted==3 && !r.resume_safe && quarantined && !deletes && !unlocks);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp)
        (p/'check.c').write_text(prefix+native+suffix)
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-O2','-no-pie',
                        '-Wall','-Wextra','-Werror','-Wno-unused-function',
                        '-Wno-pointer-to-int-cast','-Wno-int-to-pointer-cast',
                        '-I'+str(MOD),str(p/'check.c'),'-o',str(p/'check')],check=True)
        subprocess.run([str(p/'check')],check=True)
    print('PASS: native job bounds, write-source router divert/restore, stuck-output abort, nonblocking busy, event/setup/wait failures, restore/unlock, DMA quarantine; hardware accuracy untested')


if __name__ == '__main__':
    main()

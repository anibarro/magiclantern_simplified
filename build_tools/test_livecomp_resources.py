#!/usr/bin/env python3
"""Validate the bounded read-only resource walker against synthetic registries."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    code=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "resource-snapshot.h"
static uint32_t memory[1024],reads;
static int read_word(void *ctx,uint32_t address,uint32_t *out) {
    assert(!ctx);reads++;
    if (address%4 || address>=sizeof(memory)) return 0;
    *out=memory[address/4];return 1;
}
#define W(a,v) (memory[(a)/4]=(v))
static void reset(void) {
    memset(memory,0,sizeof(memory));reads=0;
    W(100,0x1234);W(120,200);W(124,2); /* lock, IDs pointer/count */
    W(304,400); /* manager heads */
    W(200,0x00000005);W(204,0x0001000a);
    W(404,504);W(412,604); /* list heads -> embedded links */
    W(500,0xabcd);W(508,400);W(512,5);W(516,1);
    W(600,0xabcd);W(608,408);W(612,0x1000a);W(616,0);
}
static int snapshot(struct lc_resource_snapshot *s) {
    return lc_resource_read(read_word,0,100,300,0xabcd,0x1234,s);
}
int main(void) {
    struct lc_resource_snapshot s={0};uint32_t before[1024];
    reset();memcpy(before,memory,sizeof(memory));
    assert(snapshot(&s) && s.count==2 && s.ids[0]==5 && s.ids[1]==0x1000a && s.busy==1);
    assert(!memcmp(memory,before,sizeof(memory))); /* no writes to registry */
    W(516,0);W(616,1);assert(snapshot(&s) && s.busy==2);
    W(616,0);assert(snapshot(&s) && !s.busy);
    reset();W(512,6);W(508,704); /* walk past a different resource */
    W(700,0xabcd);W(708,400);W(712,5);W(716,1);
    assert(snapshot(&s) && s.busy==1);
    reset();W(512,6);W(508,504);reads=0;
    assert(!snapshot(&s) && reads<300); /* cycle bounded */
    reset();W(124,17);assert(!snapshot(&s));
    reset();W(124,0);assert(!snapshot(&s));
    reset();W(100,0);assert(!snapshot(&s));
    reset();W(500,0);assert(!snapshot(&s));
    reset();W(200,0x100000);assert(!snapshot(&s));
    reset();W(120,0xfffffff0);assert(!snapshot(&s));
    reset();W(404,0xffffffff);assert(!snapshot(&s));
    reset();W(512,6);W(508,400);assert(!snapshot(&s)); /* requested ID absent */
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        p=Path(tmp);(p/'check.c').write_text(code)
        subprocess.run([os.environ.get('CC','cc'),'-std=c99','-O2','-Wall','-Wextra','-Werror',
            '-I'+str(ROOT/'modules/livecomp'),str(p/'check.c'),'-o',str(p/'check')],check=True)
        subprocess.run([str(p/'check')],check=True)
    print('PASS: resource IDs/busy masks, read-only access, bounded cycles and malformed registry rejection')


if __name__=='__main__': main()

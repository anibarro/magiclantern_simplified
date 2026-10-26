#!/usr/bin/env python3
"""Run the camera settings predicate with simulated properties, not hardware."""
from pathlib import Path
import os
import subprocess
import tempfile
from livecomp_test_utils import function
from analyze_livecomp_lv import analyze

ROOT = Path(__file__).resolve().parents[1]


def main():
    camera = (ROOT / 'modules/livecomp/livecomp-lv.c').read_text()
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "lv-ring.h"
static void livecomp_fullres_vsync(void) {}
static void livecomp_frlv_vsync(void) {}
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
#define GUARD 64u
#define LV_FRAME_CAP 120u
#define LV_PREVIEW_INTERVAL_MS 1000u
#define CBR_RET_CONTINUE 0
#define CBR_RET_STOP 1
enum { E_SETTINGS=1,E_DMA=4,E_GUARD=8,E_QUEUE=16,E_SHUTDOWN=256 };
struct lv_frame_log { uint32_t sequence,slot,start_ms,done_ms,merge_us,dma_state,dma_pointer; };
'''
    code += camera[camera.index('static const char *const setting_names[]'):
                   camera.index('struct lv_session {')]
    code += r'''
struct raw_info { int bits_per_pixel,width,height,pitch,frame_size;
    struct { int x1,y1,x2,y2; } active_area; };
static struct raw_info raw_info;
static struct {
    int job_state,raw_iso,raw_aperture,raw_shutter,wb_mode,kelvin,WBGain_R,WBGain_B,wbs_gm,wbs_ba;
} lens_info;
static int lv,LV_PAUSED,movie,lv_dispsize,RECORDING,raw_write_chan,raw_valid;
static int frame_iso,fps,tvr,hardware_reads;
static unsigned display_setup,stop_requested;
static int lv_disp_mode,draw_setting=3,info_presses,display_ready;
#define MODULE_KEY_INFO 26
static struct { int raw_channel; int gui_idle; } livecomp_camera={18,0};
static int get_global_draw_setting(void) { return draw_setting; }
static int get_global_draw(void) { return display_ready; }
static void module_send_keypress(int button) {
    assert(button==MODULE_KEY_INFO && display_setup);
    if (++info_presses==2) { lv_disp_mode=0;display_ready=1; }
}
static void msleep(unsigned delay) { assert(delay==200); }
static int ml_shutdown_requested,apply_override=1,override_calls;
static void fps_update_timers_from_evfstate(void) {
    override_calls++;
    if (apply_override) { fps=3000;tvr=3030; }
}
static int is_movie_mode(void) { return movie; }
static int raw_lv_settings_still_valid(void) { return raw_valid; }
static int get_frame_iso(void) { hardware_reads++;return frame_iso; }
static int fps_get_current_x1000(void) { hardware_reads++;return fps; }
static int get_current_shutter_reciprocal_x1000(void) { hardware_reads++;return tvr; }
static unsigned cli(void) { return 0; }
static void sei(unsigned irq) { (void)irq; }
static unsigned now=42;
static unsigned get_ms_clock(void) { return now; }
struct lv_session {
    struct raw_info info;
    int iso,aperture,shutter,wb,kelvin,gain_r,gain_b,wb_gm,wb_ba,frame_iso,fps,tvr;
    struct lc_ring ring;
    struct lv_frame_log frames[LV_FRAME_CAP];
    unsigned vsyncs,last_vsync,bytes,merged,error,quarantined,stop_kind,stop_issued,limit,preview_interval;
    unsigned timing_syncs,preview_rendered,preview_frames,preview_calls,preview_last_ms,preview_snapshot_ms;
    int timing_before_fps,timing_before_tvr,timing_after_fps,timing_after_tvr;
    void *preview_destination;
    uint8_t *input[LC_LV_SLOTS],*accumulator,*preview;
    struct lv_settings_failure settings_failure;
};
static struct lv_session *session;
static unsigned capture_reading;
static unsigned armed,preview_reading,preview_writing,preview_ready,preview_enabled;
static unsigned dma_state,dma_pointer,redirects,renders;
static uint8_t *redirected;
static void stop(unsigned kind) { stop_requested=1;session->stop_kind=kind;session->stop_issued=session->ring.issued; }
static unsigned physical(const void *p) { return (unsigned)(uintptr_t)p & ~0x40000000u; }
static unsigned edmac_get_state(unsigned channel) { assert(channel==18);return dma_state; }
static unsigned edmac_get_pointer(unsigned channel) { assert(channel==18);return dma_pointer; }
static void raw_lv_redirect_edmac(void *p) {
    assert(fps==3000 && tvr==3030);redirects++;redirected=p;
}
struct display_filter_buffers { void *src_buf,*dst_buf; };
static void sync_caches(void) {}
#define RAW_PREVIEW_COLOR_HALFRES 1
static void raw_set_preview_rect(int x,int y,int w,int h,int bars) {
    assert(x==72 && y==28 && w==1736 && h==697 && bars==1);
}
static void raw_preview_fast_ex(void *raw,void *dst,int y1,int y2,int quality) {
    assert(raw==session->preview && y1==-1 && y2==-1 && quality==RAW_PREVIEW_COLOR_HALFRES);
    memcpy(dst,raw,session->bytes);renders++;
}
'''
    code += function(camera, 'settings_valid(')
    code += function(camera, 'prepare_display(')
    code += function(camera, 'sync_capture_timing(')
    code += function(camera, 'guards_valid(')
    code += function(camera, 'tail_written(')
    code += function(camera, 'update_preview(')
    code += function(camera, 'capture_vsync(')
    code += function(camera.replace('unsigned livecomp_lv_vsync(', 'static unsigned livecomp_lv_vsync('),
                     'livecomp_lv_vsync(')
    code += function(camera.replace('unsigned livecomp_lv_preview(unsigned context)',
                                   'static unsigned livecomp_lv_preview(uintptr_t context)'),
                     'livecomp_lv_preview(')
    code += r'''
int main(void) {
    struct lv_session s={0};
    lv=movie=lv_dispsize=raw_valid=1;raw_write_chan=18;
    raw_info=(struct raw_info){14,1808,727,3164,2300228,{72,28,1808,725}};s.info=raw_info;
    s.iso=lens_info.raw_iso=72;s.aperture=lens_info.raw_aperture=24;
    s.shutter=lens_info.raw_shutter=96;s.wb=lens_info.wb_mode=9;
    s.kelvin=lens_info.kelvin=6500;s.gain_r=lens_info.WBGain_R=1234;
    s.gain_b=lens_info.WBGain_B=2345;s.frame_iso=frame_iso=72;
    s.fps=fps=3000;s.tvr=tvr=3030;s.vsyncs=1;
    assert(settings_valid(&s,1) && !s.settings_failure.phase);
    int *inputs[]={&lv,&LV_PAUSED,&movie,&lv_dispsize,&RECORDING,&lens_info.job_state,
        &raw_write_chan,&raw_valid,&raw_info.bits_per_pixel,&raw_info.width,&raw_info.height,
        &raw_info.pitch,&raw_info.frame_size,&lens_info.raw_iso,&lens_info.raw_aperture,
        &lens_info.raw_shutter,&lens_info.wb_mode,&lens_info.kelvin,&lens_info.WBGain_R,
        &lens_info.WBGain_B,&lens_info.wbs_gm,&lens_info.wbs_ba,&frame_iso,&fps,&tvr};
    assert(COUNT(inputs)==COUNT(setting_names));
    for (unsigned i=0;i<COUNT(inputs);i++) {
        int saved=*inputs[i];*inputs[i]=saved ? 0:1;
        memset(&s.settings_failure,0,sizeof(s.settings_failure));
        assert(settings_valid(&s,0)==(i>=23) && !s.settings_failure.phase);
        assert(!settings_valid(&s,2));
        assert(s.settings_failure.phase==2 && (s.settings_failure.mask & (1u<<i)));
        assert(s.settings_failure.expected[i]==saved && s.settings_failure.observed[i]==*inputs[i]);
        assert(s.settings_failure.ms==42 && s.settings_failure.vsync==1 && !s.settings_failure.issued);
        struct lv_settings_failure first=s.settings_failure;
        *inputs[i]=saved;
        assert(settings_valid(&s,1));
        int old_fps=fps;fps=1234;assert(!settings_valid(&s,2));fps=old_fps;
        assert(!memcmp(&first,&s.settings_failure,sizeof(first))); /* first failure retained */
    }
    memset(&s.settings_failure,0,sizeof(s.settings_failure));
    fps++;tvr++;
    assert(!settings_valid(&s,2) && s.settings_failure.phase==2);
    assert(s.settings_failure.mask==((1u<<23)|(1u<<24))); /* no tolerance silently added */
    fps--;tvr--;hardware_reads=0;lv=0;
    assert(!settings_valid(&s,0) && !hardware_reads);
    lv=1;raw_valid=0;
    assert(!settings_valid(&s,0) && !hardware_reads);
    raw_valid=1;
    memset(&s.settings_failure,0,sizeof(s.settings_failure));
    /* Exercise the actual callbacks with the camera's measured transient values.
     * Fake DMA completes only after the callback, as in core raw_lv_vsync. */
    uint8_t inputs_mem[LC_LV_SLOTS][64+56+64],acc[56],preview[56],lcd[56];
    memset(inputs_mem,0xa5,sizeof(inputs_mem));
    s.bytes=56;s.accumulator=acc;s.preview=preview;s.vsyncs=0;
    for (unsigned i=0;i<LC_LV_SLOTS;i++) s.input[i]=inputs_mem[i]+64;
    session=&s;s.limit=120;s.preview_interval=1000;lc_ring_init(&s.ring);armed=1;
    fps=29973;tvr=33336;now=1000;
    assert(settings_valid(&s,1)); /* pre-arm worker must not compare transient timers */
    livecomp_lv_vsync(0);
    assert(armed && !s.error && s.ring.issued==1 && redirects==1 && override_calls==1);
    assert(s.timing_before_fps==29973 && s.timing_before_tvr==33336);
    assert(s.timing_after_fps==3000 && s.timing_after_tvr==3030);
    memset(redirected,0x12,s.bytes);dma_pointer=physical(redirected)+s.bytes;
    fps=29973;tvr=33336;now+=333;
    livecomp_lv_vsync(0);
    assert(armed && !s.error && s.ring.issued==2 && redirects==2 && override_calls==2);
    int slot=lc_ring_take(&s.ring);assert(slot>=0);
    memcpy(acc,s.input[slot],s.bytes);s.merged++;assert(lc_ring_release(&s.ring,slot));
    update_preview(&s,0);preview_enabled=1;
    /* Preview reads during Canon's transient must display the merged snapshot. */
    fps=29973;tvr=33336;
    struct display_filter_buffers buffers={inputs_mem,lcd};
    assert(livecomp_lv_preview(0));
    livecomp_lv_preview((uintptr_t)&buffers);
    assert(renders==1 && s.preview_rendered==1 && !memcmp(lcd,acc,s.bytes));
    livecomp_lv_preview((uintptr_t)&buffers);assert(renders==1); /* no redundant render */
    assert(override_calls==2); /* preview never changes sensor timers */
    memset(redirected,0x34,s.bytes);dma_pointer=physical(redirected)+s.bytes;
    stop(1);now+=333;livecomp_lv_vsync(0);
    assert(!armed && !s.error && s.ring.issued==2 && override_calls==2);
    slot=lc_ring_take(&s.ring);assert(slot>=0 && lc_ring_release(&s.ring,slot));
    assert(lc_ring_take(&s.ring)==-1 && s.ring.pending==-1);
    s.merged=2;memset(acc,0x56,sizeof(acc));update_preview(&s,0);
    assert(preview_ready==1 && s.preview_frames==1); /* limit LCD work, not capture */
    update_preview(&s,1);assert(preview_ready==2 && !memcmp(preview,acc,sizeof(acc)));
    now+=1000;s.merged=3;update_preview(&s,0);assert(preview_ready==3);
    s.preview_interval=4000;now+=2000;s.merged=4;update_preview(&s,0);
    assert(preview_ready==3);
    now+=2000;update_preview(&s,0);assert(preview_ready==4);
    /* The selected cap fences the pending input before ending admission. */
    s.limit=s.ring.issued;armed=1;stop_requested=0;fps=3000;tvr=3030;
    livecomp_lv_vsync(0);assert(!armed && s.stop_kind==2 && s.ring.pending==-1);
    /* If the real override cannot restore the requested values, reject input. */
    armed=1;stop_requested=0;apply_override=0;fps=29973;tvr=33336;s.limit=120;
    unsigned admitted=s.ring.issued;livecomp_lv_vsync(0);
    assert(!armed && (s.error&E_SETTINGS) && s.ring.issued==admitted);
    assert(s.settings_failure.mask==((1u<<23)|(1u<<24)));
    /* An invalid mode never triggers timing register writes. */
    int previous=override_calls;ml_shutdown_requested=1;sync_capture_timing(&s);
    assert(override_calls==previous);
    ml_shutdown_requested=stop_requested=0;lv_disp_mode=1;
    assert(prepare_display() && info_presses==2 && !display_setup);
    draw_setting=0;assert(!prepare_display() && !display_setup);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        (base/'check.c').write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra',
                        '-Werror', '-I'+str(ROOT/'modules/livecomp'),
                        str(base/'check.c'), '-o', str(base/'check')], check=True)
        subprocess.run([str(base/'check')], check=True)
        # A settings failure must remain a failed camera gate, with readable differences.
        log = base/'LVCP0000.LOG'
        log.write_text('''LCM3a synthetic failed-session fixture
Status: LV settings changed; see LOG, reboot
Error_quarantined_saved: 1,0,0
Width_height_pitch_bytes: 1808,727,3164,2300228
FPS_x1000_shutter_reciprocal_x1000: 3000,3030
Issued_merged_vsyncs: 0,0,1
Stop_kind_issued: 4,0
Preview_snapshots_calls: 0,0
Settings_failure_phase_mask_ms_vsync_issued: 2,16777216,42,1,0
Setting_fps_expected_observed: 3000,3000
Setting_tvr_expected_observed: 3030,3031
''')
        result = analyze(log)
        assert not result['all_checks_pass'] and not result['checks']['cadence']
        assert result['settings_failure']['mismatches'] == {'tvr': [3030, 3031]}
        assert result['settings_failure']['phase'] == 2
        log.write_text(log.read_text().replace('2,16777216,42,1,0','2,u,u,u,u'))
        result = analyze(log)
        assert result['settings_failure']['incomplete_metadata']
        assert result['settings_failure']['mask'] is None
        assert result['settings_failure']['mismatches'] == {'tvr': [3030,3031]}
    print('PASS: 25 settings gates, measured FPS transient, capture/Stop callbacks, preview snapshot and redraw, exact exposure rejection and legacy log analysis')


if __name__ == '__main__':
    main()

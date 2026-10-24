#!/usr/bin/env python3
"""Read LCM3 LV capture evidence. Cadence checks do not measure sensor dead time."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics


def stats(values):
    return dict(min=min(values), mean=statistics.mean(values), max=max(values)) if values else None


def analyze(path):
    text = path.read_text()
    fields = dict(line.split(': ', 1) for line in text.splitlines() if ': ' in line)
    def numbers(name):
        return list(map(int, fields[name].split(',')))
    error, quarantine, saved = numbers('Error_quarantined_saved')
    width, height, pitch, size = numbers('Width_height_pitch_bytes')
    fps, tvr = numbers('FPS_x1000_shutter_reciprocal_x1000')
    issued, merged, vsyncs = numbers('Issued_merged_vsyncs')
    stop_kind, stop_issued = numbers('Stop_kind_issued')
    previews, calls = numbers('Preview_snapshots_calls')
    csv_path = path.with_suffix('.CSV')
    frames = []
    if csv_path.exists():
        with csv_path.open(newline='') as f:
            for row in csv.DictReader(f):
                frames.append({k: int(v, 16 if k == 'dma_pointer' else 10) for k, v in row.items()})
    intervals = [(b['start_ms']-a['start_ms']) & 0xffffffff for a,b in zip(frames,frames[1:])]
    period = 1e6/fps if fps>0 else None
    exposure = 1e6/tvr if tvr>0 else None
    dng = path.with_suffix('.DNG')
    header = b''
    if dng.exists():
        with dng.open('rb') as f:
            header = f.read(4)
    checks = dict(clean=error == quarantine == 0, saved=saved == 1,
                  all_admitted_merged=0 < issued == merged <= 120,
                  frame_record_count=len(frames) == issued,
                  sequence=[f['sequence'] for f in frames] == list(range(1, issued+1)),
                  dma_idle=bool(frames) and all(f['dma_state'] == 0 for f in frames),
                  all_processed=bool(frames) and all(f['merge_us'] > 0 for f in frames),
                  stop_bound=issued <= stop_issued if stop_kind in (1, 2) else False,
                  preview_ran=previews>0 and calls>0,
                  dng_header=header in (b'II*\0', b'MM\0*'),
                  dng_size=dng.exists() and dng.stat().st_size > size > 0,
                  cadence=bool(period and intervals and all(abs(dt-period) <= max(30,period*.2) for dt in intervals)))
    settings_failure = None
    if 'Settings_failure_phase_mask_ms_vsync_issued' in fields:
        # LCM3a used %u, unsupported by ML's formatter; preserve missing fields
        # as unknown while retaining the valid per-setting expected/observed data.
        metadata = fields['Settings_failure_phase_mask_ms_vsync_issued'].split(',')
        phase, mask, ms, vsync, count = [int(v) if v.lstrip('-').isdigit() else None for v in metadata]
        observations = {key.removeprefix('Setting_').removesuffix('_expected_observed'): numbers(key)
                        for key in fields if key.startswith('Setting_') and key.endswith('_expected_observed')}
        settings_failure = dict(phase=phase, mask=mask, ms=ms, vsync=vsync, issued=count,
                                incomplete_metadata=any(v is None for v in (phase,mask,ms,vsync,count)),
                                observations=observations,
                                mismatches={k: v for k, v in observations.items() if v[0] != v[1]})
    return dict(stem=path.stem, status=fields['Status'], error=error, quarantine=quarantine,
                settings_failure=settings_failure,
                timing_sync=numbers('Timing_syncs_before_fps_tvr_after_fps_tvr')
                            if 'Timing_syncs_before_fps_tvr_after_fps_tvr' in fields else None,
                save_stage=numbers('Save_stage_written_last_return_size')
                           if 'Save_stage_written_last_return_size' in fields else None,
                checks=checks, all_checks_pass=all(checks.values()), geometry=[width,height,pitch,size],
                issued=issued, merged=merged, vsyncs=vsyncs, fps_x1000=fps,
                reported_exposure_ms=exposure, reported_shutter_duty=exposure/period if period and exposure else None,
                intervals_ms=intervals, interval_summary_ms=stats(intervals),
                merge_summary_us=stats([f['merge_us'] for f in frames if f['merge_us']]), frames=frames,
                note='Frame callback intervals and software exposure estimates; zero sensor dead time and pixel correctness are not proved by these logs.')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('folder', type=Path)
    args = parser.parse_args()
    runs = [analyze(p) for p in sorted(args.folder.glob('LVCP*.LOG'))]
    report = dict(runs=runs, hashes={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in sorted(args.folder.glob('LVCP*')) if p.is_file()})
    (args.folder/'analysis.json').write_text(json.dumps(report, indent=2)+'\n')
    for run in runs:
        print(run['stem'], 'PASS' if run['all_checks_pass'] else 'FAIL',
              f"{run['merged']}/{run['issued']} merged; intervals {run['interval_summary_ms']}")
    if not runs or not all(r['all_checks_pass'] for r in runs):
        raise SystemExit(1)


if __name__ == '__main__':
    main()

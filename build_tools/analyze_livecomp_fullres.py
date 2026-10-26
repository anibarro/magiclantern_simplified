#!/usr/bin/env python3
"""Explain LCFR logs offline. Pass a log or a card directory (default P:/)."""
from pathlib import Path
import sys


def analyze(path):
    text = path.read_text(encoding='utf-8')
    values = {}
    for line in text.splitlines():
        if ': ' in line:
            key, value = line.split(': ', 1)
            values[key] = value

    def nums(key):
        return [int(v) for v in values.get(key, '').split(',') if v]

    print(f'{path}\n{text.splitlines()[0]}')
    print('Synthetic benchmark only: this is not a full-sensor photograph.')
    state = nums('Stage_ROM_allocated_prepared_error_unsafe')
    if len(state) == 6:
        print(f'Large buffers: {state[2]}/4. Error mask: {state[4]}.')
        if state[5]:
            print('REBOOT REQUIRED: ownership/cleanup was uncertain. Do not repeat in this session.')
    cpu = nums('CPU_us_checked_mismatches_source_ok_callbacks_max_gap')
    if len(cpu) == 6 and cpu[1]:
        print(f'CPU full-frame blend: {cpu[0]/1e6:.3f} s; mismatches={cpu[2]}, source intact={bool(cpu[3])}.')
        if cpu[0] and cpu[2] == 0 and cpu[3]:
            print(f'Merge-only ceiling: {1e6/cpu[0]:.2f} fps, BEFORE capture, LCD and storage costs.')
            print('CPU fallback is numerically verified; sensor readout and buffer scheduling still need implementation.')
    else:
        print('No completed CPU verification in this log.')
    mem = nums('Segmented_blocks_bytes')
    if len(mem) == 2:
        print(f'Additional segmented reservation: {mem[0]} blocks, {mem[1]/1048576:.1f} MiB, subsequently freed.')
        print('Enough for one synthetic frame.' if mem[1] >= 32413920 else 'Not enough for one synthetic frame in this camera state.')
    passed = []
    for case in range(6):
        key = f'Case{case}_rows_output_row_checked_mismatches_source_ok'
        result = nums(key)
        if len(result) != 5 or not result[2]:
            continue
        rows, output, checked, mismatches, source = result
        timing = nums(f'Band{case}_reason_lock_setup_wait_delete_unlock_us')
        ok = bool(timing and timing[0] == 0 and mismatches == 0 and source)
        layout = 'in-place' if output == 4 else 'separate output'
        print(f'Case {case}: {rows} rows, {layout}: {"PASS" if ok else "FAIL"}; mismatches={mismatches}, checked={checked}.')
        if ok:
            passed.append((rows, layout))
            print(f'  Native call: {timing[-1]/1000:.3f} ms (excludes verification and any scratch-buffer copy).')
    attempted = nums('Native_cases_attempted')
    indices = range(attempted[0]) if attempted else range(2)
    for case in indices:
        timing = nums(f'Band{case}_reason_lock_setup_wait_delete_unlock_us')
        if not timing or not timing[0]:
            continue
        print(f'Case/band {case} native failure mask {timing[0]}; lock={timing[1]}, setup={timing[2]}, wait={timing[3]}.')
        for channel in range(3):
            key = f'Band{case}_DMA{channel}_callbacks_before_state_flags_address_pointer_after_state_flags_address_pointer'
            raw = values.get(key)
            if not raw:
                continue
            parts = raw.split(',')
            callbacks = int(parts[0])
            before_state, before_flags, address, pointer, after_state, flags, after_address, after_pointer = [int(x, 16) for x in parts[1:]]
            print(f'  DMA {channel}: callbacks={callbacks}, state after wait={after_state}, pointer movement={after_pointer-pointer} bytes, programmed address {address:08x}->{after_address:08x}.')
    if passed:
        print('Next: use the verified layout/size to design tiled merging; do not assume larger bands work.')
    elif attempted:
        print('No native case verified. Prefer the measured CPU fallback if it passed; do not treat a timeout as a blend.')
    print('Full-sensor mode, continuous acquisition, LCD integration and its memory budget remain separate implementation gates.')


def main():
    path = Path(sys.argv[1] if len(sys.argv) > 1 else 'P:/')
    if path.is_dir():
        logs = list(path.glob('LCFR*.LOG'))
        if not logs:
            raise SystemExit(f'No LCFR logs in {path}. Pass a log file or the SD card directory.')
        path = max(logs, key=lambda p: p.stat().st_mtime_ns)
    if not path.is_file():
        raise SystemExit(f'Log not found: {path}. Pass a log file or the SD card directory.')
    analyze(path)


if __name__ == '__main__':
    main()

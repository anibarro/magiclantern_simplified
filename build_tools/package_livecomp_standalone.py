#!/usr/bin/env python3
"""Verify standard-core imports and package only the standalone module."""
from pathlib import Path
import hashlib
import json
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    mod=ROOT/'modules/livecomp'
    binary=mod/'build/livecomp.mo'
    out=ROOT/'platform/EOSM.202/build_livecomp'
    symbols=ROOT/'platform/EOSM.202/build/EOSM_202.sym'
    required=set((mod/'build/livecomp.dep').read_text().split())
    exports={line.split()[1] for line in symbols.read_text().splitlines() if line.strip()}
    assert required <= exports, sorted(required-exports)
    custom={'handle_module_keys_early','stateobj_eosm_commit_display_buffer'}
    assert not custom & (required | exports)
    # Compatibility proof must not accidentally use the former modified core.
    for name in ('gui-common.c','module.c','module.h','state-object.c','state-object.h'):
        path='src/'+name
        baseline=subprocess.check_output(['git','show','HEAD:'+path],cwd=ROOT)
        assert (ROOT/path).read_bytes().replace(b'\r\n',b'\n')==baseline.replace(b'\r\n',b'\n'), path
    header=subprocess.check_output(['arm-none-eabi-readelf','-h',str(binary)],text=True)
    assert 'ARM' in header and 'REL (Relocatable file)' in header
    assert b'LCM38 full-res composite' in binary.read_bytes()
    assert 'CBR_KEYPRESS_RAW_EARLY' not in (mod/'livecomp.c').read_text()
    assert 'xpan-composite' not in (mod/'Makefile').read_text()
    config=(mod/'build/effective-config.txt').read_text()
    assert '#define CONFIG_QEMU' not in config
    for flag in ('CONFIG_EOSM', 'CONFIG_DIGIC_V', 'CONFIG_DRYOS', 'CONFIG_3_2_SCREEN'):
        assert '#define '+flag+' ' not in config
    for p in list(mod.glob('*.c'))+list(mod.glob('*.h')):
        assert '#include \"../../platform/' not in p.read_text()
    archive=out/'livecomp-LCM38-module-only.zip'
    payload={'ML/modules/livecomp.mo':binary,'LIVE_COMPOSITE_README.txt':mod/'INSTALL.txt',
             'ANALYZE_FULLRES.py':ROOT/'build_tools/analyze_livecomp_fullres.py'}
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
        for name,p in payload.items(): z.write(p,name)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None and set(z.namelist())==set(payload)
        for name,p in payload.items(): assert z.read(name)==p.read_bytes()
    source_files=['Makefile','README.rst','INSTALL.txt','DEVELOPMENT.md','FULLRES.md','livecomp.c','livecomp-lv.c',
                  'livecomp-lv.h','livecomp-config.h','camera-profile.h','lv-ring.h','lv-dng.h','raw14-max.h',
                  'native-max.c','native-max.h','fullres-check.c','fullres-check.h','fullres-layout.h','resource-snapshot.h',
                  'fullres-lv.c','fullres-lv.h','fullres-regs.h','fullres-gamma.h','fullres-fix.h','fullres-hw.h','lv-pause-regs.h']
    with zipfile.ZipFile(out/'livecomp-LCM38-source.zip','w',zipfile.ZIP_DEFLATED) as z:
        for name in source_files: z.write(mod/name,'modules/livecomp/'+name)
    report={'build_id':'LCM38','camera':'EOSM.202','module_only':True,
            'custom_core_required':False,'other_modules_required':[],
            'stock_core_imports_resolved':len(required),'camera_tested':False,
            'preserved_capture_baseline':'LCM8, 76 frames passed on camera',
            'full_sensor_capture_enabled':'gate only (Task: Full-res readout)','native_check_camera_tested':False,
            'sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in (binary,symbols,archive)}}
    (out/'livecomp-LCM38-manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS: {len(required)} standard-core imports; no custom hooks or module dependencies; verified module-only ZIP')

if __name__=='__main__': main()

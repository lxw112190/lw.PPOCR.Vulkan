"""Compare the actual x64 DLL/ELF C exports and Python layouts with the v1 candidate."""
import argparse
import ctypes as C
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'examples/python'))
from lwvk import DeviceInfo,OcrConfig

def pe_exports(path):
    data=path.read_bytes()
    u16=lambda p:struct.unpack_from('<H',data,p)[0]
    u32=lambda p:struct.unpack_from('<I',data,p)[0]
    if data[:2]!=b'MZ':raise AssertionError('Not a PE library')
    pe=u32(0x3c)
    assert data[pe:pe+4]==b'PE\0\0' and u16(pe+4)==0x8664,'Expected x64 PE'
    optional=pe+24;assert u16(optional)==0x20b,'Expected PE32+'
    section_table=optional+u16(pe+20)
    sections=[struct.unpack_from('<8sIIII',data,section_table+40*i) for i in range(u16(pe+6))]
    def offset(rva):
        for _,virtual_size,base,raw_size,raw in sections:
            if base<=rva<base+max(virtual_size,raw_size):return raw+rva-base
        raise AssertionError(f'Unmapped PE export RVA {rva}')
    export=offset(u32(optional+112)); count=u32(export+24);names=offset(u32(export+32))
    result=[]
    for i in range(count):
        start=offset(u32(names+4*i)); end=data.index(0,start)
        result.append(data[start:end].decode('ascii'))
    return sorted(result)

def parse_elf_exports(output):
    # Ignore compiler C++ weak symbols, but catch BOTH native project prefixes.
    # Host geometry extraction uses lw_; accidentally exporting it is also ABI
    # pollution, even though it is absent from the public lwvk_ header.
    symbols=[]
    for line in output.splitlines():
        fields=line.split()
        if fields and fields[-1].startswith(('lwvk_', 'lw_')):
            symbols.append(fields[-1].split('@')[0])
    return sorted(symbols)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--library',type=Path,required=True);a=p.parse_args()
    baseline=json.loads((ROOT/'schemas/c-abi-v1.json').read_text())
    library=a.library.resolve()
    if library.suffix.lower()=='.dll':actual=pe_exports(library)
    else:
        output=subprocess.check_output(['nm','-D','--defined-only',str(library)],text=True)
        actual=parse_elf_exports(output)
    assert actual==baseline['symbols'],dict(expected=baseline['symbols'],actual=actual)
    for name,typ in [('lwvk_device_info',DeviceInfo),('lwvk_ocr_config',OcrConfig)]:
        wanted=baseline['structures'][name]
        assert C.sizeof(typ)==wanted['size'] and C.alignment(typ)==wanted['alignment'],name
        assert {key:getattr(typ,key).offset for key in wanted['offsets']}==wanted['offsets'],name
    print(f'PASS: {len(actual)} C exports / Python x64 sizes, alignment and offsets')

if __name__=='__main__':main()

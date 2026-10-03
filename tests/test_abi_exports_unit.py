"""Regression for the Linux CI private C-symbol leak, without an ELF toolchain."""
import json
from pathlib import Path
import unittest
from test_abi_exports import parse_elf_exports


class ElfExportParserTests(unittest.TestCase):
    def test_candidate_symbols_and_version_suffixes(self):
        root=Path(__file__).resolve().parents[1]
        names=json.loads((root/'schemas/c-abi-v1.json').read_text())['symbols']
        output='\n'.join('00000001 T '+name+'@@TEST_ABI' for name in reversed(names))
        output+='\n\n00000002 W _ZNStExample\n00000003 W __compiler_helper\n'
        self.assertEqual(parse_elf_exports(output), names)

    def test_private_native_symbols_remain_visible_to_the_gate(self):
        for name in ('lwvk_crop_quad_bgr_u8', 'lw_crop_quad_bgr_u8'):
            with self.subTest(name=name):
                self.assertEqual(parse_elf_exports('00000001 T '+name+'\n'), [name])


if __name__=='__main__':
    unittest.main()

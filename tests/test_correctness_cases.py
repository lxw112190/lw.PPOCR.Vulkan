import hashlib
from pathlib import Path
import unittest
from correctness_cases import cases

SOURCE = Path(__file__).resolve().parents[1]/'test-images/sample.jpg'
class CasesTests(unittest.TestCase):
    def test_sizes_names_and_modes(self):
        self.assertEqual(len(cases(SOURCE, quick=True)), 3)
        self.assertEqual(len(cases(SOURCE)), 5)
        values = cases(SOURCE, quick=True, extended=True)
        self.assertEqual(len(values), 11)
        self.assertEqual(len({name for name, _ in values}), 11)
        self.assertTrue(all(image.mode == 'RGB' and max(image.size) <= 320 for _, image in values))
        self.assertEqual(values[-1][0], 'blank')
    def test_reproducible_pixels(self):
        def hashes():
            return [hashlib.sha256(image.tobytes()).hexdigest()
                    for _, image in cases(SOURCE, quick=True, extended=True)]
        self.assertEqual(hashes(), hashes())

if __name__ == '__main__':
    unittest.main()

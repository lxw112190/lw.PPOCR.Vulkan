"""Host-only gate for mixed validation-layer stdout/stderr diagnostic parsing."""
import unittest
from profile_gpu import parse_child_output


class ProfileParsing(unittest.TestCase):
    def test_mixed_output(self):
        control, records = parse_child_output(
            'Validation Information: active\nLWVK_PROFILE_CONTROL {"device":"GPU"}\n'
            'LWVK_GPU_PROFILE {"sample":1}\n', 'LWVK_GPU_PROFILE {"sample":2}\n')
        self.assertEqual(control, {'device': 'GPU'})
        self.assertEqual(records, [{'sample': 1}, {'sample': 2}])

    def test_errors_on_both_streams(self):
        for marker in ('Validation Error', 'VUID-test', 'SYNC-HAZARD'):
            for stdout, stderr in ((marker, ''), ('', marker)):
                with self.assertRaises(AssertionError):
                    parse_child_output(stdout + '\nLWVK_PROFILE_CONTROL {}\n', stderr)

    def test_one_control_required(self):
        for stdout in ('', '{}\n', 'LWVK_PROFILE_CONTROL {}\nLWVK_PROFILE_CONTROL {}\n'):
            with self.assertRaises(AssertionError):
                parse_child_output(stdout, '')


if __name__ == '__main__':
    unittest.main()

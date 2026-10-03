"""Verify the private driver recipe and its cleanup; no real GPU claims."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'scripts/ci/mesa-l3-cleanup.patch'


def cleanup_source():
    return '\n'.join(line[1:] for line in PATCH.read_text(encoding='utf-8').splitlines()
                     if line.startswith('+') and not line.startswith('+++')) + '\n'


class DriverRecipeTests(unittest.TestCase):
    def test_cleanup_is_a_real_free_not_a_sanitizer_exclusion(self):
        source = cleanup_source()
        self.assertIn('__attribute__((destructor))', source)
        self.assertIn('free(util_cpu_caps.L3_affinity_mask);', source)
        self.assertIn('util_cpu_caps.L3_affinity_mask = NULL;', source)
        for forbidden in ('__lsan', 'suppress', 'dlopen(', 'RTLD_NODELETE'):
            self.assertNotIn(forbidden, source)

    def test_recipe_preserves_ubuntu_security_series_and_verifies_cache(self):
        script = (ROOT / 'scripts/build_ci_lavapipe.sh').read_text(encoding='utf-8')
        for required in ('c1458ab511b87d60644b2c46d67f7d9d5c0e3db691db6aae11762b1cc5b43ca0',
                         'a870171e601fc9bf99751ce315bacf68a0d57fe4f386a22626111be5dac572c0',
                         'done < "$source/debian/patches/series"', '--fuzz=0',
                         '--wrap-mode=nofallback', 'sha256sum --check files.sha256',
                         'util_cpu_caps_cleanup_on_unload$', 'meson compile',
                         'gcc --version; g++ --version; llvm-config-15 --version; meson --version;'):
            self.assertIn(required, script)
        self.assertLess(script.index('done < "$source/debian/patches/series"'),
                        script.index('meson setup'))

    def test_workflow_uses_private_icd_but_keeps_all_failure_gates(self):
        workflow = (ROOT / '.github/workflows/sanitizers.yml').read_text(encoding='utf-8')
        for required in ('.ci/mesa-lavapipe/share/vulkan/icd.d/lvp_icd.x86_64.json',
                         "hashFiles('scripts/build_ci_lavapipe.sh', 'scripts/ci/mesa-l3-cleanup.patch', '.github/workflows/sanitizers.yml')",
                         'build/sanitize/lw-ppocr-vulkan-probe', 'ctest --test-dir',
                         "LWVK_REQUIRE_DRIVER_UNLOAD_TEST: '1'",
                         'setuptools==80.9.0',
                         'scripts/sanitizer_gpu_probes.py', 'tests/test_http_invalid.py',
                         'detect_leaks=1:halt_on_error=1'):
            self.assertIn(required, workflow)
        for forbidden in ('detect_leaks=0', 'continue-on-error:', 'LSAN_OPTIONS:',
                          'VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING:'):
            self.assertNotIn(forbidden, workflow)
        # The CI software ICD is not an extra dependency in deployment recipes.
        for name in ('scripts/package.py', '.github/workflows/build.yml'):
            self.assertNotIn('mesa-lavapipe', (ROOT / name).read_text(encoding='utf-8'))

    @unittest.skipUnless(sys.platform == 'linux' and
                         os.environ.get('LWVK_REQUIRE_DRIVER_UNLOAD_TEST') == '1',
                         'DSO unload regression requires dedicated Linux sanitizer CI')
    def test_actual_unload_releases_driver_allocation_and_keeps_project_leak_gate(self):
        compiler = shutil.which('clang-14')
        self.assertIsNotNone(compiler, 'The Linux sanitizer regression requires clang-14')
        env = os.environ.copy()
        env['ASAN_OPTIONS'] = ('detect_leaks=1:leak_check_at_exit=1:halt_on_error=1:'
                               'fast_unwind_on_malloc=1')
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            main = folder / 'main.c'
            main.write_text('''#include <dlfcn.h>
#include <stdlib.h>
__attribute__((noinline)) static void project_leak(void) {
    void * volatile p = malloc(128);
    ((volatile char *)p)[0] = 1;
    p = NULL;
}
int main(int argc, char **argv) {
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) return 20;
    void (*initialize)(void) = (void (*)(void))dlsym(module, "initialize");
    if (!initialize) return 21;
    initialize();
    if (dlclose(module)) return 22;
    if (argc > 2) project_leak();
    return 0;
}
''', encoding='utf-8')
            exe = folder / 'unload'
            subprocess.run([compiler, '-O1', '-g', '-fno-omit-frame-pointer',
                            '-fsanitize=address,undefined', str(main), '-ldl', '-o', str(exe)],
                           check=True, timeout=60)
            driver = folder / 'driver.c'
            for patched in (False, True):
                # Same global pointer ownership and exact patch body; uninstrumented
                # DSO allocation is still observed by the executable's ASan runtime.
                driver.write_text('''#include <stdlib.h>
#define DETECT_OS_LINUX 1
static struct { void *L3_affinity_mask; } util_cpu_caps;
void initialize(void) {
    util_cpu_caps.L3_affinity_mask = realloc(NULL, 128);
    ((volatile char *)util_cpu_caps.L3_affinity_mask)[0] = 1;
}
''' + (cleanup_source() if patched else ''), encoding='utf-8')
                library = folder / ('fixed.so' if patched else 'original.so')
                subprocess.run([compiler, '-shared', '-fPIC', '-O1', '-g',
                                '-fno-omit-frame-pointer', str(driver), '-o', str(library)],
                               check=True, timeout=60)
                run = subprocess.run([str(exe), str(library)], env=env,
                                     capture_output=True, text=True, timeout=30)
                if patched:
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    unrelated = subprocess.run([str(exe), str(library), 'project-leak'],
                                               env=env, capture_output=True, text=True, timeout=30)
                    self.assertNotEqual(unrelated.returncode, 0)
                    self.assertIn('128 byte(s)', unrelated.stderr)
                    self.assertIn('LeakSanitizer', unrelated.stderr)
                else:
                    self.assertNotEqual(run.returncode, 0)
                    self.assertIn('128 byte(s)', run.stderr)
                    self.assertIn('LeakSanitizer', run.stderr)


if __name__ == '__main__':
    unittest.main()

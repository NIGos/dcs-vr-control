// Standalone runner for sigscan_test.h and reloc_test.h (built by
// build_sigscan_test.bat). `sigscan_test.exe --reloc DIR` instead resolves
// every hook_sigs.h location in the DLLs of DIR (tools/test_reloc.py).
#include "sigscan_test.h"
#include "reloc_test.h"

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && wcscmp(argv[1], L"--reloc") == 0) return reloc_test::RelocMain(argv[2]);
  sigscan_test::RunAll();
  reloc_test::RunAll();
  printf("%d passed, %d failed (sigscan and reloc)\n", sigscan_test::g_pass, sigscan_test::g_fail);
  return sigscan_test::g_fail ? 1 : 0;
}

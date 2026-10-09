// Unit tests for src/reloc.h (runtime relocation of the production hooks'
// DCS locations). Built with sigscan_test.h by build_sigscan_test.bat.
//
//   1. synthetic images (sigscan_test.h's FakeImage): a function found at its
//      recorded RVA, moved with new displacements (masked hash), logic
//      changed (opcode outside the pattern, fixed byte inside it), ambiguous;
//      vtables moved, wrong length, slot targets through a vtable, unrelocated
//      mappings, import thunks; the masked CodeIs check;
//   2. imports and exports by name in real loaded modules;
//   3. optional, read-only: every hook_sigs.h location in the DCS binaries
//      (DCS_BIN) resolves to its recorded RVA with every FNV check exact,
//      through the payload's API both on the analysed-build fast path and
//      with scanning forced.
// tools/test_reloc.py adds the end-to-end case: shifted and damaged copies
// of the DCS DLLs, resolved by `sigscan_test.exe --reloc DIR`.
#pragma once

#include <map>
#include <string>

#include "../src/reloc.h"
#include "sigscan_test.h"

namespace reloc_test {

using sigscan_test::Check;
using sigscan_test::FakeImage;
using sigscan_test::kFunc;
using sigscan_test::kFuncPattern;

hooksig::Loc Code(uint32_t rva, const char* pattern, uint32_t fnLen, uint64_t fnHash, uint32_t maskFirst,
                  uint32_t maskCount) {
  return {"test.code", 0, hooksig::kCode, rva, nullptr, 0, false, 0, hooksig::kPattern, pattern, 0, 0, 0,
          nullptr, nullptr, 0, fnLen, fnHash, maskFirst, maskCount};
}

hooksig::Loc Vtable(uint32_t rva, const char* rtti, uint16_t len) {
  return {"test.vtbl", 0, hooksig::kVtable, rva, rtti, 0, false, len, hooksig::kPattern, nullptr, 0, 0, 0,
          nullptr, nullptr, 0, 0, 0, 0, 0};
}

// kFunc's displacements: mov rax, [rip+disp] (13..16) and call rel32 (21..24).
const hooksig::Mask kFuncMasks[] = {{13, 4}, {21, 4}};

uint64_t MaskedFuncHash(const uint8_t* f) {
  uint8_t b[sizeof(kFunc)];
  memcpy(b, f, sizeof(b));
  for (const auto& m : kFuncMasks) memset(b + m.at, 0, m.len);
  return reloc::Fnv(b, sizeof(b));
}

uint32_t Resolve(const uint8_t* base, const reloc::Table& t, size_t i = 0, const char** why = nullptr) {
  std::vector<uint32_t> memo(t.count, UINT32_MAX);
  const char* w = nullptr;
  const uint32_t r = reloc::ResolveIn(base, t, i, memo, &w);
  if (why) *why = w;
  return r;
}

void TestCode() {
  const uint64_t gate = MaskedFuncHash(kFunc);
  const hooksig::Loc locs[] = {Code(0x1600, kFuncPattern, sizeof(kFunc), gate, 0, 2)};
  const hooksig::Hash hashes[] = {{0, 0, sizeof(kFunc), reloc::Fnv(kFunc, sizeof(kFunc)), gate}};
  const reloc::Table t{locs, 1, kFuncMasks, hashes, 1};
  const char* why = nullptr;
  {
    FakeImage img;
    memcpy(img.base + 0x1600, kFunc, sizeof(kFunc));
    Check(Resolve(img.base, t, 0) == 0x1600, "reloc: function at its recorded RVA");
    bool masked = true;
    Check(reloc::HashIn(img.base, t, 0, 0x1600, 0x1600, 0x1600 + sizeof(kFunc), hashes[0].hash, &masked) && !masked,
          "reloc: CodeIs exact at the recorded RVA");
    Check(!reloc::HashIn(img.base, t, 0, 0x1600, 0x1600, 0x1600 + 8, hashes[0].hash),
          "reloc: CodeIs without a matching table entry fails");
  }
  {
    // Moved, and its callee and data moved too (new displacements).
    FakeImage img;
    memcpy(img.base + 0x1a20, kFunc, sizeof(kFunc));
    img.base[0x1a20 + 14] ^= 0x5a;
    img.base[0x1a20 + 22] ^= 0x77;
    Check(Resolve(img.base, t, 0, &why) == 0x1a20, "reloc: moved function with new displacements re-found");
    bool masked = false;
    Check(reloc::HashIn(img.base, t, 0, 0x1a20, 0x1600, 0x1600 + sizeof(kFunc), hashes[0].hash, &masked) && masked,
          "reloc: CodeIs passes with displacements masked");
    // The same bytes with an opcode changed: the masked check fails.
    img.base[0x1a20 + 27] = 0xEC;  // add rsp -> sub rsp
    Check(!reloc::HashIn(img.base, t, 0, 0x1a20, 0x1600, 0x1600 + sizeof(kFunc), hashes[0].hash),
          "reloc: CodeIs fails when an opcode changed");
  }
  {
    // Logic changed outside the pattern: the pattern is unique, the function hash is not.
    FakeImage img;
    memcpy(img.base + 0x1a20, kFunc, sizeof(kFunc));
    img.base[0x1a20 + 28] = 0x28;  // add rsp, 0x28
    Check(Resolve(img.base, t, 0, &why) == 0 && why && strstr(why, "code differs"),
          "reloc: changed code outside the pattern resolves to 0");
    // A fixed byte of the pattern changed: not found.
    memcpy(img.base + 0x1a20, kFunc, sizeof(kFunc));
    img.base[0x1a20 + 19] = 0x50;
    Check(Resolve(img.base, t, 0, &why) == 0 && why && strstr(why, "not found"), "reloc: changed pattern byte: not found");
  }
  {
    // Two copies, neither at the recorded RVA: ambiguous.
    FakeImage img;
    memcpy(img.base + 0x1200, kFunc, sizeof(kFunc));
    memcpy(img.base + 0x1a20, kFunc, sizeof(kFunc));
    Check(Resolve(img.base, t, 0, &why) == 0 && why && strstr(why, "not unique"), "reloc: ambiguous pattern resolves to 0");
    // ...but a copy still at the recorded RVA wins (fast path, no scan).
    memcpy(img.base + 0x1600, kFunc, sizeof(kFunc));
    Check(Resolve(img.base, t, 0) == 0x1600, "reloc: recorded RVA preferred over other copies");
  }
  {
    // The recorded RVA lies outside the code sections: never matched there.
    FakeImage img;
    memcpy(img.base + 0x1a20, kFunc, sizeof(kFunc));
    memcpy(img.base + 0x3600, kFunc, sizeof(kFunc));
    const hooksig::Loc data[] = {Code(0x3600, kFuncPattern, sizeof(kFunc), gate, 0, 2)};
    const reloc::Table td{data, 1, kFuncMasks, hashes, 0};
    Check(Resolve(img.base, td, 0) == 0x1a20, "reloc: pattern bytes outside code are not a match");
  }
}

void TestVtables() {
  const char* name = ".?AVFoo@test@@";
  FakeImage img;
  const uint64_t live = reinterpret_cast<uint64_t>(img.base);
  img.AddClass(0x3000, name, 0x2100, 0, 0x2308, live);  // moved from 0x2208
  const hooksig::Loc a[] = {Vtable(0x2208, name, 0)};
  Check(Resolve(img.base, {a, 1, nullptr, nullptr, 0}) == 0x2308, "reloc: moved vtable re-found by RTTI");
  const hooksig::Loc b[] = {Vtable(0x2308, name, 4)};
  Check(Resolve(img.base, {b, 1, nullptr, nullptr, 0}) == 0x2308, "reloc: vtable at its recorded RVA, length 4");
  const hooksig::Loc c[] = {Vtable(0x2308, name, 5)};
  const char* why = nullptr;
  Check(Resolve(img.base, {c, 1, nullptr, nullptr, 0}, 0, &why) == 0 && why && strstr(why, "length"),
        "reloc: vtable with another length resolves to 0");
  const hooksig::Loc d[] = {Vtable(0x2308, ".?AVBar@test@@", 0)};
  Check(Resolve(img.base, {d, 1, nullptr, nullptr, 0}) == 0, "reloc: unknown class resolves to 0");

  // A code location through slot 1 of a vtable, with its pattern verified at
  // the target (tiny functions without a unique pattern).
  memcpy(img.base + 0x1140, kFunc, sizeof(kFunc));
  hooksig::Loc e[] = {Vtable(0x2208, name, 0), Code(0x1100, kFuncPattern, 0, 0, 0, 0)};
  e[1].via = hooksig::kViaVtable;
  e[1].vtable = 0;
  e[1].slot = 1;
  Check(Resolve(img.base, {e, 2, nullptr, nullptr, 0}, 1) == 0x1140, "reloc: slot target through the moved vtable");
  e[1].slot = 2;  // 0x1180: other bytes
  Check(Resolve(img.base, {e, 2, nullptr, nullptr, 0}, 1, &why) == 0 && why && strstr(why, "pattern"),
        "reloc: slot target not matching the pattern resolves to 0");

  // Unrelocated mapping (image resource): pointers hold the preferred base.
  FakeImage raw;
  raw.AddClass(0x3000, name, 0x2100, 0, 0x2208, 0x180000000ull);
  memcpy(raw.base + 0x1140, kFunc, sizeof(kFunc));
  e[1].slot = 1;
  Check(Resolve(raw.base, {e, 2, nullptr, nullptr, 0}, 0) == 0x2208, "reloc: vtable in an unrelocated mapping");
  Check(Resolve(raw.base, {e, 2, nullptr, nullptr, 0}, 1) == 0x1140, "reloc: slot target in an unrelocated mapping");

  // jmp [rip+disp] thunks to an IAT slot.
  auto thunk = [&](uint32_t at, uint32_t slot) {
    raw.base[at] = 0xFF;
    raw.base[at + 1] = 0x25;
    const int32_t d = static_cast<int32_t>(slot) - static_cast<int32_t>(at + 6);
    memcpy(raw.base + at + 2, &d, 4);
  };
  int n = 0;
  thunk(0x1800, 0x3800);
  thunk(0x1810, 0x3808);
  Check(reloc::ImportThunk(raw.base, 0x3800, &n) == 0x1800 && n == 1, "reloc: import thunk by IAT slot");
  thunk(0x1820, 0x3800);
  Check(reloc::ImportThunk(raw.base, 0x3800, &n) == 0 && n == 2, "reloc: two thunks to one slot are ambiguous");
}

void TestNames() {
  // This exe imports GetModuleHandleW from kernel32: the IAT slot holds it.
  const uint8_t* self = sigscan::ImageBase(GetModuleHandleW(nullptr));
  const uint32_t slot = sigscan::ImportSlotRva(self, "KERNEL32.dll", "GetModuleHandleW");
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  Check(slot && *reinterpret_cast<void* const*>(self + slot) ==
                    reinterpret_cast<void*>(GetProcAddress(k32, "GetModuleHandleW")),
        "names: IAT slot by import name (dll name case-insensitive)");
  Check(!sigscan::ImportSlotRva(self, "kernel32.dll", "NoSuchFunction"), "names: missing import");
  HMODULE nt = GetModuleHandleW(L"ntdll.dll");
  const uint32_t rva = sigscan::ExportRva(sigscan::ImageBase(nt), "NtClose");
  Check(rva && sigscan::ImageBase(nt) + rva == reinterpret_cast<const uint8_t*>(GetProcAddress(nt, "NtClose")),
        "names: export by name");
  Check(!sigscan::ExportRva(sigscan::ImageBase(nt), "NoSuchExport"), "names: missing export");
}

// ---- DCS binaries (read-only image mappings) ----

std::map<std::wstring, const uint8_t*> g_images;

const uint8_t* MappedBase(const wchar_t* name) {
  auto it = g_images.find(name);
  return it == g_images.end() ? nullptr : it->second;
}

// Maps every hook_sigs.h module from dir (else from fallback). False when one is missing.
bool MapModules(const std::wstring& dir, const std::wstring& fallback) {
  for (const auto& m : hooksig::kModules) {
    std::wstring path = dir + L"\\" + m.name;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) path = fallback + L"\\" + m.name;
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!h) {
      printf("cannot map %ls (error %lu)\n", path.c_str(), GetLastError());
      return false;
    }
    g_images[m.name] = sigscan::ImageBase(h);
  }
  reloc::g_moduleBase = &MappedBase;
  return true;
}

void ResetResolver(bool force) {
  reloc::g_forceScan = force;
  for (auto& b : reloc::g_state.base) b = nullptr;
}

void TestDcs() {
  wchar_t dir[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"DCS_BIN", dir, MAX_PATH)) {
    printf("SKIP reloc dcs: DCS_BIN not set\n");
    return;
  }
  if (!MapModules(dir, dir)) {
    printf("SKIP reloc dcs: DCS modules not mapped\n");
    return;
  }
  bool analysed = true;
  for (size_t mi = 0; mi < reloc::kModuleCount; ++mi)
    analysed &= reloc::IsAnalysedBuild(MappedBase(hooksig::kModules[mi].name), static_cast<uint8_t>(mi));
  if (!analysed) {
    printf("SKIP reloc dcs: not the analysed build (run tools/gen_hook_sigs.py after extract_manifest.py)\n");
    return;
  }
  // Resolution itself (no fast path): every location at its recorded RVA,
  // every FNV check exact there.
  const reloc::Table t = reloc::Generated();
  int bad = 0;
  for (size_t i = 0; i < t.count; ++i) {
    const uint8_t* base = MappedBase(hooksig::kModules[t.locs[i].module].name);
    const char* why = nullptr;
    const uint32_t r = Resolve(base, t, i, &why);
    if (r != t.locs[i].rva) {
      printf("  %s: 0x%x (%s), recorded 0x%x\n", t.locs[i].id, r, why ? why : "", t.locs[i].rva);
      ++bad;
    }
  }
  Check(bad == 0, "dcs: every hook_sigs.h location resolves to its recorded RVA");
  bad = 0;
  for (size_t k = 0; k < t.hashCount; ++k) {
    const hooksig::Hash& h = t.hashes[k];
    const hooksig::Loc& l = t.locs[h.loc];
    bool masked = true;
    if (!reloc::HashIn(MappedBase(hooksig::kModules[l.module].name), t, h.loc, l.rva, l.rva + h.at,
                       l.rva + h.at + h.len, h.hash, &masked) ||
        masked)
      ++bad;
  }
  Check(bad == 0, "dcs: every FNV check is exact at the recorded RVA");

  // The payload API: the analysed build takes the fast path...
  ResetResolver(false);
  bad = 0;
  for (size_t i = 0; i < hooksig::kCount; ++i) {
    const auto id = static_cast<hooksig::Id>(i);
    bad += reloc::Rva(id) != hooksig::kLocs[i].rva || reloc::Scanned(id);
  }
  Check(bad == 0, "dcs api: analysed build, recorded RVAs without scanning");
  // ...and forced scanning gives the same RVAs, checks and bytes.
  ResetResolver(true);
  bad = 0;
  for (size_t i = 0; i < hooksig::kCount; ++i) {
    const auto id = static_cast<hooksig::Id>(i);
    bad += reloc::Rva(id) != hooksig::kLocs[i].rva || !reloc::Scanned(id);
  }
  for (const auto& h : hooksig::kHashes) {
    const hooksig::Loc& l = hooksig::kLocs[h.loc];
    auto* base = const_cast<uint8_t*>(MappedBase(hooksig::kModules[l.module].name));
    bad += !reloc::CodeIs(static_cast<hooksig::Id>(h.loc), base, l.rva + h.at, l.rva + h.at + h.len, h.hash);
  }
  Check(bad == 0, "dcs api: forced scan, recorded RVAs and every CodeIs check");
  // edtime's prologue check: `sub rsp, 0x28; call rel32`, the rel32 masked.
  const uint8_t* ed = MappedBase(L"edCore.dll");
  const uint8_t want[8] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x00, 0x00, 0x00};
  Check(reloc::BytesAre(hooksig::ED_ED_get_time, ed + hooksig::kLocs[hooksig::ED_ED_get_time].rva, want, 8),
        "dcs api: BytesAre masks the prologue's rel32");
  uint8_t other[8];
  memcpy(other, want, 8);
  other[3] = 0x38;
  Check(!reloc::BytesAre(hooksig::ED_ED_get_time, ed + hooksig::kLocs[hooksig::ED_ED_get_time].rva, other, 8),
        "dcs api: BytesAre still compares the opcodes");
  reloc::g_enabled = false;
  Check(reloc::Rva(hooksig::NG_StructBufferManager_allocate, 0xbf30) == 0xbf30 &&
            !reloc::Scanned(hooksig::NG_StructBufferManager_allocate),
        "dcs api: SigScan=0 returns the recorded RVA without scanning");
  reloc::g_enabled = true;
  ResetResolver(false);
  reloc::g_moduleBase = nullptr;
}

// `sigscan_test.exe --reloc DIR`: resolves every location with scanning
// forced, modules from DIR (else DCS_BIN), and prints one line per location
// and FNV check for tools/test_reloc.py.
int RelocMain(const wchar_t* dir) {
  wchar_t bin[MAX_PATH] = {};
  GetEnvironmentVariableW(L"DCS_BIN", bin, MAX_PATH);
  if (!MapModules(dir, bin)) return 2;
  const reloc::Table t = reloc::Generated();
  std::vector<uint32_t> found(t.count);
  for (size_t i = 0; i < t.count; ++i) {
    const char* why = nullptr;
    found[i] = Resolve(MappedBase(hooksig::kModules[t.locs[i].module].name), t, i, &why);
    printf("RESOLVE %s %ls 0x%x 0x%x %s\n", t.locs[i].id, hooksig::kModules[t.locs[i].module].name, t.locs[i].rva,
           found[i], why ? why : "-");
  }
  for (size_t k = 0; k < t.hashCount; ++k) {
    const hooksig::Hash& h = t.hashes[k];
    const hooksig::Loc& l = t.locs[h.loc];
    bool masked = false;
    const bool ok = reloc::HashIn(MappedBase(hooksig::kModules[l.module].name), t, h.loc, found[h.loc],
                                  l.rva + h.at, l.rva + h.at + h.len, h.hash, &masked);
    printf("HASH %s %d %s\n", l.id, h.at, !ok ? "fail" : masked ? "masked" : "exact");
  }
  return 0;
}

int RunAll() {
  TestCode();
  TestVtables();
  TestNames();
  TestDcs();
  return 0;
}

}  // namespace reloc_test

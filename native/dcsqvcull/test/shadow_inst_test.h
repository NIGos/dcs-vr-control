// Offline tests of shadow_inst.h (R12 stage 1): the FX11 parser on DCS's own
// effect cache, our compile pipeline against it, and the instancing source
// edits on the real model shader sources. Inputs are copied into a temp
// folder first; the originals are only read by the copy.
// Included from test_main.cpp after Check() is defined.
#pragma once

#include <set>

namespace gbtest {  // gb_inst_test.h: the G-buffer part of the lifecycle
void LifecycleGb(const shadowinst::Compiler& cc, const std::wstring& shaders, ID3D11Device* dev);
}

namespace sitest {
using namespace shadowinst;

// DCS's effect cache in the current user's Saved Games (no user name in the source).
const std::wstring kFxoDirPath = [] {
  wchar_t profile[MAX_PATH] = {};
  GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  return std::wstring(profile) + L"\\Saved Games\\DCS\\fxo\\";
}();
const wchar_t* kFxoDir = kFxoDirPath.c_str();
const wchar_t* kShaderDir = L"E:\\SteamLibrary\\steamapps\\common\\DCSWorld\\Bazar\\shaders\\";

std::wstring TempRoot() {
  wchar_t t[MAX_PATH];
  GetTempPathW(MAX_PATH, t);
  return std::wstring(t) + L"dcsqv_r12_test\\";
}

// Copies files matching the extensions from src to dst recursively (skips
// the subfolder named skipDir). Returns the number of files copied.
int CopyTree(const std::wstring& src, const std::wstring& dst, const std::vector<std::wstring>& exts,
             const wchar_t* skipDir) {
  CreateDirectoryW(dst.c_str(), nullptr);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((src + L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  int n = 0;
  do {
    const std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (skipDir && _wcsicmp(name.c_str(), skipDir) == 0) continue;
      n += CopyTree(src + name + L"\\", dst + name + L"\\", exts, nullptr);
      continue;
    }
    const size_t dot = name.rfind(L'.');
    const std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
    bool want = false;
    for (const std::wstring& e : exts) want |= _wcsicmp(ext.c_str(), e.c_str()) == 0;
    if (want && CopyFileW((src + name).c_str(), (dst + name).c_str(), FALSE)) ++n;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return n;
}

std::vector<std::wstring> ListFiles(const std::wstring& dir, const wchar_t* pattern) {
  std::vector<std::wstring> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + pattern).c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do out.push_back(fd.cFileName);
  while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end());
  return out;
}

// One cached effect: DCS's cache header (version string, key), then the
// effect. Only the key and the effect's position are used.
struct FxoFile {
  std::string bytes, key;
  size_t fxOff = 0;
};
bool LoadFxo(const std::wstring& path, FxoFile& f) {
  if (!ReadWholeFile(path, f.bytes) || f.bytes.size() < 16) return false;
  const auto* b = reinterpret_cast<const uint8_t*>(f.bytes.data());
  uint32_t vl, kl;
  memcpy(&vl, b + 2, 4);
  if (6ull + vl + 4 > f.bytes.size()) return false;
  memcpy(&kl, b + 6 + vl, 4);
  const size_t ko = 6 + vl + 4;
  if (ko + kl > f.bytes.size()) return false;
  f.key.assign(f.bytes.data() + ko, kl);
  const uint32_t tag = kFx5Tag;
  const size_t at = f.bytes.find(std::string(reinterpret_cast<const char*>(&tag), 4), ko + kl);
  if (at == std::string::npos) return false;
  f.fxOff = at;
  return true;
}

// D3D shader version type for an FX object type (-1 = not a shader).
int StageOfObjType(uint32_t t) {
  switch (t) {
    case 5: case 25: return D3D11_SHVER_PIXEL_SHADER;
    case 6: case 26: return D3D11_SHVER_VERTEX_SHADER;
    case 7: case 8: case 27: return D3D11_SHVER_GEOMETRY_SHADER;
    case 28: return D3D11_SHVER_COMPUTE_SHADER;
    case 29: return D3D11_SHVER_HULL_SHADER;
    case 30: return D3D11_SHVER_DOMAIN_SHADER;
    default: return -1;
  }
}

// Every shader blob of an effect, in a stable order (variables, then inline).
void AllBlobs(const FxEffect& fx, std::vector<FxBlob>& out) {
  out.clear();
  for (const FxObjectVar& v : fx.vars)
    for (const FxBlob& b : v.shaders) out.push_back(b);
  for (const FxTechnique& t : fx.techs)
    for (const FxPass& p : t.passes)
      for (const FxAssign& a : p.assigns)
        if (a.type == 7 || a.type == 8) out.push_back(a.shader);
}

// Test define sets for model/def_material.fx: static, alpha-tested,
// skinned and damaged casters (R12 sec. 1.1 variants).
std::vector<SourceKey> ModelKeys() {
  const std::vector<std::vector<Define>> sets = {
      {{"DIRECTX11", "true"}, {"NORMAL_SIZE", "3"}, {"POSITION_SIZE", "4"}, {"USE_DCS_DEFERRED", "1"}},
      {{"BLEND_MODE", "BM_ALPHA_TEST"}, {"DIFFUSE_UV", "tc0"}, {"DIRECTX11", "true"}, {"NORMAL_SIZE", "3"},
       {"POSITION_SIZE", "4"}, {"TEXCOORD0_SIZE", "2"}, {"USE_DCS_DEFERRED", "1"}},
      {{"BONES_WEIGHTS_SIZE", "4"}, {"DIRECTX11", "true"}, {"ENABLE_SKELETAL_ANIMATION", ""},
       {"NORMAL_SIZE", "3"}, {"POSITION_SIZE", "4"}, {"USE_DCS_DEFERRED", "1"}},
      // Damage: DAMAGE_UV names a member, so `|| DAMAGE_UV` in shader_macroses
      // is 0 and the damage path needs an alpha-tested diffuse as well.
      {{"BLEND_MODE", "BM_ALPHA_TEST"}, {"DAMAGE_UV", "tc1"}, {"DIFFUSE_UV", "tc0"}, {"DIRECTX11", "true"},
       {"ENABLE_DAMAGE_ARGUMENTS", ""}, {"NORMAL_SIZE", "3"}, {"POSITION_SIZE", "4"}, {"TEXCOORD0_SIZE", "2"},
       {"TEXCOORD1_SIZE", "2"}, {"USE_DCS_DEFERRED", "1"}},
      {{"BLEND_MODE", "BM_ALPHA_TEST"}, {"DAMAGE_RGBA_MASK", ""}, {"DAMAGE_UV", "tc1"}, {"DIFFUSE_UV", "tc0"},
       {"DIRECTX11", "true"}, {"ENABLE_DAMAGE_ARGUMENTS", ""}, {"NORMAL_SIZE", "3"}, {"POSITION_SIZE", "4"},
       {"TEXCOORD0_SIZE", "2"}, {"TEXCOORD1_SIZE", "2"}, {"USE_DCS_DEFERRED", "1"}},
  };
  std::vector<SourceKey> keys;
  for (const auto& s : sets) {
    SourceKey k;
    k.path = "model/def_material.fx";
    k.defines = s;
    for (Define& d : k.defines) d.keyValue = !d.value.empty();
    k.key = BuildKey(k.path, k.defines);
    keys.push_back(k);
  }
  return keys;
}

// Fake FX objects: vtables placed at their dx11backend offsets inside a fake
// image, so the pointer compares in SnapshotRaw hold.
ID3D11VertexShader* g_fakeVs = nullptr;
ID3D11Device* g_fakeDev = nullptr;
struct FakeObj {
  void* vt;
};
FakeObj g_fakePass, g_fakeVar, g_fakeTech[2];
long __fastcall FakeTechDesc(void*, void* out) {
  struct D {
    const char* name;
    uint32_t passes, annotations;
  }* d = static_cast<D*>(out);
  d->name = "t";
  d->passes = 1;
  d->annotations = 0;
  return 0;
}
void* __fastcall FakeGetPass(void*, uint32_t i) { return i == 0 ? &g_fakePass : nullptr; }
long __fastcall FakePassVsDesc(void*, void* out) {
  struct D {
    void* var;
    uint32_t index;
  }* d = static_cast<D*>(out);
  d->var = &g_fakeVar;
  d->index = 0;
  return 0;
}
long __fastcall FakeGetVs(void*, uint32_t, ID3D11VertexShader** vs) {
  *vs = g_fakeVs;
  if (g_fakeVs) g_fakeVs->AddRef();
  return g_fakeVs ? 0 : E_FAIL;
}
long __fastcall FakeGetDevice(void*, ID3D11Device** dev) {
  *dev = g_fakeDev;
  if (g_fakeDev) g_fakeDev->AddRef();
  return g_fakeDev ? 0 : E_FAIL;
}

// Fake dx11backend/NGModel images (vtables only) shared by the fakes.
struct FakeImages {
  std::vector<uint8_t> dxImage, ngImage;
  uint8_t* dx = nullptr;
  uint8_t* ng = nullptr;
  FakeImages() : dxImage(0xc1000), ngImage(0x60000) {
    dx = dxImage.data();
    ng = ngImage.data();
    auto vt = [&](uint32_t rva, int slot, void* fn) { reinterpret_cast<void**>(dx + rva)[slot] = fn; };
    vt(kTechVtbl, 4, reinterpret_cast<void*>(&FakeTechDesc));
    vt(kTechVtbl, 7, reinterpret_cast<void*>(&FakeGetPass));
    vt(kPassVtbl, 5, reinterpret_cast<void*>(&FakePassVsDesc));
    vt(kShaderVarVtbl, 32, reinterpret_cast<void*>(&FakeGetVs));
    vt(kEffectVtbl, 4, reinterpret_cast<void*>(&FakeGetDevice));
    g_fakePass.vt = dx + kPassVtbl;
    g_fakeVar.vt = dx + kShaderVarVtbl;
    g_fakeTech[0].vt = g_fakeTech[1].vt = dx + kTechVtbl;
  }
};

// One DX11Shader laid out like DCS's (+0x50 effect, +0x58 path, +0x78 key,
// +0x98 defines, +0xb0 technique records: std::string name, +0x20 object),
// its ModelMaterialMT (+0x30 shader, +0x210/+0x218 techniques) and a
// ShadowMapRenderable -> item -> material chain for the observer.
struct FakeShader {  // heap allocated; every array size is a multiple of 8
  uint8_t sh[0x248] = {};
  uint8_t mat[0x300] = {};
  uint8_t recs[0x50 * 3] = {};
  uint8_t item[0x100] = {};
  uint8_t renderable[0x20] = {};
  FakeObj effect{};
  std::vector<uint8_t> defs;
  std::string* path = nullptr;
  std::string* key = nullptr;

  void Init(const FakeImages& im, const SourceKey& k) {
    effect.vt = im.dx + kEffectVtbl;
    *reinterpret_cast<void**>(sh) = im.dx + kShaderVtbl;
    *reinterpret_cast<void**>(sh + 0x50) = &effect;
    path = new (sh + 0x58) std::string(k.path);
    key = new (sh + 0x78) std::string(k.key);
    defs.assign(0xa0 * k.defines.size(), 0);
    for (size_t i = 0; i < k.defines.size(); ++i) {
      uint8_t* e = defs.data() + i * 0xa0;
      strcpy_s(reinterpret_cast<char*>(e + 8), 0x48, k.defines[i].name.c_str());
      strcpy_s(reinterpret_cast<char*>(e + 0x58), 0x48, k.defines[i].value.c_str());
      *reinterpret_cast<uint64_t*>(e + 0x50) = k.defines[i].keyValue ? 1 : 0;
    }
    SetDefines(defs.data(), defs.data() + defs.size());
    new (recs + 0x50 * 0) std::string("lockon_shadows");
    new (recs + 0x50 * 1) std::string("lockon_shadows_transparent");
    new (recs + 0x50 * 2) std::string("a_technique_name_longer_than_sso");
    *reinterpret_cast<void**>(recs + 0x50 * 0 + 0x20) = &g_fakeTech[0];
    *reinterpret_cast<void**>(recs + 0x50 * 1 + 0x20) = &g_fakeTech[1];
    *reinterpret_cast<void**>(recs + 0x50 * 2 + 0x20) = &g_fakeTech[1];
    *reinterpret_cast<uint8_t**>(sh + 0xb0) = recs;
    *reinterpret_cast<uint8_t**>(sh + 0xb8) = recs + sizeof(recs);
    *reinterpret_cast<void**>(mat) = im.ng + kModelMatVtbl;
    *reinterpret_cast<void**>(mat + 0x30) = sh;
    *reinterpret_cast<uint64_t*>(mat + 0x210) = 1;
    *reinterpret_cast<uint64_t*>(mat + 0x218) = 2;
    *reinterpret_cast<void**>(item + 0x10) = mat;
    *reinterpret_cast<void**>(renderable + 0x10) = item;
  }
  void SetDefines(uint8_t* b, uint8_t* e) {
    *reinterpret_cast<uint8_t**>(sh + 0x98) = b;
    *reinterpret_cast<uint8_t**>(sh + 0xa0) = e;
    *reinterpret_cast<uint8_t**>(sh + 0xa8) = e;
  }
  ~FakeShader() {
    if (path) path->~basic_string();
    if (key) key->~basic_string();
    if (path)
      for (int i = 0; i < 3; ++i) reinterpret_cast<std::string*>(recs + 0x50 * i)->~basic_string();
  }
};

bool MakeStandInVs(const Compiler& cc, ID3D11Device* dev) {
  const char* src = "float4 vs(float4 p : POSITION) : SV_Position { return p; }";
  ID3DBlob* code = nullptr;
  auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(cc.dll, "D3DCompile"));
  const bool ok = SUCCEEDED(compile(src, strlen(src), "x", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &code, nullptr)) &&
                  SUCCEEDED(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                    &g_fakeVs));
  if (code) code->Release();
  g_fakeDev = dev;
  return ok;
}

void RuntimePath(const Compiler& cc, const std::wstring& shaders, ID3D11Device* dev) {
  if (!dev || !MakeStandInVs(cc, dev)) {
    printf("SKIP shadow inst: no D3D11 device / stand-in VS for the runtime path\n");
    return;
  }
  FakeImages im;
  const SourceKey k = ModelKeys()[1];  // alpha-tested
  auto* f = new FakeShader;
  f->Init(im, k);
  uint8_t* sh = f->sh;
  uint8_t* mat = f->mat;

  // Point the runtime at the fakes and the test sources.
  uint8_t* const savedDx = g_dx;
  const std::wstring savedRoot = g_root;
  g_dx = im.dx;
  g_root = shaders;
  g_compiler = cc;
  MapEntry* const savedMap = g_map;
  g_map = new MapEntry[kMapSize];
  g_mapUsed = 0;
  g_deviceChecked = false;
  g_device = nullptr;

  auto* s = new Snapshot;
  memset(s, 0, sizeof(*s));
  const char* why = SnapshotGuarded(mat, sh, *s);
  const bool snapOk = !why && strcmp(s->path, k.path.c_str()) == 0 && strcmp(s->key, k.key.c_str()) == 0 &&
                      s->defineCount == k.defines.size() && strcmp(s->techName[0], "lockon_shadows") == 0 &&
                      strcmp(s->techName[1], "lockon_shadows_transparent") == 0 && s->passes[0] == 1 &&
                      s->passes[1] == 1 && s->dcsVs[0][0] == g_fakeVs && s->tech[0] == 1 && s->tech[1] == 2;
  Check(snapOk, "shadow inst: snapshot reads path, key, defines, techniques, passes and DCS's VS of a DX11Shader");
  if (why) printf("     snapshot: %s\n", why);
  CheckDevice(*s);
  Check(g_device == dev, "shadow inst: device from CEffect::GetDevice, confirmed by DCS's VS GetDevice");
  KeyResult r;
  CompileKey(*s, r);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    PublishLocked(*s, r);
  }
  ID3D11VertexShader* v0 = FindVs(sh, 1, 0);
  ID3D11VertexShader* v1 = FindVs(sh, 2, 0);
  Check(r.ok && r.vs.size() == 1 && r.vs[0].vs && v0 == r.vs[0].vs && v1 == v0 && !FindVs(sh, 1, 1) &&
            !FindVs(sh, 3, 0) && g_mapUsed.load() == 2,
        "shadow inst: key compiled, one instanced VS shared by both shadow techniques, map (shader, tech, pass)");
  if (!r.ok) printf("     compile: %s\n", r.why.c_str());
  LogResult(r, 1);
  // A different effect at the same address (destroyed and reused) misses.
  FakeObj other{im.dx + kEffectVtbl};
  *reinterpret_cast<void**>(sh + 0x50) = &other;
  Check(!FindVs(sh, 1, 0), "shadow inst: map lookup misses when the shader's effect changed (fingerprint)");
  *reinterpret_cast<void**>(sh + 0x50) = &f->effect;
  // A key that the defines do not reproduce is rejected before compiling.
  auto* bad = new Snapshot(*s);
  strcat_s(bad->key, "X;");
  KeyResult rb;
  CompileKey(*bad, rb);
  Check(!rb.ok && rb.why.find("do not reproduce") != std::string::npos && rb.msA == 0,
        "shadow inst: a key the defines do not reproduce is rejected");
  delete bad;
  // A non-shadow technique is rejected.
  *reinterpret_cast<uint64_t*>(mat + 0x218) = 3;
  auto* s3 = new Snapshot;
  memset(s3, 0, sizeof(*s3));
  KeyResult r3;
  if (!SnapshotGuarded(mat, sh, *s3)) CompileKey(*s3, r3);
  Check(!r3.ok && r3.why.find("not a shadow technique") != std::string::npos,
        "shadow inst: techniques other than the two shadow techniques are rejected");
  delete s3;
  *reinterpret_cast<uint64_t*>(mat + 0x218) = 2;
  // Implausible fields are caught by the snapshot, not crashed on.
  auto* s4 = new Snapshot;
  memset(s4, 0, sizeof(*s4));
  *reinterpret_cast<uint64_t*>(sh + 0x58 + 0x10) = 1ull << 40;  // path size
  const char* why4 = SnapshotGuarded(mat, sh, *s4);
  *reinterpret_cast<uint64_t*>(sh + 0x58 + 0x10) = k.path.size();
  f->SetDefines(f->defs.data(), f->defs.data() + 7);  // vector not a multiple of 0xa0
  const char* why5 = SnapshotGuarded(mat, sh, *s4);
  f->SetDefines(reinterpret_cast<uint8_t*>(16), reinterpret_cast<uint8_t*>(16 + 0xa0));  // unreadable
  const char* why6 = SnapshotGuarded(mat, sh, *s4);
  f->SetDefines(f->defs.data(), f->defs.data() + f->defs.size());
  Check(why4 && why5 && why6 && strstr(why6, "access violation"),
        "shadow inst: snapshot rejects implausible strings/vectors and survives bad pointers");
  delete s4;
  // Release as Shutdown does.
  for (PassVs& p : r.vs)
    if (p.vs) p.vs->Release();
  if (g_device) g_device->Release();
  g_device = nullptr;
  delete s;
  delete[] g_map;
  g_map = savedMap;
  g_dx = savedDx;
  g_root = savedRoot;
  delete f;
  g_fakeVs->Release();
  g_fakeVs = nullptr;
}

// Install (on the analysed binaries, loaded without imports), the observer
// fed with fake casters, the compile workers, the suite summary, Shutdown.
void Lifecycle(const Compiler& cc, const std::wstring& shaders, ID3D11Device* dev) {
  const std::wstring bin = L"E:\\SteamLibrary\\steamapps\\common\\DCSWorld\\bin\\";
  if (!dev || !LoadLibraryExW((bin + L"NGModel.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES) ||
      !LoadLibraryExW((bin + L"dx11backend.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES) ||
      !MakeStandInVs(cc, dev)) {
    printf("SKIP shadow inst: lifecycle needs the DCS binaries and a D3D11 device\n");
    return;
  }
  g_deviceChecked = false;  // RuntimePath used the device check
  const bool installed = shadowinst::Install();
  Check(installed && instcount::g_observer.load() == &Observer,
        "shadow inst: Install verifies the build, starts the workers and registers the caster observer");
  if (!installed) return;
  // From here on compiles read the test copies, and the fakes stand in for
  // DCS's objects.
  g_root = shaders;
  FakeImages im;
  uint8_t* const realDx = g_dx;
  void* const realShaderVtbl = g_shaderVtbl;
  void* const realModelVtbl = g_modelVtbl;
  g_dx = im.dx;
  g_shaderVtbl = im.dx + kShaderVtbl;
  g_modelVtbl = im.ng + kModelMatVtbl;
  auto* a = new FakeShader;
  auto* b = new FakeShader;  // a second DX11Shader with the same key
  auto* c = new FakeShader;  // a different key
  a->Init(im, ModelKeys()[0]);
  b->Init(im, ModelKeys()[0]);
  c->Init(im, ModelKeys()[2]);
  const uint32_t created0 = g_vsCreated.load();
  g_collect = true;
  for (int rep = 0; rep < 3; ++rep)  // repeats are ignored
    for (FakeShader* f : {a, b, c}) instcount::g_observer.load()(f->renderable, nullptr);
  std::atomic<bool> abort{false};
  shadowinst::SuitePhase(10, abort);
  const Totals t = shadowinst::Snap();
  Check(t.seen == 3 && t.keys == 2 && t.ok == 2 && t.done == 2 && t.queued == 0 && t.map == 6 &&
            t.created - created0 == 2,
        "shadow inst: observer queues each shader once, workers compile each key once, map has every "
        "(shader, technique, pass)");
  printf("     lifecycle: seen %u, keys %u, ok %u, map %u, VS created %u\n", t.seen, t.keys, t.ok, t.map, t.created - created0);
  ID3D11VertexShader* va = FindVs(a->sh, 1, 0);
  ID3D11VertexShader* vb = FindVs(b->sh, 2, 0);
  ID3D11VertexShader* vc = FindVs(c->sh, 1, 0);
  Check(va && va == vb && vc && vc != va, "shadow inst: shaders sharing a key share the instanced VS");
  gbtest::LifecycleGb(cc, shaders, dev);  // swaps in its own fakes and restores these
  shadowinst::Shutdown();
  Check(g_state.load() == -2 && !shadowinst::Install() && instcount::g_observer.load() == nullptr &&
            gbcount::g_observer.load() == nullptr && g_device == nullptr && g_results.empty() &&
            !FindVs(a->sh, 1, 0),
        "shadow inst: Shutdown joins the workers, unregisters both observers and releases every object");
  g_dx = realDx;
  g_shaderVtbl = realShaderVtbl;
  g_modelVtbl = realModelVtbl;
  delete a;
  delete b;
  delete c;
  g_fakeVs->Release();
  g_fakeVs = nullptr;
}

void Run() {
  const std::wstring root = TempRoot();
  Compiler cc;
  if (!cc.Load()) {
    printf("SKIP shadow inst: System32 d3dcompiler_47.dll not available\n");
    return;
  }
  printf("     shadow inst: compiler %s, temp folder %ls\n", cc.path.c_str(), root.c_str());
  CreateDirectoryW(root.c_str(), nullptr);
  const int fxoCopied = CopyTree(kFxoDir, root + L"fxo\\", {L".fxo"}, nullptr);
  const int srcCopied = CopyTree(kShaderDir, root + L"shaders\\", {L".fx", L".hlsl", L".h"}, L"fxo");
  printf("     shadow inst: copied %d .fxo files and %d shader sources\n", fxoCopied, srcCopied);
  const std::wstring shaders = root + L"shaders\\";

  // ---- 1. FX11 parser on DCS's effect cache, against D3DReflect ----
  if (fxoCopied == 0) {
    printf("SKIP shadow inst: no .fxo files in %ls\n", kFxoDir);
  } else {
    int files = 0, parsed = 0, exact = 0, countsOk = 0, blobs = 0, reflOk = 0, stageOk = 0, bindOk = 0, isgnOk = 0;
    int keysOk = 0, techVs = 0, techVsOk = 0;
    std::string firstErr;
    for (const std::wstring& name : ListFiles(root + L"fxo\\", L"*.fxo")) {
      FxoFile f;
      if (!LoadFxo(root + L"fxo\\" + name, f)) continue;
      ++files;
      SourceKey k;
      keysOk += ParseKey(f.key, k);
      FxEffect fx;
      std::string err;
      const auto* p = reinterpret_cast<const uint8_t*>(f.bytes.data()) + f.fxOff;
      const size_t n = f.bytes.size() - f.fxOff;
      if (!ParseFx5(p, n, fx, err)) {
        if (firstErr.empty()) firstErr = err;
        continue;
      }
      ++parsed;
      exact += fx.size == n;
      size_t varShaders = 0, inlineAssigns = 0;
      for (const FxObjectVar& v : fx.vars) varShaders += v.shaders.size();
      for (const FxTechnique& t : fx.techs)
        for (const FxPass& ps : t.passes)
          for (const FxAssign& a : ps.assigns) inlineAssigns += a.type == 7 || a.type == 8;
      countsOk += fx.techs.size() == fx.techniques && fx.vars.size() == fx.objects &&
                  varShaders + inlineAssigns == fx.totalShaders && inlineAssigns == fx.inlineShaders;
      // Every shader blob reflects, has the variable's stage, and our DXBC
      // readers agree with D3DReflect.
      for (const FxObjectVar& v : fx.vars)
        for (const FxBlob& b : v.shaders) {
          if (!b.p) continue;
          ++blobs;
          ID3D11ShaderReflection* r = nullptr;
          if (FAILED(cc.reflect(b.p, b.n, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r)))) continue;
          ++reflOk;
          D3D11_SHADER_DESC sd = {};
          r->GetDesc(&sd);
          const int stage = static_cast<int>(D3D11_SHVER_GET_TYPE(sd.Version));
          stageOk += stage == StageOfObjType(v.objType) && stage == DxbcProgramType(b.p, b.n);
          const char* names[shadowtex::kMaxBindings];
          const int nb = shadowtex::ParseDxbcBindings(b.p, b.n, names, shadowtex::kMaxBindings);
          bool same = nb == static_cast<int>(sd.BoundResources);
          for (UINT i = 0; same && i < sd.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC bd = {};
            r->GetResourceBindingDesc(i, &bd);
            same = strcmp(bd.Name, names[i]) == 0;
          }
          bindOk += same;
          uint32_t isz = 0;
          const uint8_t* isgn = DxbcChunk(b.p, b.n, "ISGN", &isz);
          if (!isgn) isgn = DxbcChunk(b.p, b.n, "ISG1", &isz);
          uint32_t count = 0;
          if (isgn && isz >= 4) memcpy(&count, isgn, 4);
          isgnOk += isgn && count == sd.InputParameters;
          r->Release();
        }
      // Technique pass VS extraction (every technique whose passes all set a VS).
      for (const FxTechnique& t : fx.techs) {
        std::vector<FxBlob> vs;
        std::string e2;
        bool anyVs = false;
        for (const FxPass& ps : t.passes)
          for (const FxAssign& a : ps.assigns) anyVs |= a.state == kStateVertexShader && a.shader.p;
        if (!anyVs) continue;
        ++techVs;
        if (!TechniqueVs(fx, t.name.c_str(), vs, e2)) continue;
        bool ok = true;
        for (const FxBlob& b : vs) {
          ID3D11ShaderReflection* r = nullptr;
          ok &= SUCCEEDED(cc.reflect(b.p, b.n, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r)));
          if (!r) continue;
          D3D11_SHADER_DESC sd = {};
          r->GetDesc(&sd);
          ok &= D3D11_SHVER_GET_TYPE(sd.Version) == D3D11_SHVER_VERTEX_SHADER;
          r->Release();
        }
        techVsOk += ok;
      }
      // Truncated effects are rejected, never read out of bounds.
      if (files <= 3)
        for (size_t cut = 0; cut < n; cut += (n / 97) | 1) {
          FxEffect t;
          std::string e3;
          if (ParseFx5(p, cut, t, e3) && t.size > cut) countsOk = -1000000;
        }
    }
    printf("     fxo: %d files, %d parsed (%d consumed exactly), %d header counts match, %d shader blobs: %d "
           "reflect, %d stage match, %d bindings match, %d input signatures match; technique VS %d/%d; keys %d%s%s\n",
           files, parsed, exact, countsOk, blobs, reflOk, stageOk, bindOk, isgnOk, techVsOk, techVs, keysOk,
           firstErr.empty() ? "" : "; first error: ", firstErr.c_str());
    Check(files > 0 && parsed == files && exact == files && countsOk == files,
          "shadow inst: FX11 parser reads every cached .fxo exactly (header counts, no trailing bytes)");
    Check(blobs > 0 && reflOk == blobs && stageOk == blobs && bindOk == blobs && isgnOk == blobs,
          "shadow inst: extracted shader bytecode matches D3DReflect (stage, bindings, input signature)");
    Check(techVs > 0 && techVsOk == techVs, "shadow inst: technique pass VS extraction yields reflectable VS blobs");
    Check(keysOk == files, "shadow inst: cache keys parse back into path + defines (DCS key format)");
  }

  // ---- 2. Our compile pipeline reproduces DCS's compiles (one key per source) ----
  if (fxoCopied && srcCopied) {
    std::set<std::string> donePaths;
    int tried = 0, same = 0, compiled = 0;
    std::string firstDiff;
    for (const std::wstring& name : ListFiles(root + L"fxo\\", L"*.fxo")) {
      FxoFile f;
      SourceKey k;
      if (!LoadFxo(root + L"fxo\\" + name, f) || !ParseKey(f.key, k) || !donePaths.insert(k.path).second) continue;
      ++tried;
      std::vector<uint8_t> out;
      std::string err;
      double ms = 0;
      if (!CompileEffect(cc, shaders, k, false, false, out, err, &ms)) {
        if (firstDiff.empty()) firstDiff = k.path + ": " + err;
        continue;
      }
      ++compiled;
      FxEffect mine, dcs;
      std::string e1, e2;
      const auto* p = reinterpret_cast<const uint8_t*>(f.bytes.data()) + f.fxOff;
      if (!ParseFx5(out.data(), out.size(), mine, e1) || !ParseFx5(p, f.bytes.size() - f.fxOff, dcs, e2)) continue;
      std::vector<FxBlob> a, b;
      AllBlobs(mine, a);
      AllBlobs(dcs, b);
      bool eq = a.size() == b.size() && !a.empty();
      for (size_t i = 0; eq && i < a.size(); ++i)
        eq = a[i].n == b[i].n && (!a[i].n || memcmp(a[i].p, b[i].p, a[i].n) == 0);
      const bool whole = out.size() == f.bytes.size() - f.fxOff && memcmp(out.data(), p, out.size()) == 0;
      same += eq;
      printf("     reproduce %-40s %5.0f ms: %zu shaders %s%s\n", k.path.c_str(), ms, a.size(),
             eq ? "byte-identical" : "DIFFER", whole ? ", whole effect identical" : "");
      if (!eq && firstDiff.empty()) firstDiff = k.path + ": shader bytecode differs";
    }
    printf("     reproduce: %d sources, %d compiled, %d byte-identical to DCS's cache%s%s\n", tried, compiled, same,
           firstDiff.empty() ? "" : "; first issue: ", firstDiff.c_str());
    Check(tried > 0 && same == tried,
          "shadow inst: our D3DCompile2 pipeline (fx_5_0, O3, DCS include order) reproduces DCS's cached shaders");
  }

  // ---- 3. Source edits on the real model sources ----
  if (!srcCopied) {
    printf("SKIP shadow inst: shader sources not found\n");
    return;
  }
  {
    std::string uni, lk;
    const bool read = ReadWholeFile(shaders + L"model\\common\\uniforms.hlsl", uni) &&
                      ReadWholeFile(shaders + L"model\\functions\\lk_shadow.hlsl", lk);
    std::string u2 = uni, l2 = lk;
    const char* w1 = read ? EditUniforms(u2) : "unreadable";
    const char* w2 = read ? EditLkShadow(l2) : "unreadable";
    Check(!w1 && !w2 && CountOf(u2, "uint qvPsoBase;") == 1 && CountOf(u2, "static uint posStructOffset;") == 1 &&
              CountOf(l2, "register(t127)") == 1 && CountOf(l2, "SV_InstanceID") == 1 &&
              CountOf(l2, "posStructOffset = qvInstOffsets[qvPsoBase + qvIID];") == 1,
          "shadow inst: edits apply to the real uniforms.hlsl and lk_shadow.hlsl");
    std::string bad = "cbuffer def_uniforms { float x; }";
    std::string bad2 = lk + "\nVS_OUTPUT_SHADOWS lk_shadow_vs(const VS_INPUT_SHADOWS input)\n";
    Check(EditUniforms(bad) != nullptr && EditLkShadow(bad2) != nullptr,
          "shadow inst: edits refuse files whose anchors are missing or repeated");
  }
  // Device for CreateVertexShader (hardware, else WARP).
  ID3D11Device* dev = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                               nullptr, nullptr)))
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
  int keys = 0, inert = 0, okKeys = 0, created = 0, expected = 0;
  bool negOk = true;
  for (const SourceKey& k : ModelKeys()) {
    ++keys;
    std::vector<uint8_t> a, b, c;
    std::string ea, eb, ec;
    double msA = 0, msB = 0;
    int edits = 0;
    const bool okA = CompileEffect(cc, shaders, k, false, false, a, ea, &msA);
    const bool okB = CompileEffect(cc, shaders, k, true, true, b, eb, &msB, &edits);
    const bool okC = CompileEffect(cc, shaders, k, true, false, c, ec);
    if (!okA || !okB || !okC) {
      printf("     %s: compile failed: a %s | b %s | c %s\n", k.key.c_str(), ea.c_str(), eb.c_str(), ec.c_str());
      continue;
    }
    inert += a == c;  // the edits change nothing without QV_SHADOW_INSTANCED
    FxEffect fa, fb;
    std::string e;
    if (!ParseFx5(a.data(), a.size(), fa, e) || !ParseFx5(b.data(), b.size(), fb, e)) continue;
    bool keyOk = edits == 3;
    for (const char* tech : {"lockon_shadows", "lockon_shadows_transparent"}) {
      std::vector<FxBlob> va, vb;
      std::string why;
      if (!TechniqueVs(fa, tech, va, why) || !TechniqueVs(fb, tech, vb, why) || va.size() != vb.size()) {
        printf("     %s %s: %s\n", k.key.c_str(), tech, why.c_str());
        keyOk = false;
        continue;
      }
      for (size_t p = 0; p < va.size(); ++p) {
        VariantDiff d;
        std::string detail;
        const char* diff = CompareVariants(cc, va[p], vb[p], d, detail);
        const bool exp = !diff && d.cbName == "def_uniforms" && d.cbOffset == 0xfc && d.cbSize == 0x130 &&
                         d.instrB > d.instrA && d.instrB <= d.instrA + 4;
        expected += exp;
        keyOk &= exp;
        printf("     %-60.60s %-26s pass %zu: %s%s%s; VS %u->%u instr, %u->%u temps, SV_InstanceID v%u, %s 0x%x B "
               "@0x%x; a %.0f ms, b %.0f ms\n",
               k.key.c_str(), tech, p, diff ? diff : "checks pass", detail.empty() ? "" : " ", detail.c_str(),
               d.instrA, d.instrB, d.tempsA, d.tempsB, d.iidRegister, d.cbName.c_str(), d.cbSize, d.cbOffset, msA,
               msB);
        ID3D11VertexShader* vs = nullptr;
        if (dev && SUCCEEDED(dev->CreateVertexShader(vb[p].p, vb[p].n, nullptr, &vs))) {
          ++created;
          vs->Release();
        }
        // Negative: (a) against itself is not an instanced variant.
        VariantDiff d2;
        std::string det2;
        negOk &= CompareVariants(cc, va[p], va[p], d2, det2) != nullptr;
      }
    }
    okKeys += keyOk;
  }
  Check(keys > 0 && inert == keys, "shadow inst: edited sources without QV_SHADOW_INSTANCED compile to identical effects");
  Check(keys > 0 && okKeys == keys,
        "shadow inst: instanced VS = reference + SV_InstanceID + t127, def_uniforms identical but the renamed field "
        "@0xfc (static, alpha-test, skinned, damage volume/RGBA variants)");
  Check(dev && created == expected && created > 0, "shadow inst: instanced VS objects are created by a D3D11 device");
  Check(negOk, "shadow inst: the variant check rejects a non-instanced shader");

  // ---- 4. Key and map helpers ----
  {
    SourceKey k;
    const bool ok = ParseKey("model/def_material.fx:A;B=1;C= ;", k) && k.defines.size() == 3 &&
                    !k.defines[0].keyValue && k.defines[1].value == "1" && k.defines[2].value == " " &&
                    FolderOf(k.path) == "model/" && ParseKey("x.fx", k) && k.defines.empty() &&
                    !ParseKey("x.fx:A", k);
    Check(ok, "shadow inst: DCS key format round trip");
  }

  // ---- 5. Runtime path on a fake DX11Shader laid out like DCS's ----
  RuntimePath(cc, shaders, dev);
  if (dev) dev->Release();

  // ---- 6. Build checks on the analysed binaries ----
  {
    const std::wstring bin = L"E:\\SteamLibrary\\steamapps\\common\\DCSWorld\\bin\\";
    HMODULE ngm = LoadLibraryExW((bin + L"NGModel.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    HMODULE dxm = LoadLibraryExW((bin + L"dx11backend.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!ngm || !dxm) {
      printf("SKIP shadow inst: DCS binaries not found\n");
    } else {
      const char* why = VerifyBuild(reinterpret_cast<uint8_t*>(ngm), reinterpret_cast<uint8_t*>(dxm));
      Check(!why, "shadow inst: build checks pass (RTTI, DX11Shader field stores, key/macro builders, GetDevice, "
                  "GetVertexShader)");
      if (why) printf("     %s\n", why);
      Check(!FindShaderRoot(ngm).empty(), "shadow inst: Bazar\\shaders found from the NGModel.dll location");
    }
  }

  // ---- 7. Install, observer, workers, suite summary, Shutdown ----
  {
    ID3D11Device* dev2 = nullptr;
    D3D_FEATURE_LEVEL fl2 = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl2, 1, D3D11_SDK_VERSION, &dev2,
                                 nullptr, nullptr)))
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl2, 1, D3D11_SDK_VERSION, &dev2, nullptr,
                        nullptr);
    Lifecycle(cc, shaders, dev2);
    if (dev2) dev2->Release();
  }
}

}  // namespace sitest

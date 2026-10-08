// GPU utilisation through NVML (shipped with the NVIDIA driver), loaded
// dynamically so the DLL still works without it.
#pragma once

namespace gpu {

using NvmlInit = int (*)();
using NvmlGetHandle = int (*)(unsigned, void**);
struct NvmlUtil {
  unsigned gpu, memory;
};
using NvmlGetUtil = int (*)(void*, NvmlUtil*);

NvmlGetUtil g_getUtil = nullptr;
void* g_device = nullptr;

bool Init() {
  if (g_device) return true;
  HMODULE m = LoadLibraryW(L"nvml.dll");
  if (!m) return false;
  auto init = reinterpret_cast<NvmlInit>(GetProcAddress(m, "nvmlInit_v2"));
  auto handle = reinterpret_cast<NvmlGetHandle>(GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2"));
  g_getUtil = reinterpret_cast<NvmlGetUtil>(GetProcAddress(m, "nvmlDeviceGetUtilizationRates"));
  if (!init || !handle || !g_getUtil || init() != 0 || handle(0, &g_device) != 0) {
    g_device = nullptr;
    return false;
  }
  return true;
}

// Percentage of the last sample period the GPU was busy, or -1.
int Utilization() {
  if (!g_device || !g_getUtil) return -1;
  NvmlUtil u{};
  return g_getUtil(g_device, &u) == 0 ? static_cast<int>(u.gpu) : -1;
}

}  // namespace gpu

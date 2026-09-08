// H4CN.asi entry point.
//
// Initialization is a one-shot guarded by an INIT_ONCE and reachable from two
// places:
//   * DllMain(DLL_PROCESS_ATTACH) - the ASI loader (Ultimate ASI Loader
//     proxying version.dll) loads this module before heroes4.exe starts
//     rendering, so the loader lock is the point where patching the game's
//     code cannot race with a thread executing it.
//   * the `zk` export - no-argument, no-return entry for other injection
//     tools (manual mappers, trainers) that GetProcAddress it and call it
//     after loading the module. When the loader already ran DllMain the call
//     is a no-op; a concurrent caller blocks until initialization finished.
#include <Windows.h>

#include "diagnostics.h"
#include "font_cache.h"
#include "hooks.h"
#include "inline_hook.h"

namespace {

// This module's own handle, used to locate H4CN.log / H4CN.toml next to it.
// Null only when the module was mapped without ever running DllMain; zk then
// recovers it from its own address, and if that fails too InitDiagnostics
// degrades to the exe's directory.
HMODULE g_module = nullptr;

INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK InitializeOnce(PINIT_ONCE /*init_once*/, PVOID /*parameter*/,
                             PVOID* /*context*/) {
  h4cn::InitDiagnostics(g_module);
  h4cn::InitFontCache();
  h4cn::InstallHooks();
  return TRUE;
}

// Runs the DLL_PROCESS_ATTACH sequence exactly once, from whichever entry
// point got there first.
void Initialize() {
  InitOnceExecuteOnce(&g_init_once, InitializeOnce, nullptr, nullptr);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(module);
      g_module = module;
      Initialize();
      break;
    case DLL_PROCESS_DETACH:
      // Restore the patched prologues before this module's pages go away, then
      // release the glyph caches and their GDI objects.
      h4cn::UninstallAllHooks();
      h4cn::ShutdownFontCache();
      break;
    default:
      break;
  }
  return TRUE;
}

// extern "C" keeps the export table entry exactly "zk" (MSVC strips the cdecl
// underscore for C-linkage dllexports), so GetProcAddress(h, "zk") resolves.
extern "C" __declspec(dllexport) void __cdecl zk() {
  if (g_module == nullptr) {
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&zk), &self)) {
      g_module = self;
    }
  }
  Initialize();
}

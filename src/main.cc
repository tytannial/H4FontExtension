// H4CN.asi entry point.
//
// The ASI loader (Ultimate ASI Loader proxying version.dll) loads this module
// from the game directory before heroes4.exe starts rendering, so all work
// happens in DLL_PROCESS_ATTACH under the loader lock: that is the only point
// where patching the game's code cannot race with a thread executing it.
#include <Windows.h>

#include "diagnostics.h"
#include "font_cache.h"
#include "hooks.h"
#include "inline_hook.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(module);
      h4cn::InitDiagnostics(module);
      h4cn::InitFontCache();
      h4cn::InstallHooks();
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

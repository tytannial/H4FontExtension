// H4CN.asi entry point.
//
// Initialization is a one-shot guarded by an INIT_ONCE and reachable from two
// places:
//   * DllMain(DLL_PROCESS_ATTACH) - the ASI loader (Ultimate ASI Loader
//     proxying version.dll) loads this module before heroes4.exe starts
//     rendering, so the loader lock is the point where patching the game's
//     code cannot race with a thread executing it. Heroes4GL's mod loader
//     (below) LoadLibrary's this same DllMain path from its own DllMain.
//   * the `zk` export - no-argument, no-return entry for other injection
//     tools (manual mappers, trainers) that GetProcAddress it and call it
//     after loading the module. When the loader already ran DllMain the call
//     is a no-op; a concurrent caller blocks until initialization finished.
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

// old dll compactibility: a no-argument, no-return entry point for injection
extern "C" __declspec(dllexport) void __cdecl zk() {}

// Heroes4GL (.mod) compatibility.
//
// Heroes4GL - the ddraw.dll GL wrapper from the HeroesGL project - scans
// "<game>\mods\*.mod" inside its own DllMain, LoadLibrary's every hit and keeps
// the module only when all four exports below resolve (see HeroesGL's
// Mods.cpp). It has no init export: the LoadLibrary runs our DllMain before the
// exports are looked up, so by the time GetName is called the hooks are already
// installed. The four are pure ABI stubs that must match the loader's typedefs
// exactly - __stdcall, and the LoadPackages callback is __cdecl. This plugin
// contributes no menu entries (GetMenu returns NULL; when no mod has a submenu
// H4GL deletes the "Mods" popup entirely) and requests no ERES packages.
//
// One host at a time: with both plugins\H4CN.asi and mods\H4CN.mod installed,
// the same image maps twice; the second instance finds the game's prologues
// already patched, its prologue check refuses to re-patch and it idles
// (H4CN.log then shows "0/7 font hooks installed").
//
// `GetMenu` is also a user32 API prototype in this TU, so a dllexported
// extern "C" GetMenu would clash with it; these exports go through linker
// alias directives instead (decorated names = extern "C" __stdcall on x86).
extern "C" const char* __stdcall H4cnModGetName() {
  return "H4CN Chinese font";
}

extern "C" HMENU __stdcall H4cnModGetMenu(DWORD /*first_command_id*/) {
  return nullptr;
}

extern "C" void __stdcall H4cnModSetHWND(HWND /*hWnd*/) {}

extern "C" void __stdcall H4cnModLoadPackages(
    void(__cdecl* /*callback*/)(const char* /*name*/)) {}

#pragma comment(linker, "/export:GetName=_H4cnModGetName@0")
#pragma comment(linker, "/export:GetMenu=_H4cnModGetMenu@4")
#pragma comment(linker, "/export:SetHWND=_H4cnModSetHWND@4")
#pragma comment(linker, "/export:LoadPackages=_H4cnModLoadPackages@4")

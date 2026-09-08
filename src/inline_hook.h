// Minimal x86 inline hook: overwrite a function prologue with a jump, keep a
// trampoline that runs the displaced bytes and jumps back.
#ifndef H4CN_INLINE_HOOK_H_
#define H4CN_INLINE_HOOK_H_

#include "game_addrs.h"

namespace h4cn {

enum class HookInstallResult {
  kOk,
  kBadArgs,           // null hook or target.len outside what a patch can hold
  kPrologueMismatch,  // not the exe revision these addresses were taken from
  kAllocFailed,       // VirtualAlloc/VirtualProtect refused
};

// The failure reason in a short lowercase word, for diagnostic lines.
const char* HookInstallResultName(HookInstallResult result);

// Patches `target` so it jumps to `hook`.
//
// Returns kOk, or the reason it patched nothing. Nothing is written to the
// game's code unless every byte of target.prologue matched.
//
// `orig` receives the trampoline and may be null. When it is null no trampoline
// is built at all, which is the right choice for a hook that fully replaces the
// original implementation: an unused trampoline is only a way to accidentally
// execute stale machine code later.
HookInstallResult InstallInlineHook(const game::HookTarget& target, void* hook,
                                    void** orig);

// Restores every patched prologue and frees its trampoline.
//
// Must run before this module's memory goes away: the jumps point into it, so
// unloading without restoring would make the game call unmapped memory. Other
// threads are assumed to be gone (DLL_PROCESS_DETACH); freeing a trampoline
// while another thread is executing it is not survivable.
void UninstallAllHooks();

}  // namespace h4cn

#endif  // H4CN_INLINE_HOOK_H_

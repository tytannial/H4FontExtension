#include "inline_hook.h"

#include <Windows.h>

#include <array>
#include <cstring>
#include <vector>

namespace h4cn {
namespace {

// JMP rel32 needs 5 bytes; MOV EAX, imm32 + JMP EAX needs 7. Anything longer is
// padded with NOP so the overwritten range keeps its instruction boundaries.
constexpr int kRelativeJumpLen = 5;
constexpr int kAbsoluteJumpLen = 7;
constexpr int kMaxHookLen = 8;  // sizeof(HookTarget::prologue)
constexpr int kTrampolineSize = 32;
constexpr uint8_t kNop = 0x90;

struct InstalledHook {
  uint32_t addr = 0;
  int len = 0;
  std::array<uint8_t, kMaxHookLen> saved{};
  uint8_t* trampoline = nullptr;
};

std::vector<InstalledHook> g_hooks;

}  // namespace

const char* HookInstallResultName(HookInstallResult result) {
  switch (result) {
    case HookInstallResult::kOk:
      return "ok";
    case HookInstallResult::kBadArgs:
      return "bad-args";
    case HookInstallResult::kPrologueMismatch:
      return "prologue-mismatch";
    case HookInstallResult::kAllocFailed:
      return "alloc-failed";
  }
  return "unknown";
}

HookInstallResult InstallInlineHook(const game::HookTarget& target, void* hook,
                                    void** orig) {
  if (hook == nullptr || target.len < kRelativeJumpLen ||
      target.len > kMaxHookLen) {
    return HookInstallResult::kBadArgs;
  }

  uint8_t* const code = reinterpret_cast<uint8_t*>(target.addr);
  if (std::memcmp(code, target.prologue.data(), target.len) != 0) {
    return HookInstallResult::kPrologueMismatch;
  }

  uint8_t* trampoline = nullptr;
  if (orig != nullptr) {
    trampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, kTrampolineSize,
                                                    MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) {
      return HookInstallResult::kAllocFailed;
    }
    // Displaced prologue, then jump back to the first untouched instruction.
    std::memcpy(trampoline, code, target.len);
    const uint32_t resume = target.addr + target.len;
    const uint32_t jump_at =
        reinterpret_cast<uint32_t>(trampoline) + target.len;
    trampoline[target.len] = 0xE9;
    const uint32_t rel = resume - (jump_at + kRelativeJumpLen);
    std::memcpy(trampoline + target.len + 1, &rel, sizeof(rel));
  }

  DWORD old_protect = 0;
  if (VirtualProtect(code, target.len, PAGE_EXECUTE_READWRITE, &old_protect) ==
      0) {
    if (trampoline != nullptr) VirtualFree(trampoline, 0, MEM_RELEASE);
    return HookInstallResult::kAllocFailed;
  }

  InstalledHook record;
  record.addr = target.addr;
  record.len = target.len;
  std::memcpy(record.saved.data(), code, target.len);
  record.trampoline = trampoline;

  if (target.len >= kAbsoluteJumpLen) {
    code[0] = 0xB8;  // MOV EAX, hook
    std::memcpy(code + 1, static_cast<const void*>(&hook), sizeof(hook));
    code[5] = 0xFF;  // JMP EAX
    code[6] = 0xE0;
    std::memset(code + kAbsoluteJumpLen, kNop, target.len - kAbsoluteJumpLen);
  } else {
    code[0] = 0xE9;  // JMP rel32
    const uint32_t rel =
        reinterpret_cast<uint32_t>(hook) - target.addr - kRelativeJumpLen;
    std::memcpy(code + 1, &rel, sizeof(rel));
    std::memset(code + kRelativeJumpLen, kNop, target.len - kRelativeJumpLen);
  }

  FlushInstructionCache(GetCurrentProcess(), code, target.len);
  DWORD ignored = 0;
  VirtualProtect(code, target.len, old_protect, &ignored);

  g_hooks.push_back(record);
  if (orig != nullptr) *orig = trampoline;
  return HookInstallResult::kOk;
}

void UninstallAllHooks() {
  for (const InstalledHook& hook : g_hooks) {
    uint8_t* const code = reinterpret_cast<uint8_t*>(hook.addr);
    DWORD old_protect = 0;
    if (VirtualProtect(code, hook.len, PAGE_EXECUTE_READWRITE, &old_protect) !=
        0) {
      std::memcpy(code, hook.saved.data(), hook.len);
      FlushInstructionCache(GetCurrentProcess(), code, hook.len);
      DWORD ignored = 0;
      VirtualProtect(code, hook.len, old_protect, &ignored);
    }
    if (hook.trampoline != nullptr) {
      VirtualFree(hook.trampoline, 0, MEM_RELEASE);
    }
  }
  g_hooks.clear();
}

}  // namespace h4cn

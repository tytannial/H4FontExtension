// Installation of the seven font hooks that replace the game's byte-oriented
// text renderer with a GBK-aware one.
#ifndef H4CN_HOOKS_H_
#define H4CN_HOOKS_H_

namespace h4cn {

// Installs every hook. Targets are patched independently: one whose prologue
// does not match (a different exe revision) is skipped and keeps the stock
// implementation, which shows mojibake instead of crashing.
void InstallHooks();

}  // namespace h4cn

#endif  // H4CN_HOOKS_H_

// Every diagnostic goes to two channels: OutputDebugStringA (visible in
// DebugView / the VS output window) and one line appended to H4CN.log next to
// the .asi. The log always gets the startup summary, so its presence and its
// hook count are proof the plugin loaded - and its content localises the
// failure without a debugger:
//   * a hook that refused to install (wrong exe revision),
//   * a GDI font Windows silently substituted,
//   * a font size whose GDI context could not be created.
// Without this the game just starts showing mojibake and every cause looks the
// same from the outside.
//
// Both channels are safe under the loader lock: OutputDebugStringA never blocks
// a debug-less client for long, and the log write is a single create/append/
// close that is silently dropped on any error.
//
// The channels are gated by H4CN.toml [general], which is only read outside
// DllMain. Lines logged before that (the hook-install summary) are buffered
// with their original timestamps and replayed by DiagApplyChannels, so the
// switches describe the whole session, not just the tail of it.
#ifndef H4CN_DIAGNOSTICS_H_
#define H4CN_DIAGNOSTICS_H_

namespace h4cn {

// Captures the module handle used to locate H4CN.log and H4CN.toml next to the
// .asi. Called from DllMain(DLL_PROCESS_ATTACH) before anything reports.
void InitDiagnostics(void* module);

// The .asi's own directory with a trailing backslash ("" when unknown). Only
// reads cached state - safe from anywhere after InitDiagnostics.
const char* DiagModuleDir();

// Applies the channel switches from the configuration and flushes everything
// buffered so far. Called once by EnsureConfigLoaded.
void DiagApplyChannels(bool log_file, bool log_debug_view);

// One line to the enabled channels, prefixed "H4CN: ".
void DiagLog(const char* message);

}  // namespace h4cn

#endif  // H4CN_DIAGNOSTICS_H_

#include "diagnostics.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace h4cn {
namespace {

// Module directory with trailing backslash; empty when unknown or when the
// path computation failed.
char g_module_dir[MAX_PATH] = {};
char g_log_path[4 * MAX_PATH] = {};

// Lines logged before the config chose the channels. Only the hook summary
// can reach this, so eight is generous; overflow drops the new line rather
// than emitting it under not-yet-known switches.
constexpr size_t kPendingMax = 8;
struct PendingLine {
  char stamp[24];
  char text[256];
};
PendingLine g_pending[kPendingMax];
size_t g_pending_count = 0;

bool g_channels_ready = false;
bool g_log_file = true;
bool g_debug_view = true;

// "2026-09-07 17:33:01 " - a plain local stamp. The log is one line per
// startup (plus warnings), so without a stamp a stack of identical
// "7/7 installed" lines cannot be tied to a run.
void AppendStamp(char* out, size_t cap) {
  SYSTEMTIME t = {};
  GetLocalTime(&t);
  sprintf_s(out, cap, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth,
            t.wDay, t.wHour, t.wMinute, t.wSecond);
}

void AppendToLogFile(const char* stamp, const char* line) {
  if (g_log_path[0] == 0) return;
  const HANDLE file = CreateFileA(g_log_path, FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;  // Locked, read-only or no access:
  DWORD written = 0;                         // diagnostics must never fail loud
  WriteFile(file, stamp, static_cast<DWORD>(std::strlen(stamp)), &written,
            nullptr);
  WriteFile(file, " ", 1, &written, nullptr);
  WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written,
            nullptr);
  WriteFile(file, "\n", 1, &written, nullptr);
  CloseHandle(file);
}

void Emit(const char* stamp, const char* line) {
  if (g_debug_view) OutputDebugStringA(line);
  if (g_log_file) AppendToLogFile(stamp, line);
}

}  // namespace

void InitDiagnostics(void* module) {
  char path[MAX_PATH];
  const DWORD len = GetModuleFileNameA(static_cast<HMODULE>(module), path,
                                       static_cast<DWORD>(sizeof(path)));
  if (len == 0 || len >= sizeof(path)) return;
  char* slash = std::strrchr(path, '\\');
  if (slash == nullptr) return;
  slash[1] = 0;
  sprintf_s(g_module_dir, sizeof(g_module_dir), "%s", path);

  strcpy_s(g_log_path, sizeof(g_log_path), g_module_dir);
  strcat_s(g_log_path, sizeof(g_log_path), "H4CN.log");
}

const char* DiagModuleDir() { return g_module_dir; }

void DiagApplyChannels(bool log_file, bool log_debug_view) {
  if (g_channels_ready) return;
  g_log_file = log_file;
  g_debug_view = log_debug_view;
  g_channels_ready = true;
  for (size_t i = 0; i < g_pending_count; ++i) {
    Emit(g_pending[i].stamp, g_pending[i].text);
  }
  g_pending_count = 0;
}

void DiagLog(const char* message) {
  PendingLine entry;
  AppendStamp(entry.stamp, sizeof(entry.stamp));
  sprintf_s(entry.text, "H4CN: %s", message);
  if (!g_channels_ready) {
    if (g_pending_count < kPendingMax) {
      g_pending[g_pending_count++] = entry;
    }
    return;
  }
  Emit(entry.stamp, entry.text);
}

namespace {

// One summary line per this many calls of a slot. The busy per-frame hooks
// (get_width, draw_to_rect) reach it in seconds; cold hooks only ever add a
// line when they are genuinely being hammered, so the log stays readable.
constexpr unsigned long long kPerfReportEvery = 2048;

struct PerfSlotCounters {
  std::atomic<unsigned long long> calls{0};
  std::atomic<unsigned long long> us_total{0};
  std::atomic<unsigned long long> us_max{0};
};
PerfSlotCounters g_perf_slots[kPerfSlotCount];

const char* const kPerfNames[kPerfSlotCount] = {
    "draw_to_pt", "get_width",    "get_column",         "wrap_text_mm",
    "wrap_text",  "draw_to_rect", "get_wrapped_height", "glyph_rasterize",
};

// Written once by PerfSetEnabled before the enabled flag goes true, then only
// read - so the relaxed load in ScopedPerf is enough to publish it.
unsigned long long g_perf_freq = 1;
std::atomic<bool> g_perf_enabled{false};

inline unsigned long long PerfTickNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<unsigned long long>(t.QuadPart);
}

}  // namespace

void PerfSetEnabled(bool enabled) {
  if (enabled) {
    LARGE_INTEGER f;
    if (QueryPerformanceFrequency(&f) != 0 && f.QuadPart > 0) {
      g_perf_freq = static_cast<unsigned long long>(f.QuadPart);
    }
    g_perf_enabled.store(true, std::memory_order_release);
  } else {
    g_perf_enabled.store(false, std::memory_order_relaxed);
  }
}

bool PerfEnabled() { return g_perf_enabled.load(std::memory_order_relaxed); }

void PerfRecord(int slot, unsigned long long us) {
  if (slot < 0 || slot >= kPerfSlotCount) return;
  PerfSlotCounters& ctr = g_perf_slots[slot];
  const unsigned long long calls =
      ctr.calls.fetch_add(1, std::memory_order_relaxed) + 1;
  ctr.us_total.fetch_add(us, std::memory_order_relaxed);
  unsigned long long prev_max = ctr.us_max.load(std::memory_order_relaxed);
  while (us > prev_max && !ctr.us_max.compare_exchange_weak(
                              prev_max, us, std::memory_order_relaxed)) {
  }
  if (calls < kPerfReportEvery) return;
  const unsigned long long total =
      ctr.us_total.exchange(0, std::memory_order_relaxed);
  const unsigned long long peak =
      ctr.us_max.exchange(0, std::memory_order_relaxed);
  ctr.calls.store(0, std::memory_order_relaxed);
  char message[160];
  // total/peak are microseconds; only the "total=" field converts to ms.
  sprintf_s(message, "perf: %s n=%llu avg=%.1fus max=%.1fus total=%llums",
            kPerfNames[slot], calls,
            static_cast<double>(total) / static_cast<double>(calls),
            static_cast<double>(peak), total / 1000ULL);
  DiagLog(message);
}

ScopedPerf::ScopedPerf(int slot) : slot_(-1), start_(0) {
  if (!PerfEnabled()) return;
  slot_ = slot;
  start_ = PerfTickNow();
}

ScopedPerf::~ScopedPerf() {
  if (slot_ < 0) return;
  const unsigned long long now = PerfTickNow();
  const unsigned long long us = static_cast<unsigned long long>(
      static_cast<double>(now - start_) * 1000000.0 /
      static_cast<double>(g_perf_freq));
  PerfRecord(slot_, us);
}

}  // namespace h4cn

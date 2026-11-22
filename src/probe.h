// ufc3 - dev probe (off by default): finds game data at run time and who touches it.
//
// --ufc3_probe_timer=true      In a match: snapshots every big-endian float in 5..300 in the game's
//                               heaps, keeps the ones that fall steadily over 10 s, logs them with
//                               their rate in game seconds per real second ([probe] lines), then
//                               logs the code that writes the steadiest one (host RIPs, for
//                               llvm-symbolizer with ufc3.pdb).
// --ufc3_probe_pattern=<hex>   Finds these bytes in the game's heaps (up to 4 copies) and logs the
//                               code that reads them, with the offset read, for 10 min.
//
// Readers and writers are caught with page protection and a vectored exception handler that lets
// the access through one instruction at a time (trap flag). Only for finding code; slow while on.

#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(ufc3_probe_timer, false, "ufc3",
                    "Dev: find the match clock in memory and the code that advances it ([probe] lines)");
REXCVAR_DEFINE_STRING(ufc3_probe_pattern, "", "ufc3",
                      "Dev: hex bytes to find in the game's heaps; logs the code that reads them ([probe] lines)");
REXCVAR_DEFINE_STRING(ufc3_probe_watch, "", "ufc3",
                      "Dev: guest addresses (hex, comma separated); in a match, logs their values and the code that writes them");

bool Ufc3InMatch();  // main.cpp

namespace ufc3_probe {

inline uint8_t* g_base = nullptr;

// ---- watch: page protection + trap flag ----------------------------------------------------
struct Watch {
  uintptr_t lo = 0, hi = 0;  // host range whose accesses are logged
  uint32_t guest_lo = 0;
  bool writes_only = false;
};
inline std::mutex g_watch_mutex;
inline std::vector<Watch> g_watches;
inline std::atomic<bool> g_watching{false};
// Distinct (RIP, offset) pairs with a count each, so a hot loop can't crowd out a rare reader.
struct Hit {
  std::atomic<uintptr_t> rip{0};
  std::atomic<uint32_t> offset{0};
  std::atomic<uint32_t> count{0};
};
constexpr size_t kMaxHits = 4096;  // power of two
// Table 0: readers (pattern probe); table 1: writers (timer probe).
inline Hit g_hits[2][kMaxHits];
inline std::atomic<size_t> g_hit_count[2];  // all accesses

inline void Record(int table, uintptr_t rip, uint32_t offset) {
  g_hit_count[table].fetch_add(1, std::memory_order_relaxed);
  const uintptr_t key = rip ^ (uintptr_t(offset) << 40);
  size_t i = size_t((key * 0x9E3779B97F4A7C15ull) >> 52) & (kMaxHits - 1);
  for (size_t n = 0; n < kMaxHits; ++n, i = (i + 1) & (kMaxHits - 1)) {
    Hit& h = g_hits[table][i];
    uintptr_t cur = h.rip.load(std::memory_order_acquire);
    if (cur == 0) {
      uintptr_t expected = 0;
      if (h.rip.compare_exchange_strong(expected, rip)) {
        h.offset.store(offset, std::memory_order_release);
        h.count.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      cur = expected;
    }
    if (cur == rip && h.offset.load(std::memory_order_acquire) == offset) {
      h.count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

inline void ClearHits(int table) {
  for (Hit& h : g_hits[table]) {
    h.rip.store(0);
    h.offset.store(0);
    h.count.store(0);
  }
  g_hit_count[table].store(0);
}
inline thread_local uintptr_t t_reprotect_page = 0;
inline thread_local DWORD t_reprotect_prot = 0;

inline DWORD WatchProt(bool writes_only) { return writes_only ? PAGE_READONLY : PAGE_NOACCESS; }

inline LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_SINGLE_STEP && t_reprotect_page) {
    // Re-protect only a page still watched, under the lock DisarmAll holds while it unprotects:
    // a page disarmed while this thread was mid-access stayed read-only and the next write crashed.
    std::lock_guard lock(g_watch_mutex);
    bool still = false;
    for (const Watch& w : g_watches)
      still |= t_reprotect_page >= (w.lo & ~uintptr_t(4095)) &&
               t_reprotect_page <= ((w.hi - 1) & ~uintptr_t(4095));
    if (still) {
      DWORD old;
      VirtualProtect(reinterpret_cast<void*>(t_reprotect_page), 4096, t_reprotect_prot, &old);
    }
    t_reprotect_page = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code != EXCEPTION_ACCESS_VIOLATION || !g_watching.load(std::memory_order_relaxed))
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t addr = ep->ExceptionRecord->ExceptionInformation[1];
  const uintptr_t page = addr & ~uintptr_t(4095);
  bool ours = false, in_range = false, writes_only = false;
  uint32_t offset = 0;
  {
    std::lock_guard lock(g_watch_mutex);
    for (const Watch& w : g_watches) {
      if (page >= (w.lo & ~uintptr_t(4095)) && page <= ((w.hi - 1) & ~uintptr_t(4095))) {
        ours = true;
        writes_only = w.writes_only;
        if (addr >= w.lo && addr < w.hi) {
          in_range = true;
          offset = uint32_t(addr - w.lo);
        }
      }
    }
  }
  if (!ours)
    return EXCEPTION_CONTINUE_SEARCH;
  const bool is_write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
  if (in_range && (!writes_only || is_write))
    Record(writes_only ? 1 : 0, uintptr_t(ep->ContextRecord->Rip), offset);
  DWORD old;
  VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READWRITE, &old);
  t_reprotect_page = page;
  t_reprotect_prot = WatchProt(writes_only);
  ep->ContextRecord->EFlags |= 0x100;  // trap after this one instruction
  return EXCEPTION_CONTINUE_EXECUTION;
}

inline void Arm(uint32_t guest, uint32_t size, bool writes_only) {
  Watch w{uintptr_t(g_base + guest), uintptr_t(g_base + guest + size), guest, writes_only};
  {
    std::lock_guard lock(g_watch_mutex);
    g_watches.push_back(w);
  }
  g_watching.store(true);
  for (uintptr_t p = w.lo & ~uintptr_t(4095); p < w.hi; p += 4096) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(p), 4096, WatchProt(writes_only), &old);
  }
}

inline void DisarmAll(bool writes_only) {
  std::vector<Watch> ws;
  std::lock_guard lock(g_watch_mutex);
  std::vector<Watch> keep;
  for (const Watch& w : g_watches)
    (w.writes_only == writes_only ? ws : keep).push_back(w);
  g_watches.swap(keep);
  for (const Watch& w : ws)
    for (uintptr_t p = w.lo & ~uintptr_t(4095); p < w.hi; p += 4096) {
      DWORD old;
      VirtualProtect(reinterpret_cast<void*>(p), 4096, PAGE_READWRITE, &old);
    }
  g_watching.store(!g_watches.empty());
}

// Logs each (RIP, offset) once, with how often it was seen; RIPs relative to the exe for the pdb.
inline void ReportHits(int table, const char* what) {
  std::vector<std::tuple<uint32_t, uintptr_t, uint32_t>> uniq;  // offset, rip, count
  for (Hit& h : g_hits[table])
    if (const uintptr_t rip = h.rip.load())
      uniq.emplace_back(h.offset.load(), rip, h.count.load());
  std::sort(uniq.begin(), uniq.end());
  const uintptr_t exe = uintptr_t(GetModuleHandleW(nullptr));
  REXLOG_INFO("[probe] {}: {} accesses, {} distinct (exe base 0x{:X})", what, g_hit_count[table].load(),
              uniq.size(), exe);
  for (auto& [offset, rip, count] : uniq)
    REXLOG_INFO("[probe]   rva 0x{:X} offset {} x{}", rip - exe, offset, count);
}

// ---- heap walk ------------------------------------------------------------------------------
template <typename F>
inline void ForEachHeapPage(F&& f) {
  // Heaps, the image's data (globals), and physical memory: everything but the code.
  for (auto [lo, hi] : {std::pair<uint64_t, uint64_t>{0x40000000ull, 0x82000000ull},
                        std::pair<uint64_t, uint64_t>{0x83000000ull, 0xC0000000ull}}) {
    uint64_t g = lo;
    while (g < hi) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(g_base + g, &mbi, sizeof(mbi)))
        break;
      const uint64_t start = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - g_base);
      const uint64_t end = std::min<uint64_t>(start + mbi.RegionSize, hi);
      if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY)) &&
          !(mbi.Protect & PAGE_GUARD))
        f(uint32_t(std::max<uint64_t>(start, g)), uint32_t(end - std::max<uint64_t>(start, g)));
      g = end;
    }
  }
}

inline float LoadF(uint32_t guest) {
  uint32_t v;
  std::memcpy(&v, g_base + guest, 4);
  v = _byteswap_ulong(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// ---- timer search ---------------------------------------------------------------------------
inline int32_t LoadI(uint32_t guest) {
  uint32_t v;
  std::memcpy(&v, g_base + guest, 4);
  return int32_t(_byteswap_ulong(v));
}

// One try: values that move the same way every second for 12 s (a clock counting down, or the
// time gone counting up; floats in seconds, or ints such as frames). True when some survived.
inline bool TimerTry(int attempt) {
  using namespace std::chrono;
  struct Cand {
    uint32_t guest;
    bool is_int;
    int8_t sign;
    float first, last;
  };
  auto read = [](const Cand& c) { return c.is_int ? float(LoadI(c.guest)) : LoadF(c.guest); };
  // Snapshot everything, wait 1 s, and keep only the words that changed and look like a time:
  // far fewer than all the words in range.
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> snap;
  ForEachHeapPage([&](uint32_t g, uint32_t size) {
    std::vector<uint32_t> words(size / 4);
    std::memcpy(words.data(), g_base + g, words.size() * 4);
    snap.emplace_back(g, std::move(words));
  });
  const auto t0 = steady_clock::now();  // when the "first" values were read
  std::this_thread::sleep_for(seconds(1));
  std::vector<Cand> cands;
  for (auto& [g, words] : snap) {
    for (uint32_t k = 0; k < words.size(); ++k) {
      const uint32_t addr = g + k * 4;
      uint32_t now_raw;
      std::memcpy(&now_raw, g_base + addr, 4);
      if (now_raw == words[k])
        continue;
      const uint32_t was = _byteswap_ulong(words[k]);
      float f;
      std::memcpy(&f, &was, 4);
      if (std::isfinite(f) && f >= 0.5f && f <= 400000.0f)
        cands.push_back({addr, false, 0, f, f});
      const int32_t i = int32_t(was);
      if (i >= 30 && i <= 400000)
        cands.push_back({addr, true, 0, float(i), float(i)});
    }
    std::vector<uint32_t>().swap(words);
  }
  snap.clear();
  REXLOG_INFO("[probe] timer try {}: {} candidates", attempt, cands.size());
  for (int round = 0; round < 12; ++round) {
    std::this_thread::sleep_for(seconds(1));
    std::vector<Cand> keep;
    keep.reserve(cands.size() / 8 + 16);
    for (Cand c : cands) {
      const float v = read(c);
      const float d = v - c.last;  // over ~1 s
      const float mag = std::fabs(d);
      const bool moved = std::isfinite(v) && (c.is_int ? (mag >= 1.0f && mag <= 5000.0f)
                                                       : (mag >= 0.2f && mag <= 5000.0f));
      const int8_t sign = d > 0 ? 1 : -1;
      if (moved && (c.sign == 0 || c.sign == sign)) {
        c.sign = sign;
        c.last = v;
        keep.push_back(c);
      }
    }
    cands.swap(keep);
    REXLOG_INFO("[probe] timer try {}: {} still moving after {} s", attempt, cands.size(), round + 1);
    if (cands.size() <= 40)
      for (const Cand& c : cands)
        REXLOG_INFO("[probe]   0x{:08X} {} {:.3f} -> {:.3f}", c.guest, c.is_int ? "int" : "float",
                    c.first, c.last);
    if (cands.empty())
      return false;
  }
  const double secs = duration<double>(steady_clock::now() - t0).count();
  // Classes: anything counting down (a round clock), then things counting up at about twice a
  // real second (seconds, frames at 30/60, or ms, each x1.5..x3).
  auto rate = [&](const Cand& c) { return (c.last - c.first) / secs; };
  auto fast = [](double r, double unit) { return r >= unit * 1.5 && r <= unit * 3.0; };
  std::vector<Cand> down, up;
  for (const Cand& c : cands) {
    const double r = rate(c);
    if (r < 0)
      down.push_back(c);
    else if (fast(r, 1) || fast(r, 30) || fast(r, 60) || fast(r, 1000))
      up.push_back(c);
  }
  auto by_rate = [&](const Cand& a, const Cand& b) { return rate(a) < rate(b); };
  std::stable_sort(down.begin(), down.end(), by_rate);
  std::stable_sort(up.begin(), up.end(), by_rate);
  REXLOG_INFO("[probe] timer: {} moving steadily, {} counting down, {} counting up fast",
              cands.size(), down.size(), up.size());
  for (auto* list : {&down, &up})
    for (size_t i = 0; i < list->size() && i < 150; ++i)
      REXLOG_INFO("[probe] timer cand {} 0x{:08X} {}: {:.3f} -> {:.3f}, {:.3f} per real s",
                  list == &down ? "down" : "up", (*list)[i].guest,
                  (*list)[i].is_int ? "int" : "float", (*list)[i].first, (*list)[i].last,
                  rate((*list)[i]));
  // Seconds-like ones first (|rate| 1.5..3), for the write watch.
  std::vector<Cand> odd;
  for (auto* list : {&down, &up})
    for (const Cand& c : *list)
      if (fast(std::fabs(rate(c)), 1)) odd.push_back(c);
  for (const Cand& c : down)
    if (!fast(std::fabs(rate(c)), 1)) odd.push_back(c);
  // The code writing the first odd ones, for 3 s.
  ClearHits(1);
  for (size_t i = 0; i < odd.size() && i < 8; ++i)
    Arm(odd[i].guest, 4, /*writes_only=*/true);
  std::this_thread::sleep_for(seconds(3));
  DisarmAll(true);
  ReportHits(1, "timer writers");
  return true;
}

inline void TimerProbe() {
  using namespace std::chrono;
  for (int attempt = 1; attempt <= 20; ++attempt) {
    while (!Ufc3InMatch()) std::this_thread::sleep_for(seconds(1));
    std::this_thread::sleep_for(seconds(attempt == 1 ? 40 : 5));
    if (TimerTry(attempt))
      return;
  }
}

// ---- pattern readers ------------------------------------------------------------------------
inline std::vector<uint8_t> ParseHex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size();) {
    if (s[i] == ' ') { ++i; continue; }
    out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
    i += 2;
  }
  return out;
}

inline void PatternProbe(std::vector<uint8_t> pat) {
  using namespace std::chrono;
  std::vector<uint32_t> found;
  const auto deadline = steady_clock::now() + minutes(30);
  while (found.empty() && steady_clock::now() < deadline) {
    ForEachHeapPage([&](uint32_t g, uint32_t size) {
      const uint8_t* p = g_base + g;
      const uint8_t* end = p + size;
      while (found.size() < 4) {
        p = std::search(p, end, pat.begin(), pat.end());
        if (p == end) break;
        found.push_back(uint32_t(p - g_base));
        p += pat.size();
      }
    });
    if (found.empty())
      std::this_thread::sleep_for(seconds(2));
  }
  if (found.empty()) {
    REXLOG_INFO("[probe] pattern: not found in 30 min");
    return;
  }
  for (uint32_t g : found) {
    REXLOG_INFO("[probe] pattern at 0x{:08X}", g);
    Arm(g, 4096, /*writes_only=*/false);
  }
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(seconds(60));
    ReportHits(0, "pattern readers (so far)");
  }
  DisarmAll(false);
  ReportHits(0, "pattern readers");
}

// ---- writers of given addresses --------------------------------------------------------------
inline uint32_t PeekBE(uint32_t g) {
  uint32_t v;
  std::memcpy(&v, g_base + g, 4);
  return _byteswap_ulong(v);
}

inline void WatchProbe(std::vector<uint32_t> addrs) {
  using namespace std::chrono;
  for (int round = 1; round <= 3; ++round) {
    while (!Ufc3InMatch()) std::this_thread::sleep_for(seconds(1));
    std::this_thread::sleep_for(seconds(round == 1 ? 20 : 10));
    for (uint32_t a : addrs) {
      const uint32_t v = PeekBE(a);
      float f;
      std::memcpy(&f, &v, 4);
      REXLOG_INFO("[probe] watch 0x{:08X} = {} (0x{:08X}, float {})", a, int32_t(v), v, f);
    }
    ClearHits(1);
    for (uint32_t a : addrs)
      Arm(a, 4, /*writes_only=*/true);
    std::this_thread::sleep_for(seconds(4));
    DisarmAll(true);
    ReportHits(1, "watch writers");
  }
}

inline void Start(uint8_t* base) {
  g_base = base;
  const bool timer = REXCVAR_GET(ufc3_probe_timer);
  const std::string pattern = REXCVAR_GET(ufc3_probe_pattern);
  const std::string watch = REXCVAR_GET(ufc3_probe_watch);
  if (!timer && pattern.empty() && watch.empty())
    return;
  AddVectoredExceptionHandler(1, Handler);
  if (timer)
    std::thread(TimerProbe).detach();
  if (!pattern.empty())
    std::thread(PatternProbe, ParseHex(pattern)).detach();
  if (!watch.empty()) {
    std::vector<uint32_t> addrs;
    size_t i = 0;
    while (i < watch.size()) {
      size_t j = watch.find(',', i);
      if (j == std::string::npos) j = watch.size();
      if (j > i) addrs.push_back(uint32_t(std::stoul(watch.substr(i, j - i), nullptr, 16)));
      i = j + 1;
    }
    std::thread(WatchProbe, addrs).detach();
  }
  REXLOG_INFO("[probe] on: timer {}, pattern '{}', watch '{}'", timer, pattern, watch);
}

}  // namespace ufc3_probe

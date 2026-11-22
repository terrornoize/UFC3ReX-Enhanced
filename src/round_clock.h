// ufc3 - opt-in: the round clock in real seconds.
//
// How the stock clock runs (found 2026-11-22 with the run-time probe, src/probe.h):
//   - The game keeps the time gone in a round in two floats of the match object (match =
//     [0x83574A08]), both advanced once a frame by the same fixed step:
//         elapsed += [0x833C80F0] * [0x8333A044]        (1.0 a frame at 60 fps; [0x8333A044] = slow-mo)
//       match+15832, by sub_82B7E270: the round's own clock (announcer calls, end of the round),
//                     read back by sub_82B7EFE8;
//       match+15848, by the match update (sub_82B868E0; sub_82E65F30 is a twin): the HUD clock,
//                     read back by sub_82B7F0F0 (-> sub_82D38A40 / sub_82D13CA0 -> HUD digits).
//     sub_82B7E270 also moves match+15836 the same way.
//   - Seconds left = [match+736] - int(int(elapsed) * [0x8333A480]) / int([0x833C80F8]) = elapsed / 30:
//     a clock made for 30 frames a second, run at 60, so a 5:00 round was over in 2:30 of real time.
//
// With ufc3_real_time_clock = true, the step each frame is replaced with the real time since the
// previous step of that clock (in the same units, times the stock slow-mo factor), so one clock
// second is one real second. Frames where the stock code does not move a clock (pauses, replays,
// between rounds) are left alone, and a long gap is capped, so pausing does not eat time.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(ufc3_real_time_clock, false, "ufc3",
                    "Round clock in real seconds (the stock clock runs about twice as fast)");

REX_EXTERN(__imp__sub_82B7E270);
REX_EXTERN(__imp__sub_82E65F30);
REX_EXTERN(__imp__sub_82B868E0);

namespace ufc3_round_clock {

using Clock = std::chrono::steady_clock;

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}
inline float LoadF(uint8_t* base, uint32_t addr) {
  const uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void StoreF(uint8_t* base, uint32_t addr, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = _byteswap_ulong(v);
  std::memcpy(base + addr, &v, 4);
}

constexpr uint32_t kRoundElapsed = 15832, kRoundElapsed2 = 15836, kHudElapsed = 15848;
constexpr uint32_t kStep = 0x833C80F0, kSlowMo = 0x8333A044;  // elapsed += step * slow-mo
constexpr uint32_t kToSec = 0x8333A480, kPerSec = 0x833C80F8;  // secs = int(e * toSec) / perSec

// One clock field: when the stock code moved it this frame, move it by real time instead.
// Returns the factor applied to the stock step (1 = left alone).
struct Pace {
  Clock::time_point last{};
};
inline float Repace(uint8_t* base, uint32_t addr, float before, Pace& pace) {
  const auto now = Clock::now();
  const float after = LoadF(base, addr);
  const float step = after - before;
  if (!(step > 0.0f) || step > 10.0f) {  // not running this frame (or reset)
    pace.last = {};
    return 1.0f;
  }
  const float to_sec = LoadF(base, kToSec), per_sec = LoadF(base, kPerSec);
  if (!(to_sec > 0.0f) || !(per_sec > 0.0f))
    return 1.0f;
  if (pace.last == Clock::time_point{}) {
    pace.last = now;  // first running frame: keep the stock step once
    return 1.0f;
  }
  double dt = std::chrono::duration<double>(now - pace.last).count();
  pace.last = now;
  if (dt > 0.1)
    dt = 0.1;  // a hitch or a pause in between: don't jump ahead
  float slow = LoadF(base, kSlowMo);
  if (!(slow > 0.0f) || slow > 1.0f)
    slow = 1.0f;
  const float real_step = float(dt) * (per_sec / to_sec) * slow;
  StoreF(base, addr, before + real_step);
  return real_step / step;
}

inline Pace g_round, g_hud;
inline bool g_logged = false;

inline bool On(uint32_t match) { return match && REXCVAR_GET(ufc3_real_time_clock); }

}  // namespace ufc3_round_clock

// The round's own clock.
extern "C" REX_FUNC(sub_82B7E270) {
  namespace rc = ufc3_round_clock;
  const uint32_t match = ctx.r3.u32;
  if (!rc::On(match)) {
    __imp__sub_82B7E270(ctx, base);
    return;
  }
  const float before = rc::LoadF(base, match + rc::kRoundElapsed);
  const float before2 = rc::LoadF(base, match + rc::kRoundElapsed2);
  __imp__sub_82B7E270(ctx, base);
  const float step = rc::LoadF(base, match + rc::kRoundElapsed) - before;
  const float k = rc::Repace(base, match + rc::kRoundElapsed, before, rc::g_round);
  if (!rc::g_logged && step > 0.0f) {
    rc::g_logged = true;
    REXLOG_INFO("[clock] real-time round clock on: stock step {} a frame, {} units a second",
                step, rc::LoadF(base, rc::kPerSec) / rc::LoadF(base, rc::kToSec));
  }
  // The second timer moves with the first: keep them in step.
  const float after2 = rc::LoadF(base, match + rc::kRoundElapsed2);
  if (k != 1.0f && after2 > before2)
    rc::StoreF(base, match + rc::kRoundElapsed2, before2 + (after2 - before2) * k);
}

// The match update (two copies in the game): moves the clock the HUD shows.
template <void (*Orig)(PPCContext&, uint8_t*)>
inline void UfcHudClockHook(PPCContext& ctx, uint8_t* base) {
  namespace rc = ufc3_round_clock;
  const uint32_t match = ctx.r3.u32;
  if (!rc::On(match)) {
    Orig(ctx, base);
    return;
  }
  const float before = rc::LoadF(base, match + rc::kHudElapsed);
  Orig(ctx, base);
  rc::Repace(base, match + rc::kHudElapsed, before, rc::g_hud);
}

extern "C" REX_FUNC(sub_82B868E0) { UfcHudClockHook<__imp__sub_82B868E0>(ctx, base); }
extern "C" REX_FUNC(sub_82E65F30) { UfcHudClockHook<__imp__sub_82E65F30>(ctx, base); }

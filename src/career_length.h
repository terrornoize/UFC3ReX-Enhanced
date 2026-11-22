// ufc3 - opt-in: longer careers (96 or 192 fights instead of 48).
//
// How a career ends (found 2026-11-22 with the run-time probe, src/probe.h):
//   - The career manager is G = [0x8335D4A0]; G+220 points at careermode/career_param.bin as loaded
//     (P), where file offset F is read at P+F+4 (P+4 = the file's first word, 9999). The career
//     itself is C = [G+8]: C+172 = year (from 0), C+168 = fight within the year (from 0).
//   - File offset 16 = 12 (years), offset 20 = 4 (fights a year). sub_82304878 is "is the career
//     over": year == years-1 && fight >= fights-1, or year >= years. 12 x 4 = the 48-fight limit.
//     The same two fields are read by sub_822D7770, sub_822E1430, sub_82361790 and sub_82A2DF48.
//   - The save stores year and fight-in-year as bytes (sub_822E1580), so 48 years still fit.
//
// With ufc3_career_fights = 96 or 192, the years field of the loaded table is set to fights / 4 (24 or
// 48) before any of those functions reads it, so every check agrees. 48 = the stock game. Nothing in
// the save changes format; a career saved past year 12 just keeps going.

#pragma once

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(ufc3_career_fights, 48, "ufc3",
                     "Career mode: fights before the forced retirement (48 = stock, 96, 192)");

REX_EXTERN(__imp__sub_82304878);
REX_EXTERN(__imp__sub_822D7770);
REX_EXTERN(__imp__sub_822E1430);
REX_EXTERN(__imp__sub_82361790);
REX_EXTERN(__imp__sub_82A2DF48);
REX_EXTERN(__imp__sub_822F2560);

namespace ufc3_career_length {

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}
inline void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
  const uint32_t v = _byteswap_ulong(value);
  std::memcpy(base + addr, &v, 4);
}

constexpr uint32_t kCareerManager = 0x8335D4A0;

inline void Apply(uint8_t* base) {
  const int32_t fights = REXCVAR_GET(ufc3_career_fights);
  if (fights != 96 && fights != 192)
    return;
  const uint32_t g = Load32(base, kCareerManager);
  if (!g)
    return;
  const uint32_t p = Load32(base, g + 220);
  // career_param.bin as the game ships it: first word 9999, 4 fights a year.
  if (!p || Load32(base, p + 4) != 9999 || Load32(base, p + 24) != 4)
    return;
  const uint32_t years = uint32_t(fights) / 4;
  const uint32_t cur = Load32(base, p + 20);
  if (cur == years || (cur != 12 && cur != 24 && cur != 48))
    return;
  Store32(base, p + 20, years);
  REXLOG_INFO("[career] career length: {} years x 4 fights = {} fights (was {} years)", years,
              fights, cur);
}

}  // namespace ufc3_career_length

#define UFC3_CAREER_LENGTH_HOOK(fn)                       \
  extern "C" REX_FUNC(fn) {                               \
    ufc3_career_length::Apply(base);                      \
    __imp__##fn(ctx, base);                               \
  }

UFC3_CAREER_LENGTH_HOOK(sub_82304878)  // is the career over
UFC3_CAREER_LENGTH_HOOK(sub_822D7770)
UFC3_CAREER_LENGTH_HOOK(sub_822E1430)
UFC3_CAREER_LENGTH_HOOK(sub_82361790)
UFC3_CAREER_LENGTH_HOOK(sub_82A2DF48)
UFC3_CAREER_LENGTH_HOOK(sub_822F2560)  // reads career_param every frame of the career menus

#undef UFC3_CAREER_LENGTH_HOOK

// ufc3 - ReXGlue Recompiled Project

#include "generated/default/ufc3_init.h"

#include "ufc3_app.h"
#include "first_run_save.h"
#include "import_trace.h"
#include "save_guard.h"
#include "bare_knuckle.h"
#include "blood.h"
#include "cuts.h"
#include "perf_probe.h"
#include "cutscene_60.h"
#include "legacy_roster.h"
#include "heap_trace.h"
#include "physical_heap.h"
#include "fighter_music.h"
#include "unlock_all.h"
#include "caf_points.h"
#include "career_length.h"
#include "round_clock.h"
#include "ultimate_unlock.h"
#include "pride_range.h"
#include "dialogue_trace.h"
#include "dlc_sound_banks.h"
#include "position_guard.h"
#include "acce_trace.h"
#include "flare_trace.h"
#include "skip_trace.h"
#include "steady_skin_light.h"
#include "venues.h"
#include "test_pad.h"

REX_DEFINE_APP(ufc3, Ufc3App::Create)

// WS-1 (native renderer, ultrawide): the wide picture is only for the match itself; menus keep the
// game's own 16:9. True while the match-flow dispatcher runs (bare_knuckle.h).
bool Ufc3InMatch() { return ufc3_bk::InMatch(); }

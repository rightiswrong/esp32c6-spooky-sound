// =============================================================================
//  Build settings the shared effects / sound code expects (the XIAO keeps its
//  own in firmware/XIAO_SpookySound/config.h). Only what that code uses.
// =============================================================================
#pragma once

// Effects.h sizes its per-pixel arrays MAX_CHAIN + 1; the strip renders 6.
#define MAX_CHAIN            24

// Remote "continuous" cues (fire level, rumble) fade if not refreshed.
#define SOUND_REMOTE_HOLD_MS 4000

// Effects follow the soundtrack this long after the last sound.
#define SOUND_LISTEN_MS      20000

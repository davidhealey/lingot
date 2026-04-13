/*
 * lingot-lv2-audio-stubs.c - Stub audio functions for the self-contained LV2 build.
 *
 * lingot-config.c references lingot_audio_system_find_by_name() and
 * lingot_audio_system_get_count() inside lingot_config_restore_default_values().
 * The LV2 plugin never calls lingot_config_restore_default_values(), so these
 * are never reached at runtime, but the linker still needs the symbols.
 *
 * This file satisfies those link-time dependencies without pulling in the full
 * audio backend code (ALSA, JACK, OSS, PulseAudio).
 */

#include "../src/lingot-audio.h"

int lingot_audio_system_find_by_name(const char *audio_system_name) {
    (void)audio_system_name;
    return -1;
}

int lingot_audio_system_get_count(void) {
    return 0;
}

/*
 * lingot_lv2.c - LV2 plugin for the Lingot musical instrument tuner.
 *
 * Copyright (C) 2004-2020  Iban Cereijo.
 * Copyright (C) 2004-2008  Jairo Chapela.
 *
 * This file is part of lingot.
 *
 * lingot is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * lingot is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with lingot; if not, write to the Free Software Foundation,
 * Inc. 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * ---------------------------------------------------------------------------
 *
 * This LV2 plugin receives a mono audio input and outputs pitch detection
 * results using Lingot's core DSP algorithms:
 *
 *   Port 0 - Audio In  (input, audio)
 *   Port 1 - Frequency (output, control, Hz)
 *   Port 2 - MIDI Note (output, control, 0-127, -1 = not detected)
 *   Port 3 - Cents     (output, control, -50..+50)
 *   Port 4 - Active    (output, control, 1 = note locked, 0 = silent)
 *
 * The plugin reuses liblingot's FFT, filter, and signal-processing routines.
 * It implements its own instance-local frequency locker so that multiple
 * simultaneous instances do not share static state.
 */

#ifdef HAVE_CONFIG_H
#  include "../config.h"
#endif

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>

#include "../src/lingot-config-scale.h"
#include "../src/lingot-config.h"
#include "../src/lingot-defs.h"
#include "../src/lingot-fft.h"
#include "../src/lingot-filter.h"
#include "../src/lingot-signal.h"

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define LINGOT_LV2_URI "http://nongnu.org/lingot/lv2/tuner"

/* Tuner analysis parameters */
#define LV2_FFT_SIZE          512u
#define LV2_TEMPORAL_WINDOW   0.35   /* seconds */
#define LV2_CALCULATION_RATE  15.0   /* Hz - how often to run the detector  */
#define LV2_MIN_FREQUENCY     65.0   /* Hz  (C2 - covers bass instruments)  */
#define LV2_MAX_FREQUENCY     1500.0 /* Hz  (covers most melodic instruments)*/
#define LV2_MIN_OVERALL_SNR   20.0   /* dB                                  */
#define LV2_PEAK_NUMBER       8u
#define LV2_MAX_NR_ITER       10u

/* -------------------------------------------------------------------------
 * Port indices
 * ---------------------------------------------------------------------- */

typedef enum {
    PORT_AUDIO_IN   = 0,
    PORT_FREQ_OUT   = 1,
    PORT_NOTE_OUT   = 2,
    PORT_CENTS_OUT  = 3,
    PORT_ACTIVE_OUT = 4,
    PORT_AUDIO_OUT  = 5,
} LingotPortIndex;

/* -------------------------------------------------------------------------
 * Per-instance frequency locker
 *
 * Mirrors the logic of lingot_signal_frequency_locker() but stores all
 * state in the instance struct so multiple plugin instances are independent.
 * ---------------------------------------------------------------------- */

typedef struct {
    int    locked;
    double current_frequency;
    int    hits_counter;
    int    rehits_counter;
    int    rehits_up_counter;
    double old_multiplier;
    double old_multiplier2;
} LingotFreqLocker;

static void freq_locker_init(LingotFreqLocker *lk) {
    lk->locked            = 0;
    lk->current_frequency = 0.0;
    lk->hits_counter      = 0;
    lk->rehits_counter    = 0;
    lk->rehits_up_counter = 0;
    lk->old_multiplier    = 0.0;
    lk->old_multiplier2   = 0.0;
}

static double freq_locker_step(LingotFreqLocker *lk,
                                double freq,
                                double min_frequency) {
    static const int nhits_to_lock      = 4;
    static const int nhits_to_unlock    = 5;
    static const int nhits_to_relock    = 6;
    static const int nhits_to_relock_up = 8;

    double multiplier  = 0.0;
    double multiplier2 = 0.0;
    int    fail        = 0;
    double result      = 0.0;

    int consistent = lingot_signal_frequencies_related(
        freq, lk->current_frequency, min_frequency,
        &multiplier, &multiplier2);

    if (!lk->locked) {
        if (freq > 0.0 && lk->current_frequency == 0.0) {
            consistent  = 1;
            multiplier  = 1.0;
            multiplier2 = 1.0;
        }

        if (consistent && (multiplier == 1.0) && (multiplier2 == 1.0)) {
            lk->current_frequency = freq * multiplier;
            if (++lk->hits_counter >= nhits_to_lock) {
                lk->locked       = 1;
                lk->hits_counter = 0;
            }
        } else {
            lk->hits_counter      = 0;
            lk->current_frequency = 0.0;
        }
    } else {
        if (consistent) {
            if (fabs(multiplier2 - 1.0) < 1e-5) {
                result                = freq * multiplier;
                lk->current_frequency = result;
                lk->rehits_counter    = 0;

                if (fabs(multiplier - 1.0) > 1e-5) {
                    if (fabs(multiplier - lk->old_multiplier) < 1e-5) {
                        if (++lk->rehits_up_counter >= nhits_to_relock_up) {
                            result                = freq;
                            lk->current_frequency = result;
                            lk->rehits_up_counter = 0;
                            fail                  = 0;
                        }
                    } else {
                        lk->rehits_up_counter = 0;
                    }
                } else {
                    lk->rehits_up_counter = 0;
                }
            } else {
                lk->rehits_up_counter = 0;
                if (fabs(multiplier2 - 0.5) < 1e-5) {
                    lk->hits_counter--;
                }
                fail = 1;
                if (freq * multiplier >= min_frequency) {
                    if (fabs(multiplier2 - lk->old_multiplier2) < 1e-5) {
                        if (++lk->rehits_counter >= nhits_to_relock) {
                            result                = freq * multiplier;
                            lk->current_frequency = result;
                            lk->rehits_counter    = 0;
                            fail                  = 0;
                        }
                    }
                }
            }
        } else {
            fail = 1;
        }

        if (fail) {
            result = lk->current_frequency;
            if (++lk->hits_counter >= nhits_to_unlock) {
                lk->current_frequency = 0.0;
                lk->locked            = 0;
                lk->hits_counter      = 0;
                result                = 0.0;
            }
        } else {
            lk->hits_counter = 0;
        }
    }

    lk->old_multiplier  = multiplier;
    lk->old_multiplier2 = multiplier2;
    return result;
}

/* -------------------------------------------------------------------------
 * Plugin instance state
 * ---------------------------------------------------------------------- */

typedef struct {
    /* --- LV2 port pointers -------------------------------------------- */
    const float *audio_in;
    float       *audio_out;
    float       *port_freq;
    float       *port_note;
    float       *port_cents;
    float       *port_active;

    /* --- Lingot configuration ----------------------------------------- */
    lingot_config_t config;

    /* --- DSP buffers (all pre-allocated in instantiate()) -------------- */
    double *temporal_buffer;          /* decimated audio ring buffer        */
    double *windowed_fft_buffer;      /* windowed slice for FFT             */
    double *windowed_temporal_buffer; /* windowed full temporal buf (NR p2) */
    double *hamming_window_fft;       /* precomputed window (fft_size)      */
    double *hamming_window_temporal;  /* precomputed window (temporal_size) */
    double *SPL;                      /* spectral power level (fft_size/2)  */
    double *noise_level;              /* noise floor   (fft_size/2)         */
    double *decimation_buf;           /* temp storage for one block's decim */

    /* --- Lingot DSP objects ------------------------------------------- */
    lingot_fft_plan_t fftplan;
    lingot_filter_t   antialiasing_filter;
    int               has_antialiasing_filter;

    /* --- Runtime state ------------------------------------------------- */
    double       current_freq;         /* latest locked frequency (Hz)      */
    unsigned int decimation_counter;   /* counts input samples mod oversampl */

    /* Computation timing (in input-rate samples) */
    unsigned int samples_since_compute;
    unsigned int samples_per_compute;

    /* Per-instance frequency locker */
    LingotFreqLocker locker;
} LingotLV2;

/* -------------------------------------------------------------------------
 * Frequency detection pipeline
 *
 * Called every (1 / LV2_CALCULATION_RATE) seconds (measured in input-rate
 * samples).  Mirrors lingot_core_compute_fundamental_fequency() but runs
 * entirely in the calling thread with no locks.
 * ---------------------------------------------------------------------- */

static void lingot_lv2_compute(LingotLV2 *plugin) {
    const lingot_config_t *conf  = &plugin->config;
    const unsigned int fft_size  = conf->fft_size;
    const unsigned int temp_size = conf->temporal_buffer_size;
    const unsigned int spd_size  = fft_size / 2u;

    /* Hz per FFT bin */
    const double index2f = (double)conf->sample_rate /
                           ((double)conf->oversampling * (double)fft_size);

    /* ----------------------------------------------------------------
     * 1.  Window the last fft_size decimated samples for the FFT
     * -------------------------------------------------------------- */
    if (conf->window_type != NONE && plugin->hamming_window_fft) {
        for (unsigned int i = 0; i < fft_size; i++) {
            plugin->windowed_fft_buffer[i] =
                plugin->temporal_buffer[temp_size - fft_size + i]
                * plugin->hamming_window_fft[i];
        }
    } else {
        memcpy(plugin->windowed_fft_buffer,
               &plugin->temporal_buffer[temp_size - fft_size],
               fft_size * sizeof(double));
    }

    /* ----------------------------------------------------------------
     * 2.  FFT -> Spectral Power Distribution (SPD)
     * -------------------------------------------------------------- */
    lingot_fft_compute_dft_and_spd(&plugin->fftplan, plugin->SPL, spd_size);

    static const double MIN_SPL = -200.0;
    for (unsigned int i = 0; i < spd_size; i++) {
        plugin->SPL[i] = 10.0 * log10(plugin->SPL[i]);
        if (plugin->SPL[i] < MIN_SPL) plugin->SPL[i] = MIN_SPL;
    }

    /* ----------------------------------------------------------------
     * 3.  Subtract noise floor
     * -------------------------------------------------------------- */
    double noise_bw_hz      = 150.0; /* Hz */
    unsigned int noise_bins = (unsigned int)ceil(
        noise_bw_hz * (double)fft_size * (double)conf->oversampling
        / (double)conf->sample_rate);

    lingot_signal_compute_noise_level(plugin->SPL, (int)spd_size,
                                      (int)noise_bins,
                                      plugin->noise_level);
    for (unsigned int i = 0; i < spd_size; i++) {
        plugin->SPL[i] -= plugin->noise_level[i];
    }

    /* ----------------------------------------------------------------
     * 4.  Fundamental frequency estimation
     * -------------------------------------------------------------- */
    unsigned int lowest_index = (unsigned int)ceil(
        conf->internal_min_frequency
        * ((double)conf->oversampling / (double)conf->sample_rate)
        * (double)fft_size);
    unsigned int highest_index = (unsigned int)ceil(0.95 * (double)spd_size);

    short divisor = 1;
    double f0 = lingot_signal_estimate_fundamental_frequency(
        plugin->SPL,
        0.5 * plugin->current_freq,
        plugin->fftplan.fft_out,
        spd_size,
        conf->peak_number,
        lowest_index,
        highest_index,
        (unsigned short)conf->peak_half_width,
        index2f,
        conf->min_SNR,
        conf->min_overall_SNR,
        conf->internal_min_frequency,
        &divisor);

    /* ----------------------------------------------------------------
     * 5a. Newton-Raphson pass 1: refine on windowed FFT buffer
     *     (windowed_fft_buffer is still valid from step 1)
     * -------------------------------------------------------------- */
    double w = (f0 == 0.0)
        ? 0.0
        : 2.0 * M_PI * f0 * (double)conf->oversampling / (double)conf->sample_rate;

    if (w != 0.0) {
        double wk    = -1.0e5;
        double wkm1  = w;
        double d0    = 0.0, d0_old = 0.0, d1 = 0.0, d2 = 0.0;

        for (unsigned int k = 0;
             (k < conf->max_nr_iter) && (fabs(wk - wkm1) > 1.0e-4); k++) {
            wk     = wkm1;
            d0_old = d0;
            lingot_fft_spd_diffs_eval(plugin->windowed_fft_buffer,
                                      fft_size, wk, &d0, &d1, &d2);
            wkm1 = wk - d1 / d2;
            if (d0 < d0_old) { wkm1 = 0.0; break; }
        }

        /* ----------------------------------------------------------------
         * 5b. Newton-Raphson pass 2: refine on the full temporal window
         * -------------------------------------------------------------- */
        if (wkm1 > 0.0) {
            w = wkm1;

            /* Window the entire temporal buffer */
            if (conf->window_type != NONE && plugin->hamming_window_temporal) {
                for (unsigned int i = 0; i < temp_size; i++) {
                    plugin->windowed_temporal_buffer[i] =
                        plugin->temporal_buffer[i]
                        * plugin->hamming_window_temporal[i];
                }
            } else {
                memcpy(plugin->windowed_temporal_buffer,
                       plugin->temporal_buffer,
                       temp_size * sizeof(double));
            }

            wk = -1.0e5; wkm1 = w; d0 = 0.0;
            for (unsigned int k = 0;
                 (k <= 1) || ((k < conf->max_nr_iter)
                              && (fabs(wk - wkm1) > 1.0e-4)); k++) {
                wk     = wkm1;
                d0_old = d0;
                lingot_fft_spd_diffs_eval(plugin->windowed_temporal_buffer,
                                          temp_size, wk, &d0, &d1, &d2);
                wkm1 = wk - d1 / d2;
                if (d0 < d0_old) { wkm1 = 0.0; break; }
            }
            if (wkm1 > 0.0) w = wkm1;
        }
    }

    /* ----------------------------------------------------------------
     * 6.  Convert angular frequency to Hz and apply frequency locker
     * -------------------------------------------------------------- */
    double freq = (w == 0.0)
        ? 0.0
        : w * (double)conf->sample_rate
          / ((double)divisor * 2.0 * M_PI * (double)conf->oversampling);

    plugin->current_freq = freq_locker_step(&plugin->locker, freq,
                                             conf->internal_min_frequency);
}

/* -------------------------------------------------------------------------
 * LV2 callbacks
 * ---------------------------------------------------------------------- */

static LV2_Handle instantiate(const LV2_Descriptor   *descriptor,
                               double                  rate,
                               const char             *bundle_path,
                               const LV2_Feature *const *features) {
    (void)descriptor;
    (void)bundle_path;
    (void)features;

    LingotLV2 *plugin = (LingotLV2 *)calloc(1, sizeof(LingotLV2));
    if (!plugin) return NULL;

    /* --- Configure the Lingot DSP parameters -------------------------- */
    lingot_config_new(&plugin->config);
    lingot_config_scale_restore_default_values(&plugin->config.scale);

    lingot_config_t *conf = &plugin->config;
    conf->sample_rate              = (int)rate;
    conf->fft_size                 = LV2_FFT_SIZE;
    conf->temporal_window          = LV2_TEMPORAL_WINDOW;
    conf->calculation_rate         = LV2_CALCULATION_RATE;
    conf->min_frequency            = LV2_MIN_FREQUENCY;
    conf->max_frequency            = LV2_MAX_FREQUENCY;
    conf->min_overall_SNR          = LV2_MIN_OVERALL_SNR;
    conf->peak_number              = LV2_PEAK_NUMBER;
    conf->window_type              = HAMMING;
    conf->max_nr_iter              = LV2_MAX_NR_ITER;
    conf->root_frequency_error     = 0.0;
    conf->optimize_internal_parameters = 0;

    /* Derive oversampling, temporal_buffer_size, min_SNR, etc. */
    lingot_config_update_internal_params(conf);

    const unsigned int fft_size  = conf->fft_size;
    const unsigned int temp_size = conf->temporal_buffer_size;
    const unsigned int spd_size  = fft_size / 2u;

    /* --- Allocate DSP buffers ----------------------------------------- */
    plugin->temporal_buffer          = calloc(temp_size, sizeof(double));
    plugin->windowed_fft_buffer      = calloc(fft_size,  sizeof(double));
    plugin->windowed_temporal_buffer = calloc(temp_size, sizeof(double));
    plugin->SPL                      = calloc(spd_size,  sizeof(double));
    plugin->noise_level              = calloc(spd_size,  sizeof(double));
    /* One block's worth of decimated samples (worst-case ≤ temp_size) */
    plugin->decimation_buf           = malloc(temp_size  * sizeof(double));

    if (!plugin->temporal_buffer || !plugin->windowed_fft_buffer
        || !plugin->windowed_temporal_buffer || !plugin->SPL
        || !plugin->noise_level || !plugin->decimation_buf) {
        /* Allocation failure – free what we got and return NULL */
        free(plugin->temporal_buffer);
        free(plugin->windowed_fft_buffer);
        free(plugin->windowed_temporal_buffer);
        free(plugin->SPL);
        free(plugin->noise_level);
        free(plugin->decimation_buf);
        lingot_config_destroy(&plugin->config);
        free(plugin);
        return NULL;
    }

    if (conf->window_type != NONE) {
        plugin->hamming_window_fft = malloc(fft_size  * sizeof(double));
        plugin->hamming_window_temporal = malloc(temp_size * sizeof(double));
        if (!plugin->hamming_window_fft || !plugin->hamming_window_temporal) {
            free(plugin->hamming_window_fft);
            free(plugin->hamming_window_temporal);
            /* Fall back to no windowing */
            conf->window_type = NONE;
        } else {
            lingot_signal_window((int)fft_size,  plugin->hamming_window_fft,
                                 conf->window_type);
            lingot_signal_window((int)temp_size, plugin->hamming_window_temporal,
                                 conf->window_type);
        }
    }

    /* Create FFT plan; its input pointer is windowed_fft_buffer */
    lingot_fft_plan_create(&plugin->fftplan, plugin->windowed_fft_buffer,
                           fft_size);

    /* Anti-aliasing filter for decimation */
    plugin->has_antialiasing_filter = (conf->oversampling > 1);
    if (plugin->has_antialiasing_filter) {
        lingot_filter_cheby_design(&plugin->antialiasing_filter,
                                   8, 0.5, 0.9 / (double)conf->oversampling);
    }

    /* Compute how many input-rate samples between detector runs */
    plugin->samples_per_compute =
        (unsigned int)round((double)rate / conf->calculation_rate);
    plugin->samples_since_compute = 0;

    plugin->current_freq      = 0.0;
    plugin->decimation_counter = 0;
    freq_locker_init(&plugin->locker);

    return (LV2_Handle)plugin;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
    LingotLV2 *plugin = (LingotLV2 *)instance;
    switch ((LingotPortIndex)port) {
    case PORT_AUDIO_IN:   plugin->audio_in    = (const float *)data; break;
    case PORT_AUDIO_OUT:  plugin->audio_out   = (float *)data;       break;
    case PORT_FREQ_OUT:   plugin->port_freq   = (float *)data;       break;
    case PORT_NOTE_OUT:   plugin->port_note   = (float *)data;       break;
    case PORT_CENTS_OUT:  plugin->port_cents  = (float *)data;       break;
    case PORT_ACTIVE_OUT: plugin->port_active = (float *)data;       break;
    }
}

static void activate(LV2_Handle instance) {
    LingotLV2 *plugin = (LingotLV2 *)instance;
    const unsigned int temp_size = plugin->config.temporal_buffer_size;

    memset(plugin->temporal_buffer, 0, temp_size * sizeof(double));
    plugin->decimation_counter    = 0;
    plugin->samples_since_compute = 0;
    plugin->current_freq          = 0.0;
    freq_locker_init(&plugin->locker);

    if (plugin->has_antialiasing_filter) {
        lingot_filter_reset(&plugin->antialiasing_filter);
    }
}

static void run(LV2_Handle instance, uint32_t n_samples) {
    LingotLV2 *plugin = (LingotLV2 *)instance;
    if (!plugin->audio_in || n_samples == 0) return;

    const lingot_config_t *conf  = &plugin->config;
    const unsigned int temp_size = conf->temporal_buffer_size;
    const unsigned int oversampl = conf->oversampling;

    /* ----------------------------------------------------------------
     * Decimate the input block into decimation_buf.
     * Apply the anti-aliasing filter to every input sample but only
     * keep every oversampl-th one.  Cap at temp_size to guard against
     * pathologically large block sizes.
     * -------------------------------------------------------------- */
    unsigned int dec_count = 0;

    for (uint32_t i = 0; i < n_samples; i++) {
        double sample = (double)plugin->audio_in[i];

        if (plugin->has_antialiasing_filter) {
            sample = lingot_filter_filter_sample(
                &plugin->antialiasing_filter, sample);
        }

        if (plugin->decimation_counter == 0) {
            if (dec_count < temp_size) {
                plugin->decimation_buf[dec_count++] = sample;
            }
            /* If dec_count == temp_size the buffer is full; remaining
             * decimated samples are discarded.  This is only reachable
             * with abnormally large block sizes. */
        }

        if (++plugin->decimation_counter >= oversampl) {
            plugin->decimation_counter = 0;
        }
    }

    /* ----------------------------------------------------------------
     * Append decimated samples to the temporal ring buffer.
     * Shift out the oldest samples to make room for the new ones.
     * -------------------------------------------------------------- */
    if (dec_count > 0) {
        if (dec_count >= temp_size) {
            /* Rare: more new data than the entire buffer – replace all */
            memcpy(plugin->temporal_buffer,
                   plugin->decimation_buf + (dec_count - temp_size),
                   temp_size * sizeof(double));
        } else {
            memmove(plugin->temporal_buffer,
                    plugin->temporal_buffer + dec_count,
                    (temp_size - dec_count) * sizeof(double));
            memcpy(plugin->temporal_buffer + temp_size - dec_count,
                   plugin->decimation_buf,
                   dec_count * sizeof(double));
        }
    }

    /* ----------------------------------------------------------------
     * Trigger the detector at LV2_CALCULATION_RATE Hz
     * -------------------------------------------------------------- */
    plugin->samples_since_compute += n_samples;
    if (plugin->samples_since_compute >= plugin->samples_per_compute) {
        plugin->samples_since_compute = 0;
        lingot_lv2_compute(plugin);
    }

    /* ----------------------------------------------------------------
     * Write outputs
     * -------------------------------------------------------------- */
    double freq = plugin->current_freq;

    /* Pass audio through so the plugin can sit inline on a track */
    if (plugin->audio_out && plugin->audio_in)
        memcpy(plugin->audio_out, plugin->audio_in, n_samples * sizeof(float));

    if (plugin->port_freq)   *plugin->port_freq   = (float)freq;
    if (plugin->port_active) *plugin->port_active  = (freq > 0.0) ? 1.0f : 0.0f;

    if (freq > 0.0) {
        /* MIDI note number (equal temperament, A4 = 440 Hz) */
        double midi_f  = 69.0 + 12.0 * log2(freq / 440.0);
        int    midi    = (int)round(midi_f);
        if (midi < 0)   midi = 0;
        if (midi > 127) midi = 127;

        /* Cents deviation from the nearest semitone */
        double ref_freq = 440.0 * pow(2.0, (midi - 69) / 12.0);
        double cents    = 1200.0 * log2(freq / ref_freq);

        if (plugin->port_note)  *plugin->port_note  = (float)midi;
        if (plugin->port_cents) *plugin->port_cents = (float)cents;
    } else {
        if (plugin->port_note)  *plugin->port_note  = -1.0f;
        if (plugin->port_cents) *plugin->port_cents = 0.0f;
    }
}

static void deactivate(LV2_Handle instance) {
    (void)instance;
}

static void cleanup(LV2_Handle instance) {
    LingotLV2 *plugin = (LingotLV2 *)instance;
    if (!plugin) return;

    lingot_fft_plan_destroy(&plugin->fftplan);

    if (plugin->has_antialiasing_filter) {
        lingot_filter_destroy(&plugin->antialiasing_filter);
    }

    free(plugin->temporal_buffer);
    free(plugin->windowed_fft_buffer);
    free(plugin->windowed_temporal_buffer);
    free(plugin->hamming_window_fft);
    free(plugin->hamming_window_temporal);
    free(plugin->SPL);
    free(plugin->noise_level);
    free(plugin->decimation_buf);

    lingot_config_destroy(&plugin->config);
    free(plugin);
}

static const void *extension_data(const char *uri) {
    (void)uri;
    return NULL;
}

/* -------------------------------------------------------------------------
 * Plugin descriptor and entry point
 * ---------------------------------------------------------------------- */

static const LV2_Descriptor descriptor = {
    LINGOT_LV2_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data,
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor *lv2_descriptor(uint32_t index) {
    return (index == 0) ? &descriptor : NULL;
}

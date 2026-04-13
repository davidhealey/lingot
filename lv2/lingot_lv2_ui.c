/*
 * lingot_lv2_ui.c - X11/Cairo UI for the Lingot LV2 tuner plugin.
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
 * Horizontal LV2 X11UI for the Lingot tuner.  Uses raw X11 embedding and
 * Cairo for drawing — no GTK, no version conflicts with the host.
 *
 * The host embeds us by passing its parent window via ui:parent and calling
 * our idle() callback (lv2:extensionData ui:idleInterface) regularly so we
 * can process X events and redraw.
 *
 * Visual layout (360 × 80 px, four equal columns):
 *
 *   ┌──────────┬──────────┬────────────────┬──────────┐
 *   │  Note    │  MIDI    │   Frequency    │  Cents   │
 *   │   A4     │   69     │   440.0 Hz     │  +3.2 ct │
 *   └──────────┴──────────┴────────────────┴──────────┘
 *
 * Port indices (must match lingot_lv2.c):
 *   0  Audio In      (not displayed)
 *   1  Frequency Hz
 *   2  MIDI Note
 *   3  Cents Error
 *   4  Active
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo.h>
#include <cairo-xlib.h>

#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>

/* -------------------------------------------------------------------------
 * Port indices
 * ---------------------------------------------------------------------- */

#define PORT_AUDIO_IN   0
#define PORT_FREQ_OUT   1
#define PORT_NOTE_OUT   2
#define PORT_CENTS_OUT  3
#define PORT_ACTIVE_OUT 4

#define LINGOT_LV2_UI_URI "http://nongnu.org/lingot/lv2/tuner/ui"

/* Fixed UI dimensions in pixels */
#define UI_WIDTH  360
#define UI_HEIGHT  80

/* -------------------------------------------------------------------------
 * Note name table
 * ---------------------------------------------------------------------- */

static const char *NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

/* -------------------------------------------------------------------------
 * UI state
 * ---------------------------------------------------------------------- */

typedef struct {
    LV2UI_Write_Function write;
    LV2UI_Controller     controller;

    Display *dpy;
    Window   win;

    float frequency;
    float midi_note;
    float cents;
    float active;

    int needs_redraw;
} LingotLV2UI;

/* -------------------------------------------------------------------------
 * Drawing
 * ---------------------------------------------------------------------- */

static void draw_field(cairo_t *cr,
                       double cx,              /* column centre x          */
                       const char *header,
                       const char *value,
                       int active) {
    cairo_text_extents_t te;

    /* header label — small, muted */
    cairo_set_source_rgb(cr, 0.50, 0.50, 0.50);
    cairo_select_font_face(cr, "Sans",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);
    cairo_text_extents(cr, header, &te);
    cairo_move_to(cr,
                  cx - te.width / 2.0 - te.x_bearing,
                  18.0);
    cairo_show_text(cr, header);

    /* value — larger, bold, dimmed when inactive */
    if (active)
        cairo_set_source_rgb(cr, 0.90, 0.90, 0.90);
    else
        cairo_set_source_rgb(cr, 0.38, 0.38, 0.38);

    cairo_select_font_face(cr, "Sans",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 20.0);
    cairo_text_extents(cr, value, &te);
    cairo_move_to(cr,
                  cx - te.width / 2.0 - te.x_bearing,
                  UI_HEIGHT / 2.0 + 16.0);
    cairo_show_text(cr, value);
}

static void draw(LingotLV2UI *ui) {
    cairo_surface_t *surface = cairo_xlib_surface_create(
        ui->dpy, ui->win,
        DefaultVisual(ui->dpy, DefaultScreen(ui->dpy)),
        UI_WIDTH, UI_HEIGHT);

    cairo_t *cr = cairo_create(surface);

    /* Dark background */
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_paint(cr);

    /* Vertical separators between columns */
    cairo_set_source_rgb(cr, 0.30, 0.30, 0.30);
    cairo_set_line_width(cr, 1.0);
    for (int i = 1; i < 4; i++) {
        double x = i * (UI_WIDTH / 4.0);
        cairo_move_to(cr, x, 8.0);
        cairo_line_to(cr, x, UI_HEIGHT - 8.0);
        cairo_stroke(cr);
    }

    int active = ui->active > 0.5f;

    /* Note name (e.g. "A4") */
    char note_str[8] = "\xe2\x80\x94"; /* em dash (UTF-8) */
    if (active) {
        int midi   = (int)roundf(ui->midi_note);
        int note   = ((midi % 12) + 12) % 12;
        int octave = midi / 12 - 2;
        snprintf(note_str, sizeof(note_str), "%s%d", NOTE_NAMES[note], octave);
    }
    draw_field(cr, UI_WIDTH * 0.125, "Note", note_str, active);

    /* MIDI note number */
    char midi_str[8] = "\xe2\x80\x94";
    if (active)
        snprintf(midi_str, sizeof(midi_str), "%d", (int)roundf(ui->midi_note));
    draw_field(cr, UI_WIDTH * 0.375, "MIDI", midi_str, active);

    /* Frequency */
    char freq_str[16] = "\xe2\x80\x94";
    if (active)
        snprintf(freq_str, sizeof(freq_str), "%.1f Hz", (double)ui->frequency);
    draw_field(cr, UI_WIDTH * 0.625, "Frequency", freq_str, active);

    /* Cents error */
    char cents_str[16] = "\xe2\x80\x94";
    if (active)
        snprintf(cents_str, sizeof(cents_str), "%+.1f ct", (double)ui->cents);
    draw_field(cr, UI_WIDTH * 0.875, "Cents", cents_str, active);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

/* -------------------------------------------------------------------------
 * LV2 idle interface — called by the host at ~30 fps
 * ---------------------------------------------------------------------- */

static int ui_idle(LV2UI_Handle handle) {
    LingotLV2UI *ui = (LingotLV2UI *)handle;

    XEvent ev;
    while (XPending(ui->dpy) > 0) {
        XNextEvent(ui->dpy, &ev);
        if (ev.type == Expose && ev.xexpose.count == 0)
            ui->needs_redraw = 1;
    }

    if (ui->needs_redraw) {
        draw(ui);
        XFlush(ui->dpy);
        ui->needs_redraw = 0;
    }

    return 0;
}

static const LV2UI_Idle_Interface idle_iface = { ui_idle };

/* -------------------------------------------------------------------------
 * LV2 UI callbacks
 * ---------------------------------------------------------------------- */

static LV2UI_Handle ui_instantiate(
        const LV2UI_Descriptor   *descriptor,
        const char               *plugin_uri,
        const char               *bundle_path,
        LV2UI_Write_Function      write_function,
        LV2UI_Controller          controller,
        LV2UI_Widget             *widget,
        const LV2_Feature *const *features) {

    (void)descriptor; (void)plugin_uri; (void)bundle_path;

    /* X11UI requires a parent window from the host */
    Window parent = 0;
    for (int i = 0; features[i]; i++) {
        if (!strcmp(features[i]->URI, LV2_UI__parent))
            parent = (Window)(uintptr_t)features[i]->data;
    }
    if (!parent) return NULL;

    LingotLV2UI *ui = calloc(1, sizeof(LingotLV2UI));
    if (!ui) return NULL;

    ui->write        = write_function;
    ui->controller   = controller;
    ui->needs_redraw = 1;

    ui->dpy = XOpenDisplay(NULL);
    if (!ui->dpy) { free(ui); return NULL; }

    int screen = DefaultScreen(ui->dpy);
    ui->win = XCreateSimpleWindow(
        ui->dpy, parent,
        0, 0, UI_WIDTH, UI_HEIGHT, 0,
        BlackPixel(ui->dpy, screen),
        BlackPixel(ui->dpy, screen));

    /* Fix our size so the host doesn't try to resize us */
    XSizeHints hints;
    hints.flags     = PMinSize | PMaxSize;
    hints.min_width = hints.max_width  = UI_WIDTH;
    hints.min_height = hints.max_height = UI_HEIGHT;
    XSetWMNormalHints(ui->dpy, ui->win, &hints);

    XSelectInput(ui->dpy, ui->win, ExposureMask);
    XMapWindow(ui->dpy, ui->win);
    XFlush(ui->dpy);

    *widget = (LV2UI_Widget)(uintptr_t)ui->win;
    return (LV2UI_Handle)ui;
}

static void ui_cleanup(LV2UI_Handle handle) {
    LingotLV2UI *ui = (LingotLV2UI *)handle;
    if (ui->win) XDestroyWindow(ui->dpy, ui->win);
    if (ui->dpy) XCloseDisplay(ui->dpy);
    free(ui);
}

static void ui_port_event(LV2UI_Handle handle,
                          uint32_t     port_index,
                          uint32_t     buffer_size,
                          uint32_t     format,
                          const void  *buffer) {
    (void)buffer_size;
    if (format != 0) return;

    LingotLV2UI *ui   = (LingotLV2UI *)handle;
    const float value = *(const float *)buffer;

    switch (port_index) {
    case PORT_FREQ_OUT:   ui->frequency = value; break;
    case PORT_NOTE_OUT:   ui->midi_note = value; break;
    case PORT_CENTS_OUT:  ui->cents     = value; break;
    case PORT_ACTIVE_OUT: ui->active    = value; break;
    default: break;
    }

    ui->needs_redraw = 1;
}

static const void *ui_extension_data(const char *uri) {
    if (!strcmp(uri, LV2_UI__idleInterface))
        return &idle_iface;
    return NULL;
}

/* -------------------------------------------------------------------------
 * Descriptor and entry point
 * ---------------------------------------------------------------------- */

static const LV2UI_Descriptor ui_descriptor = {
    LINGOT_LV2_UI_URI,
    ui_instantiate,
    ui_cleanup,
    ui_port_event,
    ui_extension_data,
};

LV2_SYMBOL_EXPORT
const LV2UI_Descriptor *lv2ui_descriptor(uint32_t index) {
    return (index == 0) ? &ui_descriptor : NULL;
}

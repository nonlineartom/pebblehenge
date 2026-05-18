/* Renders the day's sun-path arc as altitude-vs-time, with twilight /
 * golden-hour bands underneath and a "now" marker. Color path uses
 * shaded GColor rects; mono path uses hatch patterns. */

#include "ui_arc.h"

#include <stdbool.h>

#define HORIZON_FRAC_NUM   65   /* horizon line at 65% down from the top */
#define HORIZON_FRAC_DEN  100

#define ALT_MAX_DEG        90
#define ALT_MIN_DEG       -36   /* enough headroom for astronomical twilight */

static arc_sample_t     s_samples[ARC_SAMPLE_COUNT];
static bool             s_samples_valid;
static sun_day_events_t s_events;
static int64_t          s_local_midnight_utc;
static int16_t          s_tz_offset_min;
static int64_t          s_now_utc;
static Layer           *s_layer;

/* ----------------------------------------------------------------------- */

static int64_t local_midnight_for(int64_t any_unix_utc, int16_t tz_offset_min) {
    int64_t local = any_unix_utc + (int64_t)tz_offset_min * 60;
    int64_t local_midnight_shifted = (local / 86400) * 86400;
    if (local_midnight_shifted > local) local_midnight_shifted -= 86400;
    return local_midnight_shifted - (int64_t)tz_offset_min * 60;
}

void ui_arc_recompute(const geo_fix_t *fix) {
    s_local_midnight_utc = local_midnight_for(s_now_utc ? s_now_utc : (int64_t)time(NULL),
                                              fix->tz_offset_min);
    s_tz_offset_min = fix->tz_offset_min;
    for (int i = 0; i < ARC_SAMPLE_COUNT; i++) {
        int64_t t = s_local_midnight_utc + (int64_t)i * 1800;
        sun_position_t p = sun_position(t, fix->lat_deg, fix->lon_deg);
        s_samples[i].minutes_from_local_midnight = (int16_t)(i * 30);
        s_samples[i].altitude_deg = p.altitude;
        s_samples[i].azimuth_deg  = p.azimuth;
    }
    s_events = sun_day_events(s_local_midnight_utc + 43200,
                              fix->lat_deg, fix->lon_deg, fix->tz_offset_min);
    s_samples_valid = true;
    if (s_layer) layer_mark_dirty(s_layer);
}

const sun_day_events_t *ui_arc_events(void) {
    return &s_events;
}

void ui_arc_set_now(int64_t now_utc) {
    s_now_utc = now_utc;
    if (s_layer) layer_mark_dirty(s_layer);
}

/* ----------------------------------------------------------------------- */

static int16_t event_to_local_minutes(int64_t event_unix_utc) {
    if (event_unix_utc == SUN_NEVER_RISES) return -1;
    if (event_unix_utc == SUN_NEVER_SETS)  return 1441;
    int64_t local = event_unix_utc + (int64_t)s_tz_offset_min * 60;
    int64_t midnight_local = (s_local_midnight_utc + (int64_t)s_tz_offset_min * 60);
    int64_t delta = local - midnight_local;
    if (delta < 0)       return 0;
    if (delta >= 86400)  return 1440;
    return (int16_t)(delta / 60);
}

static int16_t x_for_minutes(int minutes, GRect b) {
    if (minutes < 0)    minutes = 0;
    if (minutes > 1440) minutes = 1440;
    return (int16_t)(b.origin.x + (minutes * (int)b.size.w) / 1440);
}

static int16_t y_for_altitude(float alt_deg, GRect b) {
    int horizon_y = b.origin.y + b.size.h * HORIZON_FRAC_NUM / HORIZON_FRAC_DEN;
    if (alt_deg >= 0) {
        int top_y = b.origin.y + 2;
        return (int16_t)(horizon_y - (int)(alt_deg * (horizon_y - top_y) / ALT_MAX_DEG));
    } else {
        int bottom_y = b.origin.y + b.size.h - 2;
        float clamped = alt_deg < ALT_MIN_DEG ? ALT_MIN_DEG : alt_deg;
        return (int16_t)(horizon_y + (int)(-clamped * (bottom_y - horizon_y) / -ALT_MIN_DEG));
    }
}

/* ----------------------------------------------------------------------- */

typedef enum {
    BAND_NIGHT,
    BAND_ASTRO_TWILIGHT,
    BAND_NAUTICAL_TWILIGHT,
    BAND_CIVIL_TWILIGHT,
    BAND_BLUE_HOUR,
    BAND_GOLDEN_HOUR,
    BAND_DAYLIGHT,
    BAND_COUNT
} band_kind_t;

#if defined(PBL_COLOR)
static GColor band_color(band_kind_t k) {
    switch (k) {
    case BAND_NIGHT:             return GColorBlack;
    case BAND_ASTRO_TWILIGHT:    return GColorOxfordBlue;
    case BAND_NAUTICAL_TWILIGHT: return GColorDukeBlue;
    case BAND_CIVIL_TWILIGHT:    return GColorBlueMoon;
    case BAND_BLUE_HOUR:         return GColorPictonBlue;
    case BAND_GOLDEN_HOUR:       return GColorOrange;
    case BAND_DAYLIGHT:          return GColorPastelYellow;
    default:                     return GColorWhite;
    }
}
#endif

#if defined(PBL_BW)
/* 8x8 hatch patterns packed as 8 bytes (LSB = leftmost pixel). */
static const uint8_t HATCH_NIGHT[8]    = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint8_t HATCH_ASTRO[8]    = {0xAA,0x55,0xAA,0x55,0xAA,0x55,0xAA,0x55};
static const uint8_t HATCH_NAUT[8]     = {0x88,0x22,0x88,0x22,0x88,0x22,0x88,0x22};
static const uint8_t HATCH_CIVIL[8]    = {0x80,0x00,0x08,0x00,0x80,0x00,0x08,0x00};
static const uint8_t HATCH_BLUE[8]     = {0x11,0x22,0x44,0x88,0x11,0x22,0x44,0x88};
static const uint8_t HATCH_GOLDEN[8]   = {0x88,0x44,0x22,0x11,0x88,0x44,0x22,0x11};
static const uint8_t HATCH_DAYLIGHT[8] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

static const uint8_t *band_hatch(band_kind_t k) {
    switch (k) {
    case BAND_NIGHT:             return HATCH_NIGHT;
    case BAND_ASTRO_TWILIGHT:    return HATCH_ASTRO;
    case BAND_NAUTICAL_TWILIGHT: return HATCH_NAUT;
    case BAND_CIVIL_TWILIGHT:    return HATCH_CIVIL;
    case BAND_BLUE_HOUR:         return HATCH_BLUE;
    case BAND_GOLDEN_HOUR:       return HATCH_GOLDEN;
    case BAND_DAYLIGHT:          return HATCH_DAYLIGHT;
    default:                     return HATCH_DAYLIGHT;
    }
}

static void fill_hatched(GContext *ctx, GRect r, const uint8_t *pat) {
    /* Painter's-rect of an 8x8 pattern. Simple per-pixel write keeps the
     * code obvious and is fast enough for the small band rects. */
    if (r.size.w <= 0 || r.size.h <= 0) return;
    graphics_context_set_stroke_color(ctx, GColorBlack);
    for (int yy = 0; yy < r.size.h; yy++) {
        uint8_t row = pat[yy & 7];
        for (int xx = 0; xx < r.size.w; xx++) {
            if (row & (1u << (xx & 7))) {
                graphics_draw_pixel(ctx, GPoint(r.origin.x + xx, r.origin.y + yy));
            }
        }
    }
}
#endif

static void fill_band(GContext *ctx, GRect b, int x0, int x1, band_kind_t k) {
    if (x1 <= x0) return;
    GRect r = GRect(x0, b.origin.y, x1 - x0, b.size.h);
#if defined(PBL_COLOR)
    graphics_context_set_fill_color(ctx, band_color(k));
    graphics_fill_rect(ctx, r, 0, GCornerNone);
#else
    fill_hatched(ctx, r, band_hatch(k));
#endif
}

/* ----------------------------------------------------------------------- */

typedef struct {
    band_kind_t kind;
    int16_t     start_min;
    int16_t     end_min;
} band_span_t;

static int build_band_spans(band_span_t *out, int cap) {
    /* Construct the morning -> evening sequence, then clip to [0,1440]. */
    int16_t midnight   = 0;
    int16_t astro_dawn = event_to_local_minutes(s_events.astronomical_dawn);
    int16_t naut_dawn  = event_to_local_minutes(s_events.nautical_dawn);
    int16_t civ_dawn   = event_to_local_minutes(s_events.civil_dawn);
    int16_t blue_end   = event_to_local_minutes(s_events.blue_hour_morning_end);
    int16_t sunrise    = event_to_local_minutes(s_events.sunrise);
    int16_t golden_morn= event_to_local_minutes(s_events.golden_hour_morning_end);
    int16_t golden_eve = event_to_local_minutes(s_events.golden_hour_evening_start);
    int16_t sunset     = event_to_local_minutes(s_events.sunset);
    int16_t blue_start = event_to_local_minutes(s_events.blue_hour_evening_start);
    int16_t civ_dusk   = event_to_local_minutes(s_events.civil_dusk);
    int16_t naut_dusk  = event_to_local_minutes(s_events.nautical_dusk);
    int16_t astro_dusk = event_to_local_minutes(s_events.astronomical_dusk);
    int16_t end_of_day = 1440;

    /* Polar day: everything is daylight. */
    if (s_events.sunrise == SUN_NEVER_SETS) {
        if (cap < 1) return 0;
        out[0] = (band_span_t){ BAND_DAYLIGHT, 0, 1440 };
        return 1;
    }
    /* Polar night: everything is night. */
    if (s_events.sunrise == SUN_NEVER_RISES) {
        if (cap < 1) return 0;
        out[0] = (band_span_t){ BAND_NIGHT, 0, 1440 };
        return 1;
    }

    static const band_kind_t order[] = {
        BAND_NIGHT, BAND_ASTRO_TWILIGHT, BAND_NAUTICAL_TWILIGHT, BAND_CIVIL_TWILIGHT,
        BAND_BLUE_HOUR, BAND_GOLDEN_HOUR, BAND_DAYLIGHT, BAND_GOLDEN_HOUR,
        BAND_BLUE_HOUR, BAND_CIVIL_TWILIGHT, BAND_NAUTICAL_TWILIGHT, BAND_ASTRO_TWILIGHT,
        BAND_NIGHT
    };
    int16_t boundaries[14] = {
        midnight, astro_dawn, naut_dawn, civ_dawn, blue_end, sunrise,
        golden_morn, golden_eve, sunset, blue_start, civ_dusk, naut_dusk,
        astro_dusk, end_of_day
    };
    /* Clip sentinels (-1 / 1441) to nearest in-range neighbor. */
    for (int i = 0; i < 14; i++) {
        if (boundaries[i] < 0)    boundaries[i] = 0;
        if (boundaries[i] > 1440) boundaries[i] = 1440;
    }
    /* Enforce monotonicity defensively. */
    for (int i = 1; i < 14; i++) {
        if (boundaries[i] < boundaries[i-1]) boundaries[i] = boundaries[i-1];
    }
    int n = 0;
    for (int i = 0; i < 13 && n < cap; i++) {
        if (boundaries[i+1] > boundaries[i]) {
            out[n++] = (band_span_t){ order[i], boundaries[i], boundaries[i+1] };
        }
    }
    return n;
}

/* ----------------------------------------------------------------------- */

static void draw_bands(GContext *ctx, GRect b) {
    band_span_t spans[16];
    int n = build_band_spans(spans, 16);
    for (int i = 0; i < n; i++) {
        int x0 = x_for_minutes(spans[i].start_min, b);
        int x1 = x_for_minutes(spans[i].end_min,   b);
        fill_band(ctx, b, x0, x1, spans[i].kind);
    }
}

static void draw_arc(GContext *ctx, GRect b) {
#if defined(PBL_COLOR)
    graphics_context_set_stroke_color(ctx, GColorBlack);
#else
    graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
    graphics_context_set_stroke_width(ctx, 2);
    for (int i = 1; i < ARC_SAMPLE_COUNT; i++) {
        GPoint a = GPoint(x_for_minutes(s_samples[i-1].minutes_from_local_midnight, b),
                          y_for_altitude(s_samples[i-1].altitude_deg, b));
        GPoint p = GPoint(x_for_minutes(s_samples[i].minutes_from_local_midnight,   b),
                          y_for_altitude(s_samples[i].altitude_deg,   b));
        graphics_draw_line(ctx, a, p);
    }
    graphics_context_set_stroke_width(ctx, 1);
}

static void draw_horizon(GContext *ctx, GRect b) {
    int horizon_y = b.origin.y + b.size.h * HORIZON_FRAC_NUM / HORIZON_FRAC_DEN;
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_line(ctx, GPoint(b.origin.x, horizon_y),
                            GPoint(b.origin.x + b.size.w - 1, horizon_y));
}

static void draw_now_marker(GContext *ctx, GRect b) {
    if (!s_samples_valid) return;
    int64_t midnight_utc = s_local_midnight_utc;
    int minutes = (int)((s_now_utc - midnight_utc) / 60);
    if (minutes < 0 || minutes > 1440) return;
    /* Interpolate altitude between the two surrounding 30-min samples. */
    int idx = minutes / 30;
    if (idx >= ARC_SAMPLE_COUNT - 1) idx = ARC_SAMPLE_COUNT - 2;
    int frac_num = minutes - idx * 30;
    float alt = s_samples[idx].altitude_deg
              + (s_samples[idx+1].altitude_deg - s_samples[idx].altitude_deg)
              * ((float)frac_num / 30.0f);
    GPoint c = GPoint(x_for_minutes(minutes, b), y_for_altitude(alt, b));

    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, c, 6);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, c, 4);
}

static void arc_update_proc(Layer *layer, GContext *ctx) {
    GRect b = layer_get_bounds(layer);
#if defined(PBL_COLOR)
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
#else
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
#endif
    if (!s_samples_valid) return;
    draw_bands(ctx, b);
    draw_horizon(ctx, b);
    draw_arc(ctx, b);
    draw_now_marker(ctx, b);
}

/* ----------------------------------------------------------------------- */

Layer *ui_arc_layer_create(GRect frame) {
    s_layer = layer_create(frame);
    layer_set_update_proc(s_layer, arc_update_proc);
    return s_layer;
}

void ui_arc_layer_destroy(Layer *layer) {
    if (layer == s_layer) s_layer = NULL;
    layer_destroy(layer);
}

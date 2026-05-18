/* Sky-dome renderer: projects the day's sun path onto a heading-aware
 * 2D view. The horizontal axis is azimuth offset from the observer's
 * current heading (centre = directly ahead, edges = directly behind);
 * the vertical axis is altitude (top = zenith, middle = horizon).
 * The result is a rainbow-shaped arc whose endpoints sit on the horizon
 * at sunrise (east) and sunset (west); a filled dot marks the sun's
 * current position. When the user turns, the whole arc slides
 * horizontally because each sample's azimuth-offset changes. */

#include "ui_arc.h"

#include <stdbool.h>

/* Vertical layout (canvas-local). Default horizon position used when
 * the IMU hasn't supplied a pitch yet (~74 deg of tilt — a typical
 * watch-on-wrist viewing angle). */
#define HORIZON_FRAC_NUM   72
#define HORIZON_FRAC_DEN  100
#define ALT_TOP_DEG        90.0f
#define ALT_BOTTOM_DEG    -20.0f

/* Pitch in degrees; -1 = no IMU yet, use the default fraction. */
static float s_pitch_deg = -1.0f;

/* Horizontal field of view: a narrower FOV "zooms in" on the visible
 * sky in front of the user. 240 deg shows roughly horizon to horizon
 * without much screen wasted on what's behind. */
#define FOV_DEG           240.0f

static arc_sample_t     s_samples[ARC_SAMPLE_COUNT];
static bool             s_samples_valid;
static sun_day_events_t s_events;
static int64_t          s_local_midnight_utc;
static int16_t          s_tz_offset_min;
static int64_t          s_now_utc;
/* -1 = "no compass yet, auto-centre on solar noon". The Pebble app loader
 * doesn't always apply .data initialisers for non-zero static floats, so
 * we also force this in ui_arc_layer_create. */
static float            s_heading_deg = -1.0f;
static bool             s_heading_init_done;
static Layer           *s_layer;

/* ----------------------------------------------------------------------- */

static int64_t local_midnight_for(int64_t any_unix_utc, int16_t tz_offset_min) {
    int64_t local = any_unix_utc + (int64_t)tz_offset_min * 60;
    int64_t local_midnight_shifted = (local / 86400) * 86400;
    if (local_midnight_shifted > local) local_midnight_shifted -= 86400;
    return local_midnight_shifted - (int64_t)tz_offset_min * 60;
}

void ui_arc_recompute(const geo_fix_t *fix) {
    s_local_midnight_utc = local_midnight_for(
        s_now_utc ? s_now_utc : (int64_t)(int32_t)time(NULL), fix->tz_offset_min);
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

const sun_day_events_t *ui_arc_events(void) { return &s_events; }

void ui_arc_set_now(int64_t now_utc) {
    s_now_utc = now_utc;
    if (s_layer) layer_mark_dirty(s_layer);
}

void ui_arc_set_heading(float heading_deg) {
    s_heading_deg = heading_deg;
    if (s_layer) layer_mark_dirty(s_layer);
}

void ui_arc_set_pitch(float pitch_deg) {
    /* Filter tiny changes so we don't redraw on every accel sample
     * when the user is holding the watch steady. */
    float delta = pitch_deg - s_pitch_deg;
    if (delta < 0.0f) delta = -delta;
    if (s_pitch_deg >= 0.0f && delta < 1.5f) return;
    s_pitch_deg = pitch_deg;
    if (s_layer) layer_mark_dirty(s_layer);
}

/* ----------------------------------------------------------------------- */

/* Cached "view centre" azimuth used by az_offset_deg. When the compass
 * is calibrated this is the observer's true heading; otherwise we centre
 * the view on the sun's current azimuth so the arc is always legible. */
static float s_view_center_deg;  /* set at first use */

static void recompute_view_center(void) {
    if (s_heading_deg >= 0.0f) {
        s_view_center_deg = s_heading_deg;
        return;
    }
    if (!s_samples_valid) { s_view_center_deg = 0.0f; return; }
    /* No compass: centre on solar noon (the sample with the highest
     * altitude). That puts the arc symmetrically across the screen with
     * sunrise on the left and sunset on the right - the rainbow shape. */
    int best = 0;
    for (int i = 1; i < ARC_SAMPLE_COUNT; i++) {
        if (s_samples[i].altitude_deg > s_samples[best].altitude_deg) best = i;
    }
    s_view_center_deg = s_samples[best].azimuth_deg;
}

static float az_offset_deg(float sun_az) {
    float d = sun_az - s_view_center_deg;
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

/* Return the y position of the horizon line, taking the IMU pitch into
 * account. Pitch 0 (face up) -> horizon near the bottom of the canvas
 * (looking straight up). Pitch ~90 (vertical) -> horizon at canvas
 * centre. Pitch 180 (face down) -> horizon near the top. */
static int horizon_y_for(GRect b) {
    if (s_pitch_deg < 0.0f) {
        return b.origin.y + b.size.h * HORIZON_FRAC_NUM / HORIZON_FRAC_DEN;
    }
    /* Clamp pitch to [0, 180] and linearly map to [bottom, top]. We pad
     * the extremes by 5% so the horizon line stays on-screen. */
    float p = s_pitch_deg;
    if (p <    0.0f) p =   0.0f;
    if (p >  180.0f) p = 180.0f;
    float frac = 0.95f - (p / 180.0f) * 0.90f;  /* [0.05, 0.95] */
    return b.origin.y + (int)(frac * (float)b.size.h);
}

static int x_for_offset(float offset_deg, GRect b) {
    float t = (offset_deg + FOV_DEG / 2.0f) / FOV_DEG;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return b.origin.x + (int)(t * (float)(b.size.w - 1));
}

static int y_for_altitude(float alt_deg, GRect b) {
    int horizon_y = horizon_y_for(b);
    if (alt_deg >= 0.0f) {
        int top_y = b.origin.y + 2;
        float t = alt_deg / ALT_TOP_DEG;
        if (t > 1.0f) t = 1.0f;
        return horizon_y - (int)(t * (float)(horizon_y - top_y));
    } else {
        int bottom_y = b.origin.y + b.size.h - 1;
        float t = alt_deg / ALT_BOTTOM_DEG;
        if (t > 1.0f) t = 1.0f;
        return horizon_y + (int)(t * (float)(bottom_y - horizon_y));
    }
}

/* Interpolate the sun's azimuth/altitude at an arbitrary unix UTC time
 * between the two cached half-hour samples that bracket it. */
static void sun_at_now(int64_t now_utc, float *az, float *alt) {
    int minutes = (int)((now_utc - s_local_midnight_utc) / 60);
    if (minutes < 0) minutes = 0;
    if (minutes > 1440) minutes = 1440;
    int idx = minutes / 30;
    if (idx >= ARC_SAMPLE_COUNT - 1) idx = ARC_SAMPLE_COUNT - 2;
    float f = (float)(minutes - idx * 30) / 30.0f;
    /* Linear interp for altitude. */
    *alt = s_samples[idx].altitude_deg
         + (s_samples[idx + 1].altitude_deg - s_samples[idx].altitude_deg) * f;
    /* Azimuth needs wrap-aware interpolation. */
    float a = s_samples[idx].azimuth_deg;
    float b = s_samples[idx + 1].azimuth_deg;
    float d = b - a;
    if (d >  180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    float v = a + d * f;
    while (v < 0.0f)    v += 360.0f;
    while (v >= 360.0f) v -= 360.0f;
    *az = v;
}

/* ----------------------------------------------------------------------- */

static void draw_background(GContext *ctx, GRect b) {
#if defined(PBL_COLOR)
    graphics_context_set_fill_color(ctx, GColorPictonBlue);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
    /* Below-horizon "ground" in dark. */
    int hy = horizon_y_for(b);
    GRect below = GRect(b.origin.x, hy, b.size.w,
                        b.origin.y + b.size.h - hy);
    graphics_context_set_fill_color(ctx, GColorOxfordBlue);
    graphics_fill_rect(ctx, below, 0, GCornerNone);
#else
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
    /* Hatch the below-horizon area lightly so it's distinguishable. */
    int horizon_y = horizon_y_for(b);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    for (int y = horizon_y + 2; y < b.origin.y + b.size.h; y += 3) {
        for (int x = b.origin.x + (y & 1); x < b.origin.x + b.size.w; x += 2) {
            graphics_draw_pixel(ctx, GPoint(x, y));
        }
    }
#endif
}

static void draw_horizon(GContext *ctx, GRect b) {
    int hy = horizon_y_for(b);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(b.origin.x, hy),
                            GPoint(b.origin.x + b.size.w - 1, hy));
}

static void draw_cardinal_ticks(GContext *ctx, GRect b) {
    /* N/E/S/W tick marks on the horizon line at their azimuth-offsets. */
    int hy = horizon_y_for(b);
    const struct { const char *label; float az; } pts[] = {
        {"N",   0.0f},
        {"E",  90.0f},
        {"S", 180.0f},
        {"W", 270.0f},
    };
    graphics_context_set_text_color(ctx, GColorBlack);
    for (unsigned i = 0; i < sizeof pts / sizeof pts[0]; i++) {
        float off = az_offset_deg(pts[i].az);
        if (off < -FOV_DEG / 2.0f || off > FOV_DEG / 2.0f) continue;
        int x = x_for_offset(off, b);
        graphics_draw_line(ctx, GPoint(x, hy - 2), GPoint(x, hy + 2));
        graphics_draw_text(ctx, pts[i].label,
                           fonts_get_system_font(FONT_KEY_GOTHIC_14),
                           GRect(x - 8, hy + 3, 16, 14),
                           GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    }
}

/* Connect two consecutive samples with a line segment, splitting at the
 * +/-180 deg azimuth-offset wrap if needed so the curve never jumps the
 * screen in a straight line through the unrendered back of the sky. */
static void draw_arc_segment(GContext *ctx, GRect b,
                              float off0, float alt0,
                              float off1, float alt1) {
    /* If the segment crosses the +/-180 wrap, split it. */
    float d = off1 - off0;
    if (d > 180.0f || d < -180.0f) return;  /* Skip the wrap-around segment. */

    GPoint p0 = GPoint(x_for_offset(off0, b), y_for_altitude(alt0, b));
    GPoint p1 = GPoint(x_for_offset(off1, b), y_for_altitude(alt1, b));
    bool below = (alt0 < 0.0f && alt1 < 0.0f);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, below ? 1 : 2);
    graphics_draw_line(ctx, p0, p1);
}

static void draw_arc(GContext *ctx, GRect b) {
    if (!s_samples_valid) return;
    for (int i = 1; i < ARC_SAMPLE_COUNT; i++) {
        float off0 = az_offset_deg(s_samples[i - 1].azimuth_deg);
        float off1 = az_offset_deg(s_samples[i].azimuth_deg);
        draw_arc_segment(ctx, b,
                         off0, s_samples[i - 1].altitude_deg,
                         off1, s_samples[i].altitude_deg);
    }
}

static int16_t event_to_local_minutes(int64_t event_unix_utc) {
    if (event_unix_utc == SUN_NEVER_RISES) return -1;
    if (event_unix_utc == SUN_NEVER_SETS)  return 1441;
    int64_t local = event_unix_utc + (int64_t)s_tz_offset_min * 60;
    int64_t midnight_local = s_local_midnight_utc + (int64_t)s_tz_offset_min * 60;
    int64_t delta = local - midnight_local;
    if (delta < 0)       return 0;
    if (delta >= 86400)  return 1440;
    return (int16_t)(delta / 60);
}

static void sun_at_minutes(int minutes, float *az, float *alt) {
    if (minutes < 0) minutes = 0;
    if (minutes > 1440) minutes = 1440;
    int idx = minutes / 30;
    if (idx >= ARC_SAMPLE_COUNT - 1) idx = ARC_SAMPLE_COUNT - 2;
    float f = (float)(minutes - idx * 30) / 30.0f;
    *alt = s_samples[idx].altitude_deg
         + (s_samples[idx + 1].altitude_deg - s_samples[idx].altitude_deg) * f;
    float a = s_samples[idx].azimuth_deg;
    float bb = s_samples[idx + 1].azimuth_deg;
    float d = bb - a;
    if (d >  180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    float v = a + d * f;
    while (v < 0.0f)    v += 360.0f;
    while (v >= 360.0f) v -= 360.0f;
    *az = v;
}

static void draw_endpoint(GContext *ctx, GRect b, int local_minutes,
                          bool is_sunrise) {
    if (local_minutes < 0 || local_minutes > 1440) return;
    float az, alt;
    sun_at_minutes(local_minutes, &az, &alt);
    float off = az_offset_deg(az);
    if (off < -FOV_DEG / 2.0f || off > FOV_DEG / 2.0f) return;
    GPoint p = GPoint(x_for_offset(off, b), y_for_altitude(alt, b));
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, p, 3);
    graphics_draw_circle(ctx, p, 3);
    if (is_sunrise) {
        /* Up-tick indicates the rising end. */
        graphics_draw_line(ctx, GPoint(p.x, p.y - 4), GPoint(p.x, p.y - 7));
    } else {
        graphics_draw_line(ctx, GPoint(p.x, p.y + 4), GPoint(p.x, p.y + 7));
    }
}

static void draw_sun(GContext *ctx, GRect b) {
    if (!s_samples_valid) return;
    float az, alt;
    sun_at_now(s_now_utc, &az, &alt);
    float off = az_offset_deg(az);
    if (off < -FOV_DEG / 2.0f || off > FOV_DEG / 2.0f) return;
    GPoint p = GPoint(x_for_offset(off, b), y_for_altitude(alt, b));

    /* White halo so it stands out over the arc + bands. */
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, p, 7);
#if defined(PBL_COLOR)
    graphics_context_set_fill_color(ctx, GColorYellow);
#else
    graphics_context_set_fill_color(ctx, GColorBlack);
#endif
    graphics_fill_circle(ctx, p, 5);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_circle(ctx, p, 5);
}

static void draw_heading_marker(GContext *ctx, GRect b) {
    /* Triangle at the very top centre pointing down, marking the
     * direction the user is facing. */
    if (s_heading_deg < 0.0f) return;
    int cx = b.origin.x + b.size.w / 2;
    int top = b.origin.y;
    graphics_context_set_fill_color(ctx, GColorBlack);
    GPoint tri[3] = {
        { cx,     top + 5 },
        { cx - 3, top     },
        { cx + 3, top     },
    };
    GPathInfo info = { 3, tri };
    GPath *p = gpath_create(&info);
    gpath_draw_filled(ctx, p);
    gpath_destroy(p);
}

static void arc_update_proc(Layer *layer, GContext *ctx) {
    GRect b = layer_get_bounds(layer);
    recompute_view_center();
    draw_background(ctx, b);
    if (!s_samples_valid) return;
    draw_horizon(ctx, b);
    draw_cardinal_ticks(ctx, b);
    draw_arc(ctx, b);
    draw_endpoint(ctx, b, event_to_local_minutes(s_events.sunrise), true);
    draw_endpoint(ctx, b, event_to_local_minutes(s_events.sunset),  false);
    draw_sun(ctx, b);
    draw_heading_marker(ctx, b);
}

/* ----------------------------------------------------------------------- */

Layer *ui_arc_layer_create(GRect frame) {
    if (!s_heading_init_done) {
        s_heading_deg = -1.0f;
        s_heading_init_done = true;
    }
    s_layer = layer_create(frame);
    layer_set_update_proc(s_layer, arc_update_proc);
    return s_layer;
}

void ui_arc_layer_destroy(Layer *layer) {
    if (layer == s_layer) s_layer = NULL;
    layer_destroy(layer);
}

#include <sys/types.h>  /* time_t typedef (Pebble's pebble.h relies on it
                         * being in scope, but newer newlib time.h is
                         * suppressed by the SDK's -D_TIME_H_). */
#include <pebble.h>

#include "sun.h"
#include "geo.h"
#include "compass.h"
#include "imu.h"
#include "ui_arc.h"

/* --- App-wide state ---------------------------------------------------- */

typedef enum {
    VIEW_DATA = 0,
    VIEW_COMPASS = 1,
    VIEW_COUNT
} view_mode_t;

static view_mode_t  s_mode = VIEW_DATA;
static float        s_last_sun_az  = 0.0f;
static float        s_last_sun_alt = 0.0f;

/* Timeline scrub: offset (in seconds) from real "now". Up/Down jog. */
#define SCRUB_STEP_SEC       (10 * 60)
#define SCRUB_INACTIVITY_SEC 6
static int32_t      s_scrub_offset_sec = 0;
static int64_t      s_scrub_last_input_unix = 0;
static pbh_compass_t s_compass = { .heading_deg = 0.0f,
                                   .status = CompassStatusDataInvalid,
                                   .calibrated = false };

/* --- UI handles --------------------------------------------------------- */

static Window     *s_window;
static Layer      *s_data_root;
static Layer      *s_compass_root;
static Layer      *s_arrow_layer;
static GPath      *s_arrow_path;

static TextLayer  *s_clock_layer;
static TextLayer  *s_gps_layer;       /* compact "* 5m" / "o 2h" / "?" */
static TextLayer  *s_events_layer;    /* "05:25 - 20:31" centred */
static Layer      *s_arc_canvas;

static TextLayer  *s_hdg_layer;
static TextLayer  *s_bearing_layer;
static TextLayer  *s_status_layer;
/* AZ/ALT lives in the compass view; not in the main view by default. */
static TextLayer  *s_compass_az_alt_layer;

/* --- Text buffers ------------------------------------------------------- */

static char s_clock_buf[8];
static char s_gps_buf[12];
static char s_events_buf[20];
static char s_compass_az_alt_buf[28];

static char s_hdg_buf[16];
static char s_bearing_buf[24];
static char s_status_buf[28];

/* Day index for which the arc + events are currently cached. */
static int s_events_day = -1;

/* --- Triangle pointing "up" (toward sun before rotation). -------------- */

static const GPathInfo ARROW_INFO = {
    .num_points = 7,
    .points = (GPoint[]){
        {  0, -32 },
        {  9,  -8 },
        {  4,  -8 },
        {  4,  24 },
        { -4,  24 },
        { -4,  -8 },
        { -9,  -8 }
    }
};

/* ----------------------------------------------------------------------- */

static const char *cardinal_8(float az_deg) {
    static const char *table[8] = {"N","NE","E","SE","S","SW","W","NW"};
    int idx = (int)((az_deg + 22.5f) / 45.0f) & 7;
    return table[idx];
}

static void format_event_short(int64_t unix_utc, int tz_offset_min,
                                char *out, size_t n, const char *prefix) {
    if (unix_utc == SUN_NEVER_RISES) { snprintf(out, n, "%s--:--", prefix); return; }
    if (unix_utc == SUN_NEVER_SETS)  { snprintf(out, n, "%salwys", prefix); return; }
    int64_t local = unix_utc + (int64_t)tz_offset_min * 60;
    int sec_of_day = (int)(((local % 86400) + 86400) % 86400);
    int hh = sec_of_day / 3600;
    int mm = (sec_of_day % 3600) / 60;
    snprintf(out, n, "%s%02d:%02d", prefix, hh, mm);
}

static void format_fix_age(int64_t fix_unix, int64_t now_utc, char *out, size_t n) {
    int64_t age = now_utc - fix_unix;
    if (age < 60)         snprintf(out, n, "%ds", (int)age);
    else if (age < 3600)  snprintf(out, n, "%dm", (int)(age / 60));
    else if (age < 86400) snprintf(out, n, "%dh", (int)(age / 3600));
    else                  snprintf(out, n, "%dd", (int)(age / 86400));
}

static void update_app_glance(int64_t now_utc);

static void recompute_arc_if_needed(int64_t now_utc, const geo_fix_t *fix) {
    int local_day = (int)(((now_utc + (int64_t)fix->tz_offset_min * 60) / 86400) & 0xFFFF);
    if (local_day == s_events_day) return;
    ui_arc_set_now(now_utc);
    ui_arc_recompute(fix);
    s_events_day = local_day;
    update_app_glance(now_utc);
}

static bool find_next_event(int64_t now_utc, const char **out_label,
                            int64_t *out_when) {
    const sun_day_events_t *e = ui_arc_events();
    struct { const char *label; int64_t when; } cands[] = {
        { "Rise",   e->sunrise },
        { "Noon",   e->solar_noon },
        { "Golden", e->golden_hour_evening_start },
        { "Set",    e->sunset },
        { "Civil",  e->civil_dusk },
    };
    int64_t best = (int64_t)1 << 62;
    const char *label = "";
    int64_t when = 0;
    for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++) {
        int64_t w = cands[i].when;
        if (w == SUN_NEVER_RISES || w == SUN_NEVER_SETS) continue;
        if (w > now_utc && w < best) { best = w; label = cands[i].label; when = w; }
    }
    if (best == ((int64_t)1 << 62)) return false;
    *out_label = label;
    *out_when  = when;
    return true;
}

static void describe_next_event(int64_t now_utc, char *out, size_t n) {
    const char *label;
    int64_t when;
    if (!find_next_event(now_utc, &label, &when)) {
        snprintf(out, n, "-");
        return;
    }
    geo_fix_t fix = geo_current();
    int64_t local = when + (int64_t)fix.tz_offset_min * 60;
    int sec = (int)(((local % 86400) + 86400) % 86400);
    snprintf(out, n, "%s %02d:%02d", label, sec / 3600, (sec % 3600) / 60);
}

typedef struct {
    char    text[24];
    int64_t expire;
} glance_payload_t;

static void glance_reload_cb(AppGlanceReloadSession *session, size_t limit, void *context) {
    if (limit < 1) return;
    glance_payload_t *p = (glance_payload_t *)context;
    AppGlanceSlice slice = (AppGlanceSlice){
        .layout = {
            .icon = 0,                /* fall back to the app's own icon */
            .subtitle_template_string = p->text,
        },
        .expiration_time = (time_t)p->expire,
    };
    app_glance_add_slice(session, slice);
}

static void update_app_glance(int64_t now_utc) {
    const char *label;
    int64_t when;
    if (!find_next_event(now_utc, &label, &when)) return;
    geo_fix_t fix = geo_current();
    int64_t local = when + (int64_t)fix.tz_offset_min * 60;
    int sec = (int)(((local % 86400) + 86400) % 86400);

    static glance_payload_t payload;
    snprintf(payload.text, sizeof payload.text, "%s %02d:%02d",
             label, sec / 3600, (sec % 3600) / 60);
    payload.expire = when;
    app_glance_reload(glance_reload_cb, &payload);
}

static float bearing_delta(float sun_az_deg, float heading_deg) {
    float d = sun_az_deg - heading_deg;
    while (d <= -180.0f) d += 360.0f;
    while (d >   180.0f) d -= 360.0f;
    return d;
}

static const char *calibration_label(CompassStatus s) {
    switch (s) {
    case CompassStatusDataInvalid: return "Wave figure-8 to calibrate";
    case CompassStatusCalibrating: return "Calibrating...";
    case CompassStatusCalibrated:  return "Calibrated";
    default:                       return "";
    }
}

static void update_compass_labels(void) {
    snprintf(s_hdg_buf, sizeof s_hdg_buf, "HDG %3d %s",
             (int)(s_compass.heading_deg + 0.5f),
             cardinal_8(s_compass.heading_deg));

    if (s_compass.status == CompassStatusDataInvalid) {
        snprintf(s_bearing_buf, sizeof s_bearing_buf, "Sun  AZ %3d",
                 (int)(s_last_sun_az + 0.5f));
    } else {
        float d = bearing_delta(s_last_sun_az, s_compass.heading_deg);
        const char *side = (d >= 0) ? "R" : "L";
        if (d < 0) d = -d;
        snprintf(s_bearing_buf, sizeof s_bearing_buf,
                 "Sun %3d %s  ALT %+3d",
                 (int)(d + 0.5f), side, (int)(s_last_sun_alt + 0.5f));
    }

    snprintf(s_status_buf, sizeof s_status_buf, "%s",
             calibration_label(s_compass.status));
}

/* --- Arrow drawing ----------------------------------------------------- */

static void arrow_update(Layer *layer, GContext *ctx) {
    GRect b = layer_get_bounds(layer);
    GPoint c = GPoint(b.size.w / 2, b.size.h / 2);
    int radius = (b.size.w < b.size.h ? b.size.w : b.size.h) / 2 - 2;

    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_draw_circle(ctx, c, radius);

    if (s_compass.status == CompassStatusDataInvalid) {
        /* Without a calibrated heading we cannot draw a bearing arrow.
         * The status banner below the dial tells the user to calibrate. */
        return;
    }

    float delta = bearing_delta(s_last_sun_az, s_compass.heading_deg);
    bool below_horizon = (s_last_sun_alt < 0.0f);

    gpath_move_to(s_arrow_path, c);
    gpath_rotate_to(s_arrow_path, DEG_TO_TRIGANGLE((int32_t)delta));
    if (below_horizon) gpath_draw_outline(ctx, s_arrow_path);
    else               gpath_draw_filled(ctx, s_arrow_path);
}

/* --- View switch -------------------------------------------------------- */

static void apply_mode(void) {
    layer_set_hidden(s_data_root,    s_mode != VIEW_DATA);
    layer_set_hidden(s_compass_root, s_mode != VIEW_COMPASS);
}

static void redraw_compass_view(void) {
    update_compass_labels();
    layer_mark_dirty(s_arrow_layer);
}

/* Pebble's runtime returns time as a 32-bit value, but newer arm-none-eabi
 * newlib defines time_t as 64-bit; the high bits come back as uninitialised
 * stack noise. Truncate explicitly. */
static int64_t now_unix_utc(void) {
    int32_t t = (int32_t)time(NULL);
    return (int64_t)t;
}

static void refresh_view(void) {
    int64_t real_now_utc = now_unix_utc();

    /* Auto-release the scrub after a few seconds of no input. */
    if (s_scrub_offset_sec != 0
        && real_now_utc - s_scrub_last_input_unix > SCRUB_INACTIVITY_SEC) {
        s_scrub_offset_sec = 0;
    }
    int64_t now_utc = real_now_utc + s_scrub_offset_sec;
    bool scrubbing = (s_scrub_offset_sec != 0);

    geo_fix_t fix = geo_current();

    time_t display_time = (time_t)now_utc;
    struct tm *lt = localtime(&display_time);
    if (scrubbing) {
        strftime(s_clock_buf, sizeof s_clock_buf, "*%H:%M", lt);
    } else {
        strftime(s_clock_buf, sizeof s_clock_buf,
                 clock_is_24h_style() ? "%H:%M" : "%I:%M", lt);
    }

    /* GPS recency indicator. Solid dot = fix < 1 h, hollow = < 24 h,
     * "?" = no fix or older than that. The user just needs reassurance
     * that location data is approximately right for the current spot. */
    if (fix.valid && fix.fix_unix > 0) {
        int64_t age_sec = real_now_utc - fix.fix_unix;
        if (age_sec < 0) age_sec = 0;
        char age[8];
        format_fix_age(fix.fix_unix, real_now_utc, age, sizeof age);
        const char *mark = (age_sec < 3600) ? "*"        /* recent: filled */
                         : (age_sec < 86400) ? "o"       /* stale: hollow */
                         : "?";                          /* very stale */
        snprintf(s_gps_buf, sizeof s_gps_buf, "%s %s", mark, age);
    } else {
        snprintf(s_gps_buf, sizeof s_gps_buf, "? seed");
    }

    sun_position_t p = sun_position(now_utc, fix.lat_deg, fix.lon_deg);
    s_last_sun_az  = p.azimuth;
    s_last_sun_alt = p.altitude;
    /* AZ/ALT shown only on the compass overlay view, not the main one. */
    int az_w = (int)p.azimuth;
    int az_f = (int)((p.azimuth - az_w) * 10);
    int alt_w = (int)p.altitude;
    int alt_f = (int)((p.altitude - alt_w) * 10);
    if (alt_f < 0) alt_f = -alt_f;
    snprintf(s_compass_az_alt_buf, sizeof s_compass_az_alt_buf,
             "AZ %d.%d %s   ALT %+d.%d",
             az_w, az_f, cardinal_8(p.azimuth), alt_w, alt_f);

    recompute_arc_if_needed(real_now_utc, &fix);
    ui_arc_set_now(now_utc);

    /* Bottom strip: single centred "HH:MM - HH:MM" for rise / set. The
     * arc itself already labels both ends so the text doesn't need to
     * repeat "rise" / "set". */
    const sun_day_events_t *e = ui_arc_events();
    char rh[8], sh[8];
    format_event_short(e->sunrise, fix.tz_offset_min, rh, sizeof rh, "");
    format_event_short(e->sunset,  fix.tz_offset_min, sh, sizeof sh, "");
    snprintf(s_events_buf, sizeof s_events_buf, "%s - %s", rh, sh);

    redraw_compass_view();
    layer_mark_dirty(window_get_root_layer(s_window));
}

/* --- Service callbacks ------------------------------------------------- */

static void on_minute_tick(struct tm *tick_time, TimeUnits units_changed) {
    (void)tick_time;
    (void)units_changed;
    refresh_view();
}

static void on_imu_sample(pbh_attitude_t att) {
    if (att.ready) ui_arc_set_pitch(att.pitch_deg);
}

static void on_compass_sample(pbh_compass_t sample) {
    s_compass = sample;
    /* Feed the sky-arc renderer so the main view rotates as the user
     * turns. When the compass isn't calibrated yet, fall back to
     * north-up (heading = -1) so the user still sees the day's path. */
    /* Only a fully calibrated reading is trustworthy; anything else
     * (DataInvalid, Calibrating, or the emulator's -1 "unavailable")
     * leaves the view auto-centred on solar noon. */
    if (sample.status == CompassStatusCalibrated) {
        ui_arc_set_heading(sample.heading_deg);
    } else {
        ui_arc_set_heading(-1.0f);
    }
    redraw_compass_view();
}

static void invalidate_events_cache(void) { s_events_day = -1; }

static void inbox_received(DictionaryIterator *iter, void *context) {
    (void)context;
    Tuple *t_lat   = dict_find(iter, MESSAGE_KEY_LatitudeE7);
    Tuple *t_lon   = dict_find(iter, MESSAGE_KEY_LongitudeE7);
    Tuple *t_tz    = dict_find(iter, MESSAGE_KEY_TzOffsetMin);
    Tuple *t_ts    = dict_find(iter, MESSAGE_KEY_LocationTimestamp);
    Tuple *t_decl  = dict_find(iter, MESSAGE_KEY_MagDeclinationE4);

    bool got_fix = (t_lat && t_lon && t_tz);
    if (got_fix) {
        float lat = (float)t_lat->value->int32 / 1.0e7f;
        float lon = (float)t_lon->value->int32 / 1.0e7f;
        int16_t tz = (int16_t)t_tz->value->int32;
        int64_t fix_ts = t_ts ? (int64_t)t_ts->value->int32 : (int64_t)(int32_t)time(NULL);
        geo_set(lat, lon, tz, fix_ts);
        invalidate_events_cache();
    }
    if (t_decl) {
        float decl = (float)t_decl->value->int32 / 1.0e4f;
        geo_set_mag_declination(decl);
        pbh_compass_set_declination(decl);
    }
    if (got_fix || t_decl) refresh_view();
}

static void inbox_dropped(AppMessageResult reason, void *context) {
    (void)context;
    APP_LOG(APP_LOG_LEVEL_WARNING, "AppMessage dropped: %d", reason);
}

static void request_fresh_location(void) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
    dict_write_uint8(iter, MESSAGE_KEY_RequestLocation, 1);
    app_message_outbox_send();
}

/* --- Click handlers ---------------------------------------------------- */

static void select_click(ClickRecognizerRef ref, void *context) {
    (void)ref; (void)context;
    s_mode = (s_mode + 1) % VIEW_COUNT;
    apply_mode();
}

static void scrub_by(int32_t delta_sec) {
    s_scrub_offset_sec += delta_sec;
    /* Clamp to +/- 24 hours so we stay inside today's arc cache. */
    if (s_scrub_offset_sec >  12 * 3600) s_scrub_offset_sec =  12 * 3600;
    if (s_scrub_offset_sec < -12 * 3600) s_scrub_offset_sec = -12 * 3600;
    s_scrub_last_input_unix = (int64_t)(int32_t)time(NULL);
    refresh_view();
}

static void up_click(ClickRecognizerRef ref, void *context) {
    (void)ref; (void)context;
    scrub_by(+SCRUB_STEP_SEC);
}

static void down_click(ClickRecognizerRef ref, void *context) {
    (void)ref; (void)context;
    scrub_by(-SCRUB_STEP_SEC);
}

static void up_long_click(ClickRecognizerRef ref, void *context) {
    (void)ref; (void)context;
    scrub_by(+60 * 60);
}

static void down_long_click(ClickRecognizerRef ref, void *context) {
    (void)ref; (void)context;
    scrub_by(-60 * 60);
}

static void click_config_provider(void *context) {
    (void)context;
    window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
    window_single_repeating_click_subscribe(BUTTON_ID_UP,   200, up_click);
    window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 200, down_click);
    window_long_click_subscribe(BUTTON_ID_UP,   500, up_long_click,   NULL);
    window_long_click_subscribe(BUTTON_ID_DOWN, 500, down_long_click, NULL);
}

/* --- Layer construction ------------------------------------------------ */

static TextLayer *mk_line(Layer *parent, GRect frame, const char *font_key,
                          GTextAlignment align, char *backing) {
    TextLayer *t = text_layer_create(frame);
    text_layer_set_background_color(t, GColorClear);
    text_layer_set_text_color(t, GColorBlack);
    text_layer_set_text_alignment(t, align);
    text_layer_set_font(t, fonts_get_system_font(font_key));
    backing[0] = '\0';
    text_layer_set_text(t, backing);
    layer_add_child(parent, text_layer_get_layer(t));
    return t;
}

static void build_data_view(Layer *parent, GRect b) {
    /* Header: clock on the left, compact GPS-recency indicator on the
     * right (filled/hollow dot + age, e.g. "* 5m" or "o 2h"). */
    s_clock_layer = mk_line(parent, GRect(0, 0, b.size.w * 60 / 100, 22),
                            FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentLeft, s_clock_buf);
    s_gps_layer   = mk_line(parent, GRect(b.size.w * 60 / 100, 4,
                                          b.size.w * 40 / 100 - 2, 16),
                            FONT_KEY_GOTHIC_14, GTextAlignmentRight, s_gps_buf);

    /* Arc canvas - given more vertical space now that the AZ/ALT row
     * is gone. Bottom strip is just sunrise / sunset. */
    int arc_y = 22;
    int arc_h = b.size.h - arc_y - 22;
    s_arc_canvas = ui_arc_layer_create(GRect(0, arc_y, b.size.w, arc_h));
    layer_add_child(parent, s_arc_canvas);

    /* Bottom strip: single centred "HH:MM - HH:MM". */
    int below = arc_y + arc_h + 2;
    s_events_layer = mk_line(parent, GRect(0, below, b.size.w, 18),
                             FONT_KEY_GOTHIC_14_BOLD, GTextAlignmentCenter,
                             s_events_buf);
}

static void build_compass_view(Layer *parent, GRect b) {
    /* Heading row at top. */
    s_hdg_layer = mk_line(parent, GRect(0, 0, b.size.w, 20),
                          FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentCenter, s_hdg_buf);

    /* Arrow occupies the middle square; size based on screen width. */
    int arrow_size = b.size.w - 32;
    int arrow_y    = 22;
    s_arrow_layer = layer_create(GRect((b.size.w - arrow_size) / 2,
                                       arrow_y, arrow_size, arrow_size));
    layer_set_update_proc(s_arrow_layer, arrow_update);
    layer_add_child(parent, s_arrow_layer);

    int below_y = arrow_y + arrow_size + 4;
    s_bearing_layer = mk_line(parent, GRect(0, below_y, b.size.w, 20),
                              FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentCenter,
                              s_bearing_buf);
    /* Full AZ/ALT line for users who do want the scientific readout. */
    s_compass_az_alt_layer = mk_line(parent, GRect(0, below_y + 20, b.size.w, 16),
                                     FONT_KEY_GOTHIC_14, GTextAlignmentCenter,
                                     s_compass_az_alt_buf);
    s_status_layer  = mk_line(parent, GRect(0, below_y + 36, b.size.w, 16),
                              FONT_KEY_GOTHIC_14, GTextAlignmentCenter,
                              s_status_buf);
}

static void window_load(Window *window) {
    window_set_background_color(window, GColorWhite);
    Layer *root = window_get_root_layer(window);
    GRect b = layer_get_bounds(root);

    s_data_root = layer_create(b);
    s_compass_root = layer_create(b);
    layer_add_child(root, s_data_root);
    layer_add_child(root, s_compass_root);

    s_arrow_path = gpath_create(&ARROW_INFO);

    build_data_view(s_data_root, b);
    build_compass_view(s_compass_root, b);

    apply_mode();
    window_set_click_config_provider(window, click_config_provider);
}

static void window_unload(Window *window) {
    (void)window;
    text_layer_destroy(s_clock_layer);
    text_layer_destroy(s_gps_layer);
    text_layer_destroy(s_events_layer);
    ui_arc_layer_destroy(s_arc_canvas);
    text_layer_destroy(s_hdg_layer);
    text_layer_destroy(s_bearing_layer);
    text_layer_destroy(s_compass_az_alt_layer);
    text_layer_destroy(s_status_layer);
    layer_destroy(s_arrow_layer);
    gpath_destroy(s_arrow_path);
    layer_destroy(s_data_root);
    layer_destroy(s_compass_root);
}

static void init(void) {
    geo_init();

    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers){
        .load = window_load,
        .unload = window_unload,
    });
    window_stack_push(s_window, true);

    app_message_register_inbox_received(inbox_received);
    app_message_register_inbox_dropped(inbox_dropped);
    app_message_open(256, 32);

    pbh_compass_init(on_compass_sample, geo_mag_declination_deg());
    pbh_imu_init(on_imu_sample);

    refresh_view();
    request_fresh_location();
    tick_timer_service_subscribe(MINUTE_UNIT, on_minute_tick);
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    pbh_compass_deinit();
    pbh_imu_deinit();
    app_message_deregister_callbacks();
    window_destroy(s_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}

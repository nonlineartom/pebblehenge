#include <pebble.h>

#include "sun.h"
#include "geo.h"
#include "compass.h"

/* --- App-wide state ---------------------------------------------------- */

typedef enum {
    VIEW_DATA = 0,
    VIEW_COMPASS = 1,
    VIEW_COUNT
} view_mode_t;

static view_mode_t  s_mode = VIEW_DATA;
static float        s_last_sun_az  = 0.0f;
static float        s_last_sun_alt = 0.0f;
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
static TextLayer  *s_loc_layer;
static TextLayer  *s_az_layer;
static TextLayer  *s_alt_layer;
static TextLayer  *s_rise_layer;
static TextLayer  *s_noon_layer;
static TextLayer  *s_set_layer;

static TextLayer  *s_hdg_layer;
static TextLayer  *s_bearing_layer;
static TextLayer  *s_status_layer;

/* --- Text buffers ------------------------------------------------------- */

static char s_clock_buf[8];
static char s_loc_buf[24];
static char s_az_buf[20];
static char s_alt_buf[16];
static char s_rise_buf[20];
static char s_noon_buf[20];
static char s_set_buf[20];

static char s_hdg_buf[16];
static char s_bearing_buf[24];
static char s_status_buf[28];

/* --- Cached day events -------------------------------------------------- */

static sun_day_events_t s_events;
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

static void format_local_hm(int64_t unix_utc, int tz_offset_min, char *out, size_t n) {
    if (unix_utc == SUN_NEVER_RISES) { snprintf(out, n, "--:--"); return; }
    if (unix_utc == SUN_NEVER_SETS)  { snprintf(out, n, "always"); return; }
    int64_t local = unix_utc + (int64_t)tz_offset_min * 60;
    int sec_of_day = (int)(((local % 86400) + 86400) % 86400);
    int hh = sec_of_day / 3600;
    int mm = (sec_of_day % 3600) / 60;
    snprintf(out, n, "%02d:%02d", hh, mm);
}

static void format_event_line(int64_t unix_utc, int tz_offset_min,
                               const char *label, char *out, size_t n) {
    char hm[8];
    format_local_hm(unix_utc, tz_offset_min, hm, sizeof hm);
    snprintf(out, n, "%s  %s", label, hm);
}

static void format_fix_age(int64_t fix_unix, int64_t now_utc, char *out, size_t n) {
    int64_t age = now_utc - fix_unix;
    if (age < 60)         snprintf(out, n, "%ds", (int)age);
    else if (age < 3600)  snprintf(out, n, "%dm", (int)(age / 60));
    else if (age < 86400) snprintf(out, n, "%dh", (int)(age / 3600));
    else                  snprintf(out, n, "%dd", (int)(age / 86400));
}

static void recompute_events_if_needed(int64_t now_utc, const geo_fix_t *fix) {
    int local_day = (int)(((now_utc + (int64_t)fix->tz_offset_min * 60) / 86400) & 0xFFFF);
    if (local_day == s_events_day) return;
    s_events = sun_day_events(now_utc, fix->lat_deg, fix->lon_deg, fix->tz_offset_min);
    s_events_day = local_day;
    format_event_line(s_events.sunrise,    fix->tz_offset_min, "Rise", s_rise_buf, sizeof s_rise_buf);
    format_event_line(s_events.solar_noon, fix->tz_offset_min, "Noon", s_noon_buf, sizeof s_noon_buf);
    format_event_line(s_events.sunset,     fix->tz_offset_min, "Set ", s_set_buf,  sizeof s_set_buf);
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

static void refresh_view(void) {
    time_t now;
    time(&now);
    int64_t now_utc = (int64_t)now;

    geo_fix_t fix = geo_current();

    struct tm *lt = localtime(&now);
    strftime(s_clock_buf, sizeof s_clock_buf,
             clock_is_24h_style() ? "%H:%M" : "%I:%M", lt);

    if (fix.valid && fix.fix_unix > 0) {
        char age[8];
        format_fix_age(fix.fix_unix, now_utc, age, sizeof age);
        snprintf(s_loc_buf, sizeof s_loc_buf, "%.2f,%.2f %s",
                 fix.lat_deg, fix.lon_deg, age);
    } else {
        snprintf(s_loc_buf, sizeof s_loc_buf, "seed %.2f,%.2f",
                 fix.lat_deg, fix.lon_deg);
    }

    sun_position_t p = sun_position(now_utc, fix.lat_deg, fix.lon_deg);
    s_last_sun_az  = p.azimuth;
    s_last_sun_alt = p.altitude;
    snprintf(s_az_buf,  sizeof s_az_buf,  "AZ %5.1f %s", p.azimuth, cardinal_8(p.azimuth));
    snprintf(s_alt_buf, sizeof s_alt_buf, "ALT %+5.1f", p.altitude);

    recompute_events_if_needed(now_utc, &fix);

    redraw_compass_view();
    layer_mark_dirty(window_get_root_layer(s_window));
}

/* --- Service callbacks ------------------------------------------------- */

static void on_minute_tick(struct tm *tick_time, TimeUnits units_changed) {
    (void)tick_time;
    (void)units_changed;
    refresh_view();
}

static void on_compass_sample(pbh_compass_t sample) {
    s_compass = sample;
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
        int64_t fix_ts = t_ts ? (int64_t)t_ts->value->int32 : (int64_t)time(NULL);
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

static void click_config_provider(void *context) {
    (void)context;
    window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
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
    int y = 0;
    s_clock_layer = mk_line(parent, GRect(0, y, b.size.w, 24),
                            FONT_KEY_GOTHIC_24_BOLD, GTextAlignmentCenter, s_clock_buf);
    y += 24;
    s_loc_layer   = mk_line(parent, GRect(0, y, b.size.w, 16),
                            FONT_KEY_GOTHIC_14, GTextAlignmentCenter, s_loc_buf);
    y += 18;
    s_az_layer    = mk_line(parent, GRect(0, y, b.size.w, 22),
                            FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentLeft, s_az_buf);
    y += 22;
    s_alt_layer   = mk_line(parent, GRect(0, y, b.size.w, 22),
                            FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentLeft, s_alt_buf);
    y += 26;
    s_rise_layer  = mk_line(parent, GRect(0, y, b.size.w, 18),
                            FONT_KEY_GOTHIC_14, GTextAlignmentLeft, s_rise_buf);
    y += 18;
    s_noon_layer  = mk_line(parent, GRect(0, y, b.size.w, 18),
                            FONT_KEY_GOTHIC_14, GTextAlignmentLeft, s_noon_buf);
    y += 18;
    s_set_layer   = mk_line(parent, GRect(0, y, b.size.w, 18),
                            FONT_KEY_GOTHIC_14, GTextAlignmentLeft, s_set_buf);
}

static void build_compass_view(Layer *parent, GRect b) {
    /* Heading row at top. */
    s_hdg_layer = mk_line(parent, GRect(0, 0, b.size.w, 20),
                          FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentCenter, s_hdg_buf);

    /* Arrow occupies the middle square; size based on screen width. */
    int arrow_size = b.size.w - 24;
    int arrow_y    = 24;
    s_arrow_layer = layer_create(GRect((b.size.w - arrow_size) / 2,
                                       arrow_y, arrow_size, arrow_size));
    layer_set_update_proc(s_arrow_layer, arrow_update);
    layer_add_child(parent, s_arrow_layer);

    int below_y = arrow_y + arrow_size + 4;
    s_bearing_layer = mk_line(parent, GRect(0, below_y, b.size.w, 20),
                              FONT_KEY_GOTHIC_18_BOLD, GTextAlignmentCenter,
                              s_bearing_buf);
    s_status_layer  = mk_line(parent, GRect(0, below_y + 22, b.size.w, 16),
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
    text_layer_destroy(s_loc_layer);
    text_layer_destroy(s_az_layer);
    text_layer_destroy(s_alt_layer);
    text_layer_destroy(s_rise_layer);
    text_layer_destroy(s_noon_layer);
    text_layer_destroy(s_set_layer);
    text_layer_destroy(s_hdg_layer);
    text_layer_destroy(s_bearing_layer);
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

    refresh_view();
    request_fresh_location();
    tick_timer_service_subscribe(MINUTE_UNIT, on_minute_tick);
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    pbh_compass_deinit();
    app_message_deregister_callbacks();
    window_destroy(s_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}

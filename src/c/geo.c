/* Geo module: holds the current location/timezone/declination, loads
 * persisted values on launch and saves them whenever a fresh fix arrives. */

#include <pebble.h>
#include "geo.h"

/* Default seed location until the phone supplies a real fix. */
#define DEFAULT_LAT_DEG        40.7128f
#define DEFAULT_LON_DEG       -74.0060f
#define DEFAULT_TZ_OFFSET_MIN  (-240)

/* Persistent storage keys. PERSIST_KEY_VERSION is bumped when the schema
 * changes so old data is ignored cleanly. */
#define PERSIST_KEY_VERSION    0
#define PERSIST_KEY_LAT        1
#define PERSIST_KEY_LON        2
#define PERSIST_KEY_TZ         3
#define PERSIST_KEY_FIX_UNIX   4
#define PERSIST_KEY_DECL       5
#define PERSIST_SCHEMA_VERSION 1

static geo_fix_t s_fix;
static float     s_mag_decl_deg;

static void seed_defaults(void) {
    s_fix.lat_deg       = DEFAULT_LAT_DEG;
    s_fix.lon_deg       = DEFAULT_LON_DEG;
    s_fix.tz_offset_min = DEFAULT_TZ_OFFSET_MIN;
    s_fix.fix_unix      = 0;
    s_fix.valid         = false;
    s_mag_decl_deg      = 0.0f;
}

void geo_load(void) {
    seed_defaults();
    if (!persist_exists(PERSIST_KEY_VERSION)) return;
    if (persist_read_int(PERSIST_KEY_VERSION) != PERSIST_SCHEMA_VERSION) return;

    if (persist_exists(PERSIST_KEY_LAT) && persist_exists(PERSIST_KEY_LON)) {
        int32_t lat_e7 = persist_read_int(PERSIST_KEY_LAT);
        int32_t lon_e7 = persist_read_int(PERSIST_KEY_LON);
        s_fix.lat_deg = (float)lat_e7 / 1.0e7f;
        s_fix.lon_deg = (float)lon_e7 / 1.0e7f;
        s_fix.tz_offset_min = (int16_t)persist_read_int(PERSIST_KEY_TZ);
        s_fix.fix_unix = persist_read_int(PERSIST_KEY_FIX_UNIX);
        s_fix.valid = true;
    }
    if (persist_exists(PERSIST_KEY_DECL)) {
        int32_t decl_e4 = persist_read_int(PERSIST_KEY_DECL);
        s_mag_decl_deg = (float)decl_e4 / 1.0e4f;
    }
}

void geo_save(void) {
    persist_write_int(PERSIST_KEY_VERSION, PERSIST_SCHEMA_VERSION);
    persist_write_int(PERSIST_KEY_LAT, (int32_t)(s_fix.lat_deg * 1.0e7f));
    persist_write_int(PERSIST_KEY_LON, (int32_t)(s_fix.lon_deg * 1.0e7f));
    persist_write_int(PERSIST_KEY_TZ,  s_fix.tz_offset_min);
    persist_write_int(PERSIST_KEY_FIX_UNIX, (int32_t)s_fix.fix_unix);
    persist_write_int(PERSIST_KEY_DECL, (int32_t)(s_mag_decl_deg * 1.0e4f));
}

void geo_init(void) {
    geo_load();
}

geo_fix_t geo_current(void) {
    return s_fix;
}

void geo_set(float lat_deg, float lon_deg, int16_t tz_offset_min, int64_t fix_unix) {
    s_fix.lat_deg       = lat_deg;
    s_fix.lon_deg       = lon_deg;
    s_fix.tz_offset_min = tz_offset_min;
    s_fix.fix_unix      = fix_unix;
    s_fix.valid         = true;
    geo_save();
}

float geo_mag_declination_deg(void) {
    return s_mag_decl_deg;
}

void geo_set_mag_declination(float decl_deg) {
    s_mag_decl_deg = decl_deg;
    geo_save();
}

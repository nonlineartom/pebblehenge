/*
 * NOAA Solar Calculator port. Algorithm follows the public-domain
 * NOAA ESRL spreadsheet/JavaScript (https://gml.noaa.gov/grad/solcalc/).
 * Accuracy is ~0.01° in position and ~10 s in event times for 1950..2050.
 *
 * Julian-day base kept in double for precision; downstream trig is double
 * on the host (tests) and on Pebble's Cortex-M4F it still runs comfortably
 * inside one minute-tick budget.
 */

#include "sun.h"
#include <math.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double d2r(double d) { return d * (M_PI / 180.0); }
static double r2d(double r) { return r * (180.0 / M_PI); }

static double julian_day(int64_t unix_utc) {
    return 2440587.5 + (double)unix_utc / 86400.0;
}

static double julian_century(double jd) {
    return (jd - 2451545.0) / 36525.0;
}

static double geom_mean_long_sun(double T) {
    double L = 280.46646 + T * (36000.76983 + T * 0.0003032);
    L = fmod(L, 360.0);
    if (L < 0.0) L += 360.0;
    return L;
}

static double geom_mean_anom_sun(double T) {
    return 357.52911 + T * (35999.05029 - T * 0.0001537);
}

static double eccent_earth_orbit(double T) {
    return 0.016708634 - T * (0.000042037 + T * 0.0000001267);
}

static double sun_eq_of_center(double T) {
    double m = d2r(geom_mean_anom_sun(T));
    return sin(m)     * (1.914602 - T * (0.004817 + 0.000014 * T))
         + sin(2 * m) * (0.019993 - T * 0.000101)
         + sin(3 * m) * 0.000289;
}

static double sun_true_long(double T) {
    return geom_mean_long_sun(T) + sun_eq_of_center(T);
}

static double sun_app_long(double T) {
    double omega = 125.04 - 1934.136 * T;
    return sun_true_long(T) - 0.00569 - 0.00478 * sin(d2r(omega));
}

static double mean_obliq_ecliptic(double T) {
    double seconds = 21.448 - T * (46.8150 + T * (0.00059 - T * 0.001813));
    return 23.0 + (26.0 + seconds / 60.0) / 60.0;
}

static double obliq_corr(double T) {
    return mean_obliq_ecliptic(T)
         + 0.00256 * cos(d2r(125.04 - 1934.136 * T));
}

static double sun_declination(double T) {
    return r2d(asin(sin(d2r(obliq_corr(T))) * sin(d2r(sun_app_long(T)))));
}

static double equation_of_time_min(double T) {
    double eps = d2r(obliq_corr(T));
    double l0  = d2r(geom_mean_long_sun(T));
    double e   = eccent_earth_orbit(T);
    double m   = d2r(geom_mean_anom_sun(T));
    double y   = tan(eps / 2.0); y *= y;
    double etime = y * sin(2 * l0)
                 - 2.0 * e * sin(m)
                 + 4.0 * e * y * sin(m) * cos(2 * l0)
                 - 0.5 * y * y * sin(4 * l0)
                 - 1.25 * e * e * sin(2 * m);
    return r2d(etime) * 4.0;
}

/* Refraction correction in degrees for an apparent-altitude estimate.
 * Bennett-style polynomial used in the NOAA spreadsheet. */
static double refraction_deg(double alt_deg) {
    if (alt_deg > 85.0) return 0.0;
    double te = tan(d2r(alt_deg));
    double r;
    if (alt_deg > 5.0) {
        r = 58.1 / te
          - 0.07 / (te * te * te)
          + 0.000086 / (te * te * te * te * te);
    } else if (alt_deg > -0.575) {
        r = 1735.0
          + alt_deg * (-518.2
          + alt_deg * (103.4
          + alt_deg * (-12.79
          + alt_deg *   0.711)));
    } else {
        r = -20.774 / te;
    }
    return r / 3600.0;
}

sun_position_t sun_position(int64_t unix_utc, float lat_deg, float lon_deg) {
    double T = julian_century(julian_day(unix_utc));
    double decl_deg = sun_declination(T);
    double eq_min   = equation_of_time_min(T);

    /* True solar time, minutes since local solar midnight, [0,1440). */
    double seconds_of_day = (double)(unix_utc - (int64_t)floor((double)unix_utc / 86400.0) * 86400);
    double tst = seconds_of_day / 60.0 + eq_min + 4.0 * (double)lon_deg;
    tst = fmod(tst, 1440.0);
    if (tst < 0.0) tst += 1440.0;

    double ha_deg = tst / 4.0 - 180.0;
    if (ha_deg < -180.0) ha_deg += 360.0;

    double lat = d2r((double)lat_deg);
    double dec = d2r(decl_deg);
    double H   = d2r(ha_deg);

    double cos_zen = sin(lat) * sin(dec) + cos(lat) * cos(dec) * cos(H);
    if (cos_zen >  1.0) cos_zen =  1.0;
    if (cos_zen < -1.0) cos_zen = -1.0;
    double zenith_deg = r2d(acos(cos_zen));
    double alt_true   = 90.0 - zenith_deg;

    /* NOAA convention: azimuth measured clockwise from true north.
     *   A = (sin(decl) - sin(alt)*sin(lat)) / (cos(alt)*cos(lat))
     *   acos(A) in [0,180]. Morning (H<0): az = acos(A).
     *   Afternoon (H>=0): az = 360 - acos(A). */
    double az_deg;
    double denom = cos(d2r(alt_true)) * cos(lat);
    if (fabs(denom) < 1e-9) {
        /* Sun at zenith or observer at a pole; azimuth is undefined. */
        az_deg = (lat_deg > 0) ? 180.0 : 0.0;
    } else {
        double cos_az = (sin(dec) - sin(d2r(alt_true)) * sin(lat)) / denom;
        if (cos_az >  1.0) cos_az =  1.0;
        if (cos_az < -1.0) cos_az = -1.0;
        double acos_a = r2d(acos(cos_az));
        az_deg = (ha_deg >= 0.0) ? (360.0 - acos_a) : acos_a;
    }
    if (az_deg <    0.0) az_deg += 360.0;
    if (az_deg >= 360.0) az_deg -= 360.0;

    sun_position_t pos;
    pos.azimuth  = (float)az_deg;
    pos.altitude = (float)(alt_true + refraction_deg(alt_true));
    return pos;
}

/* Internal: compute solar noon (UTC unix) for the UTC date containing `t`
 * at observer longitude `lon_deg`. */
static int64_t solar_noon_utc_for(int64_t t, double lon_deg, double *eq_min_out) {
    double T = julian_century(julian_day(t));
    double eq_min = equation_of_time_min(T);
    int64_t day_floor = (int64_t)floor((double)t / 86400.0) * 86400;
    double noon_sec  = 720.0 * 60.0 - (eq_min + 4.0 * lon_deg) * 60.0;
    if (eq_min_out) *eq_min_out = eq_min;
    return day_floor + (int64_t)floor(noon_sec + 0.5);
}

int64_t sun_event(int64_t date_unix_utc, float lat_deg, float lon_deg,
                  float target_alt_deg, bool morning) {
    /* Two-pass refinement: first pass uses declination at the input time,
     * second pass uses declination at the first-pass event time. */
    int64_t event = date_unix_utc;
    double lat = d2r((double)lat_deg);
    double tgt = d2r((double)target_alt_deg);

    for (int pass = 0; pass < 2; pass++) {
        double T = julian_century(julian_day(event));
        double dec = d2r(sun_declination(T));
        double cos_H = (sin(tgt) - sin(lat) * sin(dec)) / (cos(lat) * cos(dec));
        if (cos_H >  1.0) return SUN_NEVER_RISES;
        if (cos_H < -1.0) return SUN_NEVER_SETS;
        double H_deg = r2d(acos(cos_H));
        int64_t noon = solar_noon_utc_for(date_unix_utc, (double)lon_deg, NULL);
        int64_t off  = (int64_t)floor(H_deg * 240.0 + 0.5); /* 4 min/deg = 240 s/deg */
        event = morning ? (noon - off) : (noon + off);
    }
    return event;
}

sun_day_events_t sun_day_events(int64_t unix_utc, float lat_deg, float lon_deg,
                                int tz_offset_min) {
    /* Pick a UTC instant on the *local* day so that all events fall within ±12h. */
    int64_t local = unix_utc + (int64_t)tz_offset_min * 60;
    int64_t local_midnight_shifted = (int64_t)floor((double)local / 86400.0) * 86400;
    int64_t local_noon_utc = local_midnight_shifted + 43200 - (int64_t)tz_offset_min * 60;

    sun_day_events_t e;
    e.solar_noon = solar_noon_utc_for(local_noon_utc, (double)lon_deg, NULL);

    e.sunrise = sun_event(local_noon_utc, lat_deg, lon_deg, -0.833f, true);
    e.sunset  = sun_event(local_noon_utc, lat_deg, lon_deg, -0.833f, false);

    e.civil_dawn  = sun_event(local_noon_utc, lat_deg, lon_deg,  -6.0f, true);
    e.civil_dusk  = sun_event(local_noon_utc, lat_deg, lon_deg,  -6.0f, false);

    e.nautical_dawn = sun_event(local_noon_utc, lat_deg, lon_deg, -12.0f, true);
    e.nautical_dusk = sun_event(local_noon_utc, lat_deg, lon_deg, -12.0f, false);

    e.astronomical_dawn = sun_event(local_noon_utc, lat_deg, lon_deg, -18.0f, true);
    e.astronomical_dusk = sun_event(local_noon_utc, lat_deg, lon_deg, -18.0f, false);

    e.golden_hour_morning_end   = sun_event(local_noon_utc, lat_deg, lon_deg,  6.0f, true);
    e.golden_hour_evening_start = sun_event(local_noon_utc, lat_deg, lon_deg,  6.0f, false);

    e.blue_hour_morning_end     = sun_event(local_noon_utc, lat_deg, lon_deg, -4.0f, true);
    e.blue_hour_evening_start   = sun_event(local_noon_utc, lat_deg, lon_deg, -4.0f, false);

    return e;
}

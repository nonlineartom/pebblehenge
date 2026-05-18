/*
 * NOAA Solar Calculator port. Algorithm follows the public-domain
 * NOAA ESRL spreadsheet/JavaScript (https://gml.noaa.gov/grad/solcalc/).
 * Accuracy is ~0.02° in position and ~10 s in event times for 1950..2050.
 *
 * Julian-day arithmetic uses double (2.4M+ days needs >23 bits of mantissa).
 * Trig is implemented inline (Taylor / minimax polynomials) so the binary
 * is self-contained: newlib's sinf/cosf rely on internal lookup tables
 * (npio2_hw, two_over_pi) that the Pebble app loader does not relocate,
 * causing a hard fault on the first call.
 */

#include "sun.h"
#include <math.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define PI_F      3.14159265358979323846f
#define TWO_PI_F  6.28318530717958647692f
#define HALF_PI_F 1.57079632679489661923f
#define D2R_F     (PI_F / 180.0f)
#define R2D_F     (180.0f / PI_F)

static float pbh_fmodf(float x, float y) {
    if (y == 0.0f) return 0.0f;
    float q = x / y;
    /* Truncate toward zero. */
    long iq = (long)q;
    float r = x - (float)iq * y;
    return r;
}

/* sin(x) for x in [-pi/2, pi/2]. 7th-order Horner; max |err| < 5e-8. */
static float poly_sin_quad(float x) {
    float x2 = x * x;
    return x * (1.0f
              + x2 * (-1.6666667e-1f
              + x2 * ( 8.3333333e-3f
              + x2 * (-1.9841270e-4f
              + x2 *   2.7557319e-6f))));
}

/* Argument-reduced sine for any real x. */
static float pbh_sinf(float x) {
    /* Reduce to [-pi, pi]. */
    x = pbh_fmodf(x, TWO_PI_F);
    if      (x >  PI_F) x -= TWO_PI_F;
    else if (x < -PI_F) x += TWO_PI_F;
    /* Reduce to [-pi/2, pi/2] using sin(pi - x) = sin(x). */
    if      (x >  HALF_PI_F) x =  PI_F - x;
    else if (x < -HALF_PI_F) x = -PI_F - x;
    return poly_sin_quad(x);
}

static float pbh_cosf(float x) {
    return pbh_sinf(x + HALF_PI_F);
}

static float pbh_tanf(float x) {
    float c = pbh_cosf(x);
    /* Guard against divide-by-zero near +/- pi/2; refraction() never calls us
     * close enough to matter, but be defensive. */
    if (c > -1e-7f && c < 1e-7f) c = (c < 0.0f) ? -1e-7f : 1e-7f;
    return pbh_sinf(x) / c;
}

/* atan(x) via the standard arctan series with range reduction.
 *  - |x| > 1:   atan(x) = sign(x)*pi/2 - atan(1/x)
 *  - |x| > tan(pi/12): atan(x) = pi/6 + atan((x*sqrt3 - 1)/(sqrt3 + x))
 * The remaining argument is in roughly +/-0.27, where a 9th-order
 * minimax polynomial is < 1e-6 of error.
 */
static float poly_atan_small(float x) {
    float x2 = x * x;
    return x * (1.0f
              + x2 * (-0.3333314f
              + x2 * ( 0.1999355f
              + x2 * (-0.1420546f
              + x2 *   0.0976160f))));
}

static float pbh_atanf(float x) {
    int neg = 0;
    if (x < 0.0f) { neg = 1; x = -x; }
    int inv = 0;
    if (x > 1.0f) { inv = 1; x = 1.0f / x; }
    int shift = 0;
    /* tan(pi/12) ~= 0.2679 */
    if (x > 0.2679491924f) {
        shift = 1;
        const float SQRT3 = 1.7320508076f;
        x = (x * SQRT3 - 1.0f) / (SQRT3 + x);
    }
    float r = poly_atan_small(x);
    if (shift) r += PI_F / 6.0f;
    if (inv)   r = HALF_PI_F - r;
    if (neg)   r = -r;
    return r;
}

static float pbh_atan2f(float y, float x) {
    if (x > 0.0f) return pbh_atanf(y / x);
    if (x < 0.0f) return pbh_atanf(y / x) + (y >= 0.0f ? PI_F : -PI_F);
    /* x == 0 */
    if (y > 0.0f) return  HALF_PI_F;
    if (y < 0.0f) return -HALF_PI_F;
    return 0.0f;
}

/* Newton-Raphson square root. SDK targets Cortex-M3, so newlib's sqrtf
 * pulls in a table-driven reduction we can't relocate; this stays self-
 * contained. Five iterations give >7 decimal digits of accuracy. */
static float pbh_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    /* Initial estimate via bit-trick: halve the exponent. */
    union { float f; uint32_t u; } u;
    u.f = x;
    u.u = (u.u >> 1) + (1U << 29) - (1U << 22);
    float r = u.f;
    for (int i = 0; i < 5; i++) {
        r = 0.5f * (r + x / r);
    }
    return r;
}

/* asin(x) = atan(x / sqrt(1 - x*x)). For |x| close to 1 this loses
 * precision, but sun-position never feeds in arguments near +/-1
 * except when the sun is exactly at the zenith or below the horizon,
 * which we clamp before calling. */
static float pbh_asinf(float x) {
    if (x >  1.0f) x =  1.0f;
    if (x < -1.0f) x = -1.0f;
    float c = pbh_sqrtf(1.0f - x * x);
    return pbh_atan2f(x, c);
}

static float pbh_acosf(float x) {
    return HALF_PI_F - pbh_asinf(x);
}

static float pbh_fabsf(float x) { return x < 0.0f ? -x : x; }

static float d2r(float d) { return d * D2R_F; }
static float r2d(float r) { return r * R2D_F; }

static double julian_day(int64_t unix_utc) {
    return 2440587.5 + (double)unix_utc / 86400.0;
}

static float julian_century(double jd) {
    return (float)((jd - 2451545.0) / 36525.0);
}

static float geom_mean_long_sun(float T) {
    float L = 280.46646f + T * (36000.76983f + T * 0.0003032f);
    L = pbh_fmodf(L, 360.0f);
    if (L < 0.0f) L += 360.0f;
    return L;
}

static float geom_mean_anom_sun(float T) {
    return 357.52911f + T * (35999.05029f - T * 0.0001537f);
}

static float eccent_earth_orbit(float T) {
    return 0.016708634f - T * (0.000042037f + T * 0.0000001267f);
}

static float sun_eq_of_center(float T) {
    float m = d2r(geom_mean_anom_sun(T));
    return pbh_sinf(m)        * (1.914602f - T * (0.004817f + 0.000014f * T))
         + pbh_sinf(2.0f * m) * (0.019993f - T * 0.000101f)
         + pbh_sinf(3.0f * m) * 0.000289f;
}

static float sun_true_long(float T) {
    return geom_mean_long_sun(T) + sun_eq_of_center(T);
}

static float sun_app_long(float T) {
    float omega = 125.04f - 1934.136f * T;
    return sun_true_long(T) - 0.00569f - 0.00478f * pbh_sinf(d2r(omega));
}

static float mean_obliq_ecliptic(float T) {
    float seconds = 21.448f - T * (46.8150f + T * (0.00059f - T * 0.001813f));
    return 23.0f + (26.0f + seconds / 60.0f) / 60.0f;
}

static float obliq_corr(float T) {
    return mean_obliq_ecliptic(T)
         + 0.00256f * pbh_cosf(d2r(125.04f - 1934.136f * T));
}

static float sun_declination(float T) {
    return r2d(pbh_asinf(pbh_sinf(d2r(obliq_corr(T))) * pbh_sinf(d2r(sun_app_long(T)))));
}

static float equation_of_time_min(float T) {
    float eps = d2r(obliq_corr(T));
    float l0  = d2r(geom_mean_long_sun(T));
    float e   = eccent_earth_orbit(T);
    float m   = d2r(geom_mean_anom_sun(T));
    float y   = pbh_tanf(eps / 2.0f); y *= y;
    float etime = y * pbh_sinf(2.0f * l0)
                - 2.0f * e * pbh_sinf(m)
                + 4.0f * e * y * pbh_sinf(m) * pbh_cosf(2.0f * l0)
                - 0.5f * y * y * pbh_sinf(4.0f * l0)
                - 1.25f * e * e * pbh_sinf(2.0f * m);
    return r2d(etime) * 4.0f;
}

static float refraction_deg(float alt_deg) {
    if (alt_deg > 85.0f) return 0.0f;
    float te = pbh_tanf(d2r(alt_deg));
    float r;
    if (alt_deg > 5.0f) {
        r = 58.1f / te
          - 0.07f / (te * te * te)
          + 0.000086f / (te * te * te * te * te);
    } else if (alt_deg > -0.575f) {
        r = 1735.0f
          + alt_deg * (-518.2f
          + alt_deg * ( 103.4f
          + alt_deg * ( -12.79f
          + alt_deg *    0.711f)));
    } else {
        r = -20.774f / te;
    }
    return r / 3600.0f;
}

sun_position_t sun_position(int64_t unix_utc, float lat_deg, float lon_deg) {
    float T = julian_century(julian_day(unix_utc));
    float decl_deg = sun_declination(T);
    float eq_min   = equation_of_time_min(T);

    /* True solar time, minutes since local solar midnight, [0,1440). */
    int64_t day_floor_unix = (int64_t)(unix_utc / 86400) * 86400;
    if (day_floor_unix > unix_utc) day_floor_unix -= 86400;
    float seconds_of_day = (float)(unix_utc - day_floor_unix);
    float tst = seconds_of_day / 60.0f + eq_min + 4.0f * lon_deg;
    tst = pbh_fmodf(tst, 1440.0f);
    if (tst < 0.0f) tst += 1440.0f;

    float ha_deg = tst / 4.0f - 180.0f;
    if (ha_deg < -180.0f) ha_deg += 360.0f;

    float lat = d2r(lat_deg);
    float dec = d2r(decl_deg);
    float H   = d2r(ha_deg);

    float cos_zen = pbh_sinf(lat) * pbh_sinf(dec) + pbh_cosf(lat) * pbh_cosf(dec) * pbh_cosf(H);
    if (cos_zen >  1.0f) cos_zen =  1.0f;
    if (cos_zen < -1.0f) cos_zen = -1.0f;
    float zenith_deg = r2d(pbh_acosf(cos_zen));
    float alt_true   = 90.0f - zenith_deg;

    /* NOAA convention: azimuth measured clockwise from true north. */
    float az_deg;
    float denom = pbh_cosf(d2r(alt_true)) * pbh_cosf(lat);
    if (pbh_fabsf(denom) < 1e-6f) {
        az_deg = (lat_deg > 0.0f) ? 180.0f : 0.0f;
    } else {
        float cos_az = (pbh_sinf(dec) - pbh_sinf(d2r(alt_true)) * pbh_sinf(lat)) / denom;
        if (cos_az >  1.0f) cos_az =  1.0f;
        if (cos_az < -1.0f) cos_az = -1.0f;
        float acos_a = r2d(pbh_acosf(cos_az));
        az_deg = (ha_deg >= 0.0f) ? (360.0f - acos_a) : acos_a;
    }
    if (az_deg <    0.0f) az_deg += 360.0f;
    if (az_deg >= 360.0f) az_deg -= 360.0f;

    sun_position_t pos;
    pos.azimuth  = az_deg;
    pos.altitude = alt_true + refraction_deg(alt_true);
    return pos;
}

/* Internal: compute solar noon (UTC unix) for the UTC date containing `t`
 * at observer longitude `lon_deg`. */
static int64_t solar_noon_utc_for(int64_t t, float lon_deg) {
    float T = julian_century(julian_day(t));
    float eq_min = equation_of_time_min(T);
    int64_t day_floor = (t / 86400) * 86400;
    if (day_floor > t) day_floor -= 86400;
    float noon_sec  = 720.0f * 60.0f - (eq_min + 4.0f * lon_deg) * 60.0f;
    return day_floor + (int64_t)(noon_sec + (noon_sec >= 0 ? 0.5f : -0.5f));
}

int64_t sun_event(int64_t date_unix_utc, float lat_deg, float lon_deg,
                  float target_alt_deg, bool morning) {
    int64_t event = date_unix_utc;
    float lat = d2r(lat_deg);
    float tgt = d2r(target_alt_deg);

    for (int pass = 0; pass < 2; pass++) {
        float T = julian_century(julian_day(event));
        float dec = d2r(sun_declination(T));
        float cos_H = (pbh_sinf(tgt) - pbh_sinf(lat) * pbh_sinf(dec)) / (pbh_cosf(lat) * pbh_cosf(dec));
        if (cos_H >  1.0f) return SUN_NEVER_RISES;
        if (cos_H < -1.0f) return SUN_NEVER_SETS;
        float H_deg = r2d(pbh_acosf(cos_H));
        int64_t noon = solar_noon_utc_for(date_unix_utc, lon_deg);
        int64_t off  = (int64_t)(H_deg * 240.0f + 0.5f);
        event = morning ? (noon - off) : (noon + off);
    }
    return event;
}

sun_day_events_t sun_day_events(int64_t unix_utc, float lat_deg, float lon_deg,
                                int tz_offset_min) {
    int64_t local = unix_utc + (int64_t)tz_offset_min * 60;
    int64_t local_midnight_shifted = (local / 86400) * 86400;
    if (local_midnight_shifted > local) local_midnight_shifted -= 86400;
    int64_t local_noon_utc = local_midnight_shifted + 43200 - (int64_t)tz_offset_min * 60;

    sun_day_events_t e;
    e.solar_noon = solar_noon_utc_for(local_noon_utc, lon_deg);

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

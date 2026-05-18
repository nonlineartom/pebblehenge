/*
 * Host-side unit tests for src/c/sun.c. Build with:
 *   gcc -std=c99 -Wall -Wextra -O2 -o sun_test test/sun_test.c src/c/sun.c -lm
 * Reference values are cross-checked against the NOAA ESRL Solar Calculator
 * and SunCalc.org for the same lat/lon/UTC inputs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "../src/c/sun.h"

static int failures = 0;

static void check_near(const char *what, double actual, double expected, double tol) {
    double diff = fabs(actual - expected);
    if (diff > tol) {
        printf("FAIL %-44s expected %9.4f +/- %.4f, got %9.4f (diff %.4f)\n",
               what, expected, tol, actual, diff);
        failures++;
    } else {
        printf("ok   %-44s %9.4f (expected %9.4f +/- %.4f)\n",
               what, actual, expected, tol);
    }
}

static void check_time(const char *what, int64_t actual, int64_t expected, int tol_sec) {
    int64_t diff = actual - expected;
    if (diff < 0) diff = -diff;
    if (diff > tol_sec) {
        printf("FAIL %-44s expected %lld +/- %d, got %lld (diff %lld)\n",
               what, (long long)expected, tol_sec, (long long)actual, (long long)diff);
        failures++;
    } else {
        printf("ok   %-44s %lld (expected %lld +/- %d)\n",
               what, (long long)actual, (long long)expected, tol_sec);
    }
}

static void check_sentinel(const char *what, int64_t actual, int64_t expected) {
    if (actual == expected) {
        printf("ok   %-44s sentinel matches\n", what);
    } else {
        printf("FAIL %-44s expected sentinel %lld, got %lld\n",
               what, (long long)expected, (long long)actual);
        failures++;
    }
}

int main(void) {
    /* --- Case 1: NYC at its own solar noon (self-consistency anchor).
     * At solar noon, true azimuth must be 180 deg (NH, sun south of zenith)
     * and altitude must equal 90 - |lat - decl|. */
    {
        int64_t t = 1718971200; /* 2024-06-21 12:00 UTC */
        sun_day_events_t e = sun_day_events(t, 40.7128f, -74.0060f, -240);
        sun_position_t p = sun_position(e.solar_noon, 40.7128f, -74.0060f);
        printf("\n[NYC solar noon 2024-06-21] noon_unix=%lld az=%.3f alt=%.3f\n",
               (long long)e.solar_noon, p.azimuth, p.altitude);
        /* decl(2024-06-21 ~17:00 UTC) ~= +23.43 deg; expected alt = 72.72. */
        check_near("NYC noon altitude", p.altitude, 72.72, 0.05);
        check_near("NYC noon azimuth", p.azimuth, 180.0, 0.2);
    }

    /* --- Case 2: NYC summer solstice, 12:00 EDT = 16:00 UTC (off-noon). */
    {
        int64_t t = 1718985600;
        sun_position_t p = sun_position(t, 40.7128f, -74.0060f);
        printf("\n[NYC 2024-06-21 16:00 UTC] az=%.3f alt=%.3f\n",
               p.azimuth, p.altitude);
        /* ~58 min before solar noon (16:58 UTC), so sun is southeast. */
        check_near("NYC summer altitude", p.altitude, 68.9, 0.3);
        check_near("NYC summer azimuth",  p.azimuth, 140.0, 2.5);
    }

    /* --- Case 3: London at its own solar noon (vernal equinox). */
    {
        int64_t t = 1710936000; /* 2024-03-20 12:00 UTC */
        sun_day_events_t e = sun_day_events(t, 51.5074f, -0.1278f, 0);
        sun_position_t p = sun_position(e.solar_noon, 51.5074f, -0.1278f);
        printf("\n[London solar noon 2024-03-20] noon_unix=%lld az=%.3f alt=%.3f\n",
               (long long)e.solar_noon, p.azimuth, p.altitude);
        /* decl ~ +0.2 deg, lat 51.5 -> alt ~ 38.7. */
        check_near("London noon altitude", p.altitude, 38.7, 0.3);
        check_near("London noon azimuth",  p.azimuth, 180.0, 0.2);
    }

    /* --- Case 4: Sydney at its own solar noon (summer solstice SH). */
    {
        int64_t t = 1734739200; /* 2024-12-21 00:00 UTC */
        sun_day_events_t e = sun_day_events(t, -33.8688f, 151.2093f, 660);
        sun_position_t p = sun_position(e.solar_noon, -33.8688f, 151.2093f);
        printf("\n[Sydney solar noon 2024-12-21] noon_unix=%lld az=%.3f alt=%.3f\n",
               (long long)e.solar_noon, p.azimuth, p.altitude);
        /* lat -33.87, decl -23.44 -> alt = 90 - |lat - decl| = 79.57; sun in N. */
        check_near("Sydney noon altitude", p.altitude, 79.57, 0.1);
        /* Azimuth at noon is due N (0/360) since |lat| > |decl|. */
        double az_from_north = p.azimuth > 180.0 ? 360.0 - p.azimuth : p.azimuth;
        check_near("Sydney noon azimuth deviation from N", az_from_north, 0.0, 0.2);
    }

    /* --- Case 5: Equator at its own solar noon (vernal equinox). */
    {
        int64_t t = 1710936000;
        sun_day_events_t e = sun_day_events(t, 0.0f, 0.0f, 0);
        sun_position_t p = sun_position(e.solar_noon, 0.0f, 0.0f);
        printf("\n[Equator solar noon 2024-03-20] noon_unix=%lld az=%.3f alt=%.3f\n",
               (long long)e.solar_noon, p.azimuth, p.altitude);
        /* At equinox at equator, sun is at zenith. */
        check_near("Equator noon altitude", p.altitude, 90.0, 0.3);
    }

    /* --- Case 5: NYC summer solstice events --------------------------- */
    {
        int64_t t = 1718971200; /* 2024-06-21 12:00 UTC */
        sun_day_events_t e = sun_day_events(t, 40.7128f, -74.0060f, -240);
        printf("\n[NYC 2024-06-21 events, EDT=UTC-4]\n");
        printf("  sunrise  unix=%lld\n", (long long)e.sunrise);
        printf("  noon     unix=%lld\n", (long long)e.solar_noon);
        printf("  sunset   unix=%lld\n", (long long)e.sunset);
        /* Reference (NOAA): sunrise 05:25 EDT = 09:25 UTC, noon 12:58 EDT = 16:58 UTC. */
        check_time("NYC sunrise",    e.sunrise,    1718961900, 120);
        check_time("NYC solar noon", e.solar_noon, 1718989080, 60);
        /* Sunset ~20:31 EDT = 00:31 UTC next day = 1719016260 */
        check_time("NYC sunset",     e.sunset,     1719016260, 120);
    }

    /* --- Case 6: Tromsø polar night ----------------------------------- */
    {
        int64_t t = 1734739200; /* 2024-12-21 00:00 UTC */
        sun_day_events_t e = sun_day_events(t, 69.6492f, 18.9553f, 60);
        printf("\n[Tromso 2024-12-21 polar night, CET=UTC+1]\n");
        printf("  sunrise sentinel=%lld sunset sentinel=%lld\n",
               (long long)e.sunrise, (long long)e.sunset);
        check_sentinel("Tromso sunrise NEVER_RISES", e.sunrise, SUN_NEVER_RISES);
        check_sentinel("Tromso sunset NEVER_RISES",  e.sunset,  SUN_NEVER_RISES);
    }

    /* --- Case 7: Tromsø polar day at summer solstice ------------------ */
    {
        int64_t t = 1718971200; /* 2024-06-21 12:00 UTC */
        sun_day_events_t e = sun_day_events(t, 69.6492f, 18.9553f, 120);
        printf("\n[Tromso 2024-06-21 midnight sun, CEST=UTC+2]\n");
        printf("  sunrise=%lld sunset=%lld\n",
               (long long)e.sunrise, (long long)e.sunset);
        check_sentinel("Tromso sunrise NEVER_SETS", e.sunrise, SUN_NEVER_SETS);
        check_sentinel("Tromso sunset NEVER_SETS",  e.sunset,  SUN_NEVER_SETS);
    }

    /* --- Case 8: Golden / blue hour ordering check -------------------- */
    {
        int64_t t = 1718971200;
        sun_day_events_t e = sun_day_events(t, 40.7128f, -74.0060f, -240);
        printf("\n[NYC golden/blue ordering]\n");
        printf("  astro_dawn=%lld nautical_dawn=%lld civil_dawn=%lld\n",
               (long long)e.astronomical_dawn, (long long)e.nautical_dawn,
               (long long)e.civil_dawn);
        printf("  blue_morn_end=%lld sunrise=%lld golden_morn_end=%lld\n",
               (long long)e.blue_hour_morning_end, (long long)e.sunrise,
               (long long)e.golden_hour_morning_end);
        if (e.astronomical_dawn < e.nautical_dawn &&
            e.nautical_dawn     < e.civil_dawn &&
            e.civil_dawn        < e.blue_hour_morning_end &&
            e.blue_hour_morning_end <= e.sunrise &&
            e.sunrise           < e.golden_hour_morning_end &&
            e.golden_hour_morning_end < e.solar_noon &&
            e.solar_noon        < e.golden_hour_evening_start &&
            e.golden_hour_evening_start < e.sunset &&
            e.sunset            <= e.blue_hour_evening_start &&
            e.blue_hour_evening_start < e.civil_dusk &&
            e.civil_dusk        < e.nautical_dusk &&
            e.nautical_dusk     < e.astronomical_dusk) {
            printf("ok   NYC twilight ordering monotonic\n");
        } else {
            printf("FAIL NYC twilight ordering broken\n");
            failures++;
        }
    }

    printf("\n%s: %d failure(s)\n", failures ? "FAILURE" : "SUCCESS", failures);
    return failures ? 1 : 0;
}

/* IMU module: subscribes to AccelerometerService, smooths the gravity
 * vector with an exponential moving average, and derives pitch / roll
 * from it. The Pebble app loader doesn't relocate newlib's atan2f
 * lookup table, so we use the self-contained `pbh_atan2f`-style
 * polynomial we already ship in sun.c... except sun.c keeps those
 * internal, so we duplicate the small atan2 routine here rather than
 * cross-link the static helpers. */

#include "imu.h"

#include <math.h>

#define PI_F          3.14159265358979323846f
#define HALF_PI_F     1.57079632679489661923f
#define R2D_F         (180.0f / PI_F)

/* AccelerometerService delivers samples in milli-g. 1000 = +1 g. */
#define SAMPLE_RATE   ACCEL_SAMPLING_10HZ
#define BATCH_SIZE    1

/* Exponential moving average weight on the new sample (0..1). Lower =
 * heavier smoothing. 0.15 -> ~3 sample (~0.3 s) settling time, which
 * tracks deliberate tilting but rejects single-frame wrist twitches. */
#define EMA_ALPHA     0.15f

/* Drop this many samples before reporting "ready" so the user doesn't
 * see the EMA settling from (0,0,0). */
#define WARMUP        3

static pbh_imu_handler s_handler;
static float           s_gx, s_gy, s_gz;
static bool            s_have_seed;
static int             s_warmup;

/* ---- self-contained atan2f (sun.c keeps its identical helpers static) - */

static float poly_atan_small(float x) {
    float x2 = x * x;
    return x * (1.0f
              + x2 * (-0.3333314f
              + x2 * ( 0.1999355f
              + x2 * (-0.1420546f
              + x2 *   0.0976160f))));
}

static float imu_atanf(float x) {
    int neg = 0;
    if (x < 0.0f) { neg = 1; x = -x; }
    int inv = 0;
    if (x > 1.0f) { inv = 1; x = 1.0f / x; }
    int shift = 0;
    if (x > 0.2679491924f) {
        const float SQRT3 = 1.7320508076f;
        shift = 1;
        x = (x * SQRT3 - 1.0f) / (SQRT3 + x);
    }
    float r = poly_atan_small(x);
    if (shift) r += PI_F / 6.0f;
    if (inv)   r = HALF_PI_F - r;
    if (neg)   r = -r;
    return r;
}

static float imu_atan2f(float y, float x) {
    if (x > 0.0f) return imu_atanf(y / x);
    if (x < 0.0f) return imu_atanf(y / x) + (y >= 0.0f ? PI_F : -PI_F);
    if (y > 0.0f) return  HALF_PI_F;
    if (y < 0.0f) return -HALF_PI_F;
    return 0.0f;
}

static float imu_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    float r = x * 0.5f + 0.5f;  /* simple initial guess */
    for (int i = 0; i < 6; i++) r = 0.5f * (r + x / r);
    return r;
}

/* ----------------------------------------------------------------------- */

static void on_accel(AccelData *data, uint32_t num_samples) {
    if (!s_handler || num_samples == 0) return;

    /* Use the latest sample of the batch. */
    AccelData s = data[num_samples - 1];

    /* Pebble axes (face-up on a desk reads roughly (0, 0, -1000) on
     * Pebble docs' convention, but the silk firmware in this QEMU build
     * reports (0, 0, +1000). Either sign works since the atan2 below
     * uses both components.) Convert to floats in g. */
    float ax = (float)s.x / 1000.0f;
    float ay = (float)s.y / 1000.0f;
    float az = (float)s.z / 1000.0f;

    if (!s_have_seed) {
        s_gx = ax; s_gy = ay; s_gz = az;
        s_have_seed = true;
    } else {
        s_gx += EMA_ALPHA * (ax - s_gx);
        s_gy += EMA_ALPHA * (ay - s_gy);
        s_gz += EMA_ALPHA * (az - s_gz);
    }

    if (s_warmup > 0) { s_warmup--; return; }

    /* Pitch: rotation about the watch's X axis (left-right band axis).
     *   pitch = atan2(-Y, sqrt(X*X + Z*Z))
     * gives 0 when the watch is held face-up flat (gravity along +/-Z)
     * and +90 when the screen is vertical with Y pointing down (a typical
     * "looking at watch" tilt of ~45-60 deg). */
    float pitch = imu_atan2f(-s_gy, imu_sqrtf(s_gx * s_gx + s_gz * s_gz));
    /* Roll about the watch's Y axis (up-down band axis):
     *   roll  = atan2(X, sqrt(Y*Y + Z*Z)) */
    float roll  = imu_atan2f(s_gx, imu_sqrtf(s_gy * s_gy + s_gz * s_gz));

    pbh_attitude_t att = {
        .pitch_deg = pitch * R2D_F,
        .roll_deg  = roll  * R2D_F,
        .ready     = true,
    };
    s_handler(att);
}

void pbh_imu_init(pbh_imu_handler handler) {
    s_handler   = handler;
    s_have_seed = false;
    s_warmup    = WARMUP;
    accel_service_set_sampling_rate(SAMPLE_RATE);
    accel_data_service_subscribe(BATCH_SIZE, on_accel);
}

void pbh_imu_deinit(void) {
    accel_data_service_unsubscribe();
    s_handler = NULL;
}

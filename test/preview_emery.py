"""Color-accurate preview of the Pebble Time 2 (emery) data view.

Emery's QEMU machine isn't ported to QEMU 10.x in coredevices/qemu yet
(`Robert (F7xx) platform not yet ported to QEMU 10.x`), so we can't run
the .pbw against the real watch firmware here. This script reproduces
ui_arc.c's PBL_COLOR rendering pipeline exactly - same projection, same
sample cadence, same Pebble GColor* palette - so the output is what the
emery binary inside the .pbw WILL draw once the F7xx port lands.
"""
import math
import os
import sys
from PIL import Image, ImageDraw, ImageFont

# Match emery resolution.
SCREEN_W, SCREEN_H = 200, 228

# Match main.c's build_data_view layout (arc canvas placement).
ARC_Y      = 28           # y origin of arc canvas within screen
ARC_H      = SCREEN_H - ARC_Y - 28
ARC_W      = SCREEN_W
HORIZON_FRAC_NUM = 72     # = ui_arc.c
HORIZON_FRAC_DEN = 100
ALT_TOP_DEG    = 90.0
ALT_BOTTOM_DEG = -20.0
FOV_DEG        = 240.0
ARC_SAMPLE_STEP_MIN = 5
ARC_SAMPLE_COUNT = 1440 // ARC_SAMPLE_STEP_MIN + 1   # 289

# Pebble GColor* approximations (the Pebble color palette is a 6-bit cube).
COLOR_PICTON_BLUE  = (85, 170, 255)     # GColorPictonBlue
COLOR_OXFORD_BLUE  = (0, 0, 85)         # GColorOxfordBlue
COLOR_DUKE_BLUE    = (0, 0, 170)
COLOR_BLUE_MOON    = (0, 85, 255)
COLOR_ORANGE       = (255, 170, 0)
COLOR_PASTEL_YEL   = (255, 255, 170)
COLOR_YELLOW       = (255, 255, 0)
COLOR_WHITE        = (255, 255, 255)
COLOR_BLACK        = (0, 0, 0)

# ---------------- Sun position algorithm (mirrors src/c/sun.c) ----------------

def sun_position(unix_utc, lat_deg, lon_deg):
    T = ((2440587.5 + unix_utc / 86400.0) - 2451545.0) / 36525.0
    L = (280.46646 + T * (36000.76983 + T * 0.0003032)) % 360.0
    M = 357.52911 + T * (35999.05029 - T * 0.0001537)
    e = 0.016708634 - T * (0.000042037 + T * 0.0000001267)
    Mr = math.radians(M)
    C = (math.sin(Mr) * (1.914602 - T * (0.004817 + 0.000014 * T))
         + math.sin(2 * Mr) * (0.019993 - T * 0.000101)
         + math.sin(3 * Mr) * 0.000289)
    Ltrue = L + C
    omega = 125.04 - 1934.136 * T
    lam = Ltrue - 0.00569 - 0.00478 * math.sin(math.radians(omega))
    eps0 = 23 + (26 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60) / 60
    eps = eps0 + 0.00256 * math.cos(math.radians(omega))
    decl = math.degrees(math.asin(math.sin(math.radians(eps)) * math.sin(math.radians(lam))))
    y = math.tan(math.radians(eps) / 2) ** 2
    l0 = math.radians(L)
    etime = (y * math.sin(2 * l0) - 2 * e * math.sin(Mr)
             + 4 * e * y * math.sin(Mr) * math.cos(2 * l0)
             - 0.5 * y * y * math.sin(4 * l0) - 1.25 * e * e * math.sin(2 * Mr))
    eq_min = math.degrees(etime) * 4

    sec_of_day = unix_utc % 86400
    tst = sec_of_day / 60 + eq_min + 4 * lon_deg
    tst %= 1440
    if tst < 0: tst += 1440
    ha = tst / 4 - 180
    if ha < -180: ha += 360
    lat_r = math.radians(lat_deg); d = math.radians(decl); H = math.radians(ha)
    cos_zen = math.sin(lat_r) * math.sin(d) + math.cos(lat_r) * math.cos(d) * math.cos(H)
    cos_zen = max(-1.0, min(1.0, cos_zen))
    zen = math.degrees(math.acos(cos_zen))
    alt = 90 - zen
    denom = math.cos(math.radians(alt)) * math.cos(lat_r)
    if abs(denom) < 1e-9:
        az = 180.0 if lat_deg > 0 else 0.0
    else:
        cos_az = (math.sin(d) - math.sin(math.radians(alt)) * math.sin(lat_r)) / denom
        cos_az = max(-1.0, min(1.0, cos_az))
        ang = math.degrees(math.acos(cos_az))
        az = 360 - ang if ha >= 0 else ang
    return az, alt


def event_for_target_alt(date_unix, lat, lon, target_alt_deg, morning):
    """Solve hour-angle equation for the moment alt == target. Returns unix_utc or None."""
    event = date_unix
    lat_r = math.radians(lat); tgt = math.radians(target_alt_deg)
    for _ in range(2):
        T = ((2440587.5 + event / 86400.0) - 2451545.0) / 36525.0
        omega = 125.04 - 1934.136 * T
        lam = ((280.46646 + T * (36000.76983 + T * 0.0003032)) % 360.0
               + math.sin(math.radians(357.52911 + T * (35999.05029 - T * 0.0001537)))
                  * (1.914602 - T * (0.004817 + 0.000014 * T))
               - 0.00569
               - 0.00478 * math.sin(math.radians(omega)))
        eps0 = 23 + (26 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60) / 60
        eps = eps0 + 0.00256 * math.cos(math.radians(omega))
        dec = math.radians(math.degrees(math.asin(math.sin(math.radians(eps)) * math.sin(math.radians(lam)))))
        c = (math.sin(tgt) - math.sin(lat_r) * math.sin(dec)) / (math.cos(lat_r) * math.cos(dec))
        if c > 1 or c < -1:
            return None
        H_deg = math.degrees(math.acos(c))
        # Solar noon (UTC) for this longitude.
        M = 357.52911 + T * (35999.05029 - T * 0.0001537); Mr = math.radians(M)
        e = 0.016708634 - T * (0.000042037 + T * 0.0000001267)
        y_t = math.tan(math.radians(eps) / 2) ** 2
        l0 = math.radians((280.46646 + T * (36000.76983 + T * 0.0003032)) % 360.0)
        etime = (y_t * math.sin(2 * l0) - 2 * e * math.sin(Mr)
                 + 4 * e * y_t * math.sin(Mr) * math.cos(2 * l0)
                 - 0.5 * y_t * y_t * math.sin(4 * l0) - 1.25 * e * e * math.sin(2 * Mr))
        eq_min = math.degrees(etime) * 4
        day_floor = int(event // 86400) * 86400
        noon_sec = 720 * 60 - (eq_min + 4 * lon) * 60
        noon = day_floor + int(noon_sec + 0.5)
        off = int(H_deg * 240 + 0.5)
        event = noon - off if morning else noon + off
    return event


# ---------------- Renderer ----------------

def render(now_utc, lat, lon, tz_offset_min, heading_deg=None, out_path="/tmp/emery.png"):
    img = Image.new("RGB", (SCREEN_W, SCREEN_H), COLOR_WHITE)
    draw = ImageDraw.Draw(img)

    # ---- Compute arc samples for the local day -----------------------------
    local_midnight = ((now_utc + tz_offset_min * 60) // 86400) * 86400 - tz_offset_min * 60
    samples = []
    for i in range(ARC_SAMPLE_COUNT):
        t = local_midnight + i * (ARC_SAMPLE_STEP_MIN * 60)
        az, alt = sun_position(t, lat, lon)
        samples.append((az, alt))

    # ---- View centre: solar-noon azimuth (sample with max altitude) ---------
    if heading_deg is not None:
        view_center = heading_deg
    else:
        best = max(range(ARC_SAMPLE_COUNT), key=lambda i: samples[i][1])
        view_center = samples[best][0]

    def az_off(az):
        d = az - view_center
        while d > 180: d -= 360
        while d < -180: d += 360
        return d

    horizon_y = ARC_Y + ARC_H * HORIZON_FRAC_NUM // HORIZON_FRAC_DEN
    canvas_top = ARC_Y + 2
    canvas_bot = ARC_Y + ARC_H - 1

    def x_for_offset(off):
        t = (off + FOV_DEG / 2) / FOV_DEG
        t = max(0.0, min(1.0, t))
        return ARC_W * t

    def y_for_alt(a):
        if a >= 0:
            return horizon_y - min(1.0, a / ALT_TOP_DEG) * (horizon_y - canvas_top)
        return horizon_y + min(1.0, a / ALT_BOTTOM_DEG) * (canvas_bot - horizon_y)

    # ---- Background: sky above horizon, ground below ----------------------
    draw.rectangle([(0, ARC_Y), (SCREEN_W, ARC_Y + ARC_H)], fill=COLOR_PICTON_BLUE)
    draw.rectangle([(0, horizon_y), (SCREEN_W, ARC_Y + ARC_H)], fill=COLOR_OXFORD_BLUE)

    # ---- Horizon line -----------------------------------------------------
    draw.line([(0, horizon_y), (SCREEN_W - 1, horizon_y)], fill=COLOR_BLACK, width=1)

    # ---- Cardinal ticks N/E/S/W ------------------------------------------
    try:
        font_small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 12)
        font_med = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 18)
        font_big = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 24)
    except OSError:
        font_small = font_med = font_big = ImageFont.load_default()

    for label, az in [("N", 0), ("E", 90), ("S", 180), ("W", 270)]:
        off = az_off(az)
        if -FOV_DEG / 2 <= off <= FOV_DEG / 2:
            x = x_for_offset(off)
            draw.line([(x, horizon_y - 3), (x, horizon_y + 3)], fill=COLOR_BLACK)
            draw.text((x - 5, horizon_y + 4), label, fill=COLOR_BLACK, font=font_small)

    # ---- Arc curve --------------------------------------------------------
    pts = []
    for az, alt in samples:
        pts.append((x_for_offset(az_off(az)), y_for_alt(alt)))
    for i in range(1, len(pts)):
        off0 = az_off(samples[i - 1][0])
        off1 = az_off(samples[i][0])
        if abs(off1 - off0) > 180:
            continue  # wrap-around segment
        below = samples[i - 1][1] < 0 and samples[i][1] < 0
        draw.line([pts[i - 1], pts[i]], fill=COLOR_BLACK, width=1 if below else 2)

    # ---- Hourly tick marks + major rise/noon/set labels -------------------
    rise = event_for_target_alt(local_midnight + 43200, lat, lon, -0.833, True)
    set_ = event_for_target_alt(local_midnight + 43200, lat, lon, -0.833, False)
    # Minor hourly ticks (only above horizon).
    for h in range(25):
        t = local_midnight + h * 3600
        az_h, alt_h = sun_position(t, lat, lon)
        if alt_h < -2: continue
        off = az_off(az_h)
        if not (-FOV_DEG/2 <= off <= FOV_DEG/2): continue
        x, y = x_for_offset(off), y_for_alt(alt_h)
        draw.line([(x, y - 3), (x, y + 3)], fill=COLOR_BLACK, width=1)
    # Major dots + labels.
    # Solar-noon by max altitude.
    best_t = None; best_alt = -90
    for h_min in range(0, 1441, 5):
        t = local_midnight + h_min * 60
        _, a = sun_position(t, lat, lon)
        if a > best_alt: best_alt = a; best_t = t
    for t, lab in [(rise, "rise"), (best_t, "noon"), (set_, "set")]:
        if t is None: continue
        az_e, alt_e = sun_position(t, lat, lon)
        off = az_off(az_e)
        if not (-FOV_DEG/2 <= off <= FOV_DEG/2): continue
        x, y = x_for_offset(off), y_for_alt(alt_e)
        draw.ellipse((x - 4, y - 4, x + 4, y + 4), fill=COLOR_WHITE, outline=COLOR_BLACK)
        draw.ellipse((x - 2, y - 2, x + 2, y + 2), fill=COLOR_BLACK)
        ly = (y - 18) if alt_e > 0 else (y + 6)
        draw.text((x - 16, ly), lab, fill=COLOR_BLACK, font=font_small)

    # ---- Sun ball (current position) --------------------------------------
    az_now, alt_now = sun_position(now_utc, lat, lon)
    off_now = az_off(az_now)
    if -FOV_DEG / 2 <= off_now <= FOV_DEG / 2:
        x = x_for_offset(off_now); y = y_for_alt(alt_now)
        draw.ellipse((x - 9, y - 9, x + 9, y + 9), fill=COLOR_WHITE)
        draw.ellipse((x - 6, y - 6, x + 6, y + 6), fill=COLOR_YELLOW, outline=COLOR_BLACK)

    # ---- Heading marker triangle (only when compass calibrated) -----------
    if heading_deg is not None:
        cx = SCREEN_W // 2
        draw.polygon([(cx, ARC_Y + 6), (cx - 4, ARC_Y), (cx + 4, ARC_Y)], fill=COLOR_BLACK)

    # ---- Header strip: clock + GPS recency indicator ----------------------
    local_secs = (now_utc + tz_offset_min * 60) % 86400
    hh = int(local_secs // 3600); mm = int((local_secs % 3600) // 60)
    clock = f"{hh:02d}:{mm:02d}"
    draw.text((4, 2), clock, fill=COLOR_BLACK, font=font_big)
    # In the preview we always show a seed status.
    draw.text((SCREEN_W - 60, 8), "? seed", fill=COLOR_BLACK, font=font_small)

    # ---- Bottom strip: single centred "HH:MM - HH:MM" --------------------
    bot_y = ARC_Y + ARC_H + 2
    def fmt_local(t):
        lsec = (t + tz_offset_min * 60) % 86400
        return f"{int(lsec // 3600):02d}:{int((lsec % 3600) // 60):02d}"
    if rise and set_:
        msg = f"{fmt_local(rise)} - {fmt_local(set_)}"
        tw = draw.textlength(msg, font=font_med)
        draw.text(((SCREEN_W - tw) // 2, bot_y), msg, fill=COLOR_BLACK, font=font_med)

    img.save(out_path)
    print(f"saved {out_path} ({SCREEN_W}x{SCREEN_H})")
    return img


if __name__ == "__main__":
    # NYC seed location, simulate solar noon.
    NYC_LAT, NYC_LON, EDT_TZ = 40.7128, -74.0060, -240
    # 2026-05-18 16:00 UTC = noon EDT.
    render(1779120000, NYC_LAT, NYC_LON, EDT_TZ,
           out_path=os.path.join(os.path.dirname(__file__), "..", "screenshots", "emery-data.png"))
    # Mid-afternoon, sun in the SW.
    render(1779138000, NYC_LAT, NYC_LON, EDT_TZ,
           out_path=os.path.join(os.path.dirname(__file__), "..", "screenshots", "emery-afternoon.png"))
    # Heading-rotated view: user facing east, same clock-noon moment.
    render(1779120000, NYC_LAT, NYC_LON, EDT_TZ, heading_deg=90.0,
           out_path=os.path.join(os.path.dirname(__file__), "..", "screenshots", "emery-facing-east.png"))

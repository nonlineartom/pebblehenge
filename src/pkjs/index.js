// PebbleKit JS for Pebblehenge: supplies the watch with phone GPS,
// timezone offset and (optionally) magnetic declination.

var GEO_OPTIONS = {
    enableHighAccuracy: true,
    timeout: 20000,
    maximumAge: 5 * 60 * 1000
};

var DECL_CACHE_MS = 30 * 24 * 60 * 60 * 1000; // 30 days

function log(msg) { console.log('[pebblehenge] ' + msg); }

function sendFix(coords, declDeg) {
    var now = Math.floor(Date.now() / 1000);
    var payload = {
        LatitudeE7:         Math.round(coords.latitude  * 1e7),
        LongitudeE7:        Math.round(coords.longitude * 1e7),
        LocationAccuracyM:  Math.round(coords.accuracy || 0),
        LocationTimestamp:  now,
        TzOffsetMin:        -new Date().getTimezoneOffset()
    };
    if (typeof declDeg === 'number' && isFinite(declDeg)) {
        payload.MagDeclinationE4 = Math.round(declDeg * 1e4);
    }
    Pebble.sendAppMessage(payload,
        function() { log('fix sent: ' + JSON.stringify(payload)); },
        function(e) { log('fix send FAILED: ' + JSON.stringify(e)); });
}

// Magnetic declination via NOAA's NCEI calculator. Cached for 30 days
// (declination changes < 1 deg/year). Failure is non-fatal; the watch
// still works without it (azimuth display is unaffected; only the
// magnetic->true compass conversion needs it, which is Phase 4).
function fetchDeclination(lat, lon, cb) {
    try {
        var cached = JSON.parse(localStorage.getItem('decl') || 'null');
        if (cached && Date.now() - cached.t < DECL_CACHE_MS
                && Math.abs(cached.lat - lat) < 1.0
                && Math.abs(cached.lon - lon) < 1.0) {
            log('declination from cache: ' + cached.d);
            return cb(cached.d);
        }
    } catch (e) { /* ignore */ }

    var url = 'https://www.ngdc.noaa.gov/geomag-web/calculators/calculateDeclination'
            + '?lat1=' + lat + '&lon1=' + lon
            + '&resultFormat=json';
    var req = new XMLHttpRequest();
    req.open('GET', url, true);
    req.timeout = 8000;
    req.onload = function() {
        try {
            var r = JSON.parse(req.responseText);
            var d = r.result && r.result[0] && r.result[0].declination;
            if (typeof d === 'number') {
                try { localStorage.setItem('decl', JSON.stringify({ lat: lat, lon: lon, d: d, t: Date.now() })); } catch (e) {}
                log('declination fetched: ' + d);
                cb(d);
            } else {
                log('declination response had no value');
                cb(null);
            }
        } catch (e) {
            log('declination parse failed: ' + e);
            cb(null);
        }
    };
    req.onerror = req.ontimeout = function() {
        log('declination request failed');
        cb(null);
    };
    req.send();
}

function pushLocation() {
    navigator.geolocation.getCurrentPosition(
        function(pos) {
            log('got fix: ' + pos.coords.latitude + ',' + pos.coords.longitude);
            fetchDeclination(pos.coords.latitude, pos.coords.longitude,
                function(decl) { sendFix(pos.coords, decl); });
        },
        function(err) {
            log('geolocation failed: ' + JSON.stringify(err));
        },
        GEO_OPTIONS);
}

Pebble.addEventListener('ready', function() {
    log('ready');
    pushLocation();
});

Pebble.addEventListener('appmessage', function(e) {
    if (e.payload && e.payload.RequestLocation) {
        log('watch requested fresh location');
        pushLocation();
    }
});

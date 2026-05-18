// PebbleKit JS entry point.
// Phase 3 will populate this with phone geolocation + magnetic declination
// lookup. For now it acknowledges the watch's "ready" event so the SDK
// is happy.

Pebble.addEventListener('ready', function() {
    console.log('Pebblehenge PebbleKit JS ready');
});

Pebble.addEventListener('appmessage', function(e) {
    console.log('AppMessage received: ' + JSON.stringify(e.payload));
});

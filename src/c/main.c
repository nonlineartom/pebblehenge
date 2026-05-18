#include <pebble.h>

#include "sun.h"

static Window *s_window;
static TextLayer *s_status_layer;

static void window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(root);

    s_status_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 14,
                                              bounds.size.w, 28));
    text_layer_set_text(s_status_layer, "Pebblehenge");
    text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
    text_layer_set_font(s_status_layer,
                        fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    layer_add_child(root, text_layer_get_layer(s_status_layer));
}

static void window_unload(Window *window) {
    text_layer_destroy(s_status_layer);
}

static void init(void) {
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers){
        .load = window_load,
        .unload = window_unload,
    });
    window_stack_push(s_window, true);
}

static void deinit(void) {
    window_destroy(s_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}

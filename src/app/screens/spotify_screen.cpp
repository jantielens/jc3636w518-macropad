#include "spotify_screen.h"

#if HAS_DISPLAY

#include "../spotify_manager.h"
#include "../log_manager.h"

#include <esp_heap_caps.h>

SpotifyScreen::SpotifyScreen() {}

SpotifyScreen::~SpotifyScreen() {
    destroy();
}

void SpotifyScreen::create() {
    if (scr) return;

    scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Top labels
    title = lv_label_create(scr);
    lv_label_set_text(title, "Spotify");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "Not connected");
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 34);

    // Album art image
    img = lv_img_create(scr);
    // Use as a full-screen background (we pre-scale album art to 360x360).
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    lv_img_set_src(img, nullptr);
    lv_obj_move_background(img);

    // Large prev/next "buttons" at bottom (use generic objects; LV_USE_BTN may be disabled)
    btn_prev = lv_obj_create(scr);
    lv_obj_set_size(btn_prev, 140, 56);
    lv_obj_align(btn_prev, LV_ALIGN_BOTTOM_LEFT, 18, -18);
    lv_obj_set_style_radius(btn_prev, 12, 0);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x1f2937), 0);
    lv_obj_add_event_cb(btn_prev, prev_cb, LV_EVENT_CLICKED, this);

    lv_obj_t* lprev = lv_label_create(btn_prev);
    lv_label_set_text(lprev, "Prev");
    lv_obj_center(lprev);
    lv_obj_clear_flag(lprev, LV_OBJ_FLAG_CLICKABLE);

    btn_next = lv_obj_create(scr);
    lv_obj_set_size(btn_next, 140, 56);
    lv_obj_align(btn_next, LV_ALIGN_BOTTOM_RIGHT, -18, -18);
    lv_obj_set_style_radius(btn_next, 12, 0);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x1f2937), 0);
    lv_obj_add_event_cb(btn_next, next_cb, LV_EVENT_CLICKED, this);

    lv_obj_t* lnext = lv_label_create(btn_next);
    lv_label_set_text(lnext, "Next");
    lv_obj_center(lnext);
    lv_obj_clear_flag(lnext, LV_OBJ_FLAG_CLICKABLE);

    // Ensure labels don't intercept clicks.
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(subtitle, LV_OBJ_FLAG_CLICKABLE);
}

void SpotifyScreen::destroy() {
    freePixelBuf();
    if (scr) {
        lv_obj_del(scr);
        scr = nullptr;
        img = nullptr;
        title = nullptr;
        subtitle = nullptr;
        btn_prev = nullptr;
        btn_next = nullptr;
    }
}

void SpotifyScreen::show() {
    if (!scr) create();
    spotify_manager::set_active(true);
    lv_scr_load(scr);
}

void SpotifyScreen::hide() {
    spotify_manager::set_active(false);
}

void SpotifyScreen::freePixelBuf() {
    if (pixel_buf) {
        heap_caps_free(pixel_buf);
        pixel_buf = nullptr;
        pixel_buf_bytes = 0;
    }
    memset(&img_dsc, 0, sizeof(img_dsc));
    if (img) {
        lv_img_set_src(img, nullptr);
        #if LV_USE_IMG_TRANSFORM
        lv_img_set_zoom(img, 256);
        #endif
    }
}

void SpotifyScreen::maybeAdoptNewImage() {
    SpotifyImage newImg;
    if (!spotify_manager::take_image(&newImg)) return;

    freePixelBuf();

    pixel_buf = newImg.pixels;
    pixel_buf_bytes = (size_t)newImg.w * (size_t)newImg.h * 2;

    img_dsc.header.always_zero = 0;
    img_dsc.header.w = (uint32_t)newImg.w;
    img_dsc.header.h = (uint32_t)newImg.h;
    img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    img_dsc.data_size = pixel_buf_bytes;
    img_dsc.data = (const uint8_t*)pixel_buf;

    lv_img_set_src(img, &img_dsc);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);

    Logger.logMessagef("Spotify", "Album art applied to UI: %dx%d", newImg.w, newImg.h);

    #if LV_USE_IMG_TRANSFORM
    // Album art is pre-scaled to 360x360, so keep 1:1.
    lv_img_set_zoom(img, 256);
    #endif
}

void SpotifyScreen::update() {
    const unsigned long now = millis();
    if (now - last_ui_update_ms < 250) {
        maybeAdoptNewImage();
        return;
    }
    last_ui_update_ms = now;

    const bool connected = spotify_manager::is_connected();
    SpotifyNowPlaying np = spotify_manager::get_now_playing();

    if (!connected) {
        lv_label_set_text(subtitle, "Not connected (use portal)");
    } else if (!np.valid) {
        lv_label_set_text(subtitle, "Connected (loading…)");
    } else if (np.track_name[0] == '\0') {
        lv_label_set_text(subtitle, "Nothing playing");
    } else {
        // Keep it short; this is a POC.
        static char line[192];
        snprintf(line, sizeof(line), "%s — %s", np.track_name, np.artist_name);
        lv_label_set_text(subtitle, line);
    }

    maybeAdoptNewImage();
}

void SpotifyScreen::prev_cb(lv_event_t* e) {
    (void)e;
    spotify_manager::request_prev();
}

void SpotifyScreen::next_cb(lv_event_t* e) {
    (void)e;
    spotify_manager::request_next();
}

#endif

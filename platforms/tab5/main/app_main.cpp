/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <app.h>
#include <hal/hal.h>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <esp_lvgl_port.h>

static lv_obj_t* _scr_home  = nullptr;
static lv_obj_t* _scr_hello = nullptr;
static bool      _is_hello  = false;

static lv_indev_t* _active_indev = nullptr;
static lv_point_t  _press_start  = {0, 0};
static lv_point_t  _press_last   = {0, 0};

static const int kSwipeThresholdPx = 60;

static void _swipe_timer_cb(lv_timer_t* timer)
{
    (void)timer;

    if (_active_indev) {
        lv_indev_state_t state = lv_indev_get_state(_active_indev);
        lv_point_t pt;
        lv_indev_get_point(_active_indev, &pt);

        if (state == LV_INDEV_STATE_PRESSED) {
            _press_last = pt;
        } else {
            int32_t dx = _press_last.x - _press_start.x;
            int32_t dy = _press_last.y - _press_start.y;

            if (abs((int)dx) > kSwipeThresholdPx && abs((int)dx) > abs((int)dy)) {
                if (!_is_hello && dx < 0 && _scr_hello) {
                    lv_screen_load(_scr_hello);
                    _is_hello = true;
                } else if (_is_hello && dx > 0 && _scr_home) {
                    lv_screen_load(_scr_home);
                    _is_hello = false;
                }
            }
            _active_indev = nullptr;
        }
        return;
    }

    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER)
            continue;
        if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            lv_point_t pt;
            lv_indev_get_point(indev, &pt);
            _active_indev = indev;
            _press_start  = pt;
            _press_last   = pt;
            break;
        }
    }
}

static void _setup_dual_screen()
{
    lvgl_port_lock(0);

    _scr_home = lv_screen_active();

    lv_display_t* disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);

    _scr_hello = lv_obj_create(nullptr);
    lv_obj_set_size(_scr_hello, w, h);
    lv_obj_set_style_bg_color(_scr_hello, lv_color_black(), 0);

    lv_obj_t* lbl = lv_label_create(_scr_hello);
    lv_label_set_text(lbl, "Hello World!");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl);

    lv_timer_create(_swipe_timer_cb, 30, nullptr);

    lvgl_port_unlock();
}

extern "C" void app_main(void)
{
    app::InitCallback_t callback;

    callback.onHalInjection = []() {
        hal::Inject(std::make_unique<HalEsp32>());
    };

    app::Init(callback);

    _setup_dual_screen();

    while (!app::IsDone()) {
        app::Update();
        vTaskDelay(1);
    }
    app::Destroy();
}
/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <app.h>
#include <hal/hal.h>
#include <apps/wifi_station/wifi_station_view.h>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <esp_lvgl_port.h>

static lv_obj_t* _scr_home  = nullptr;
static lv_obj_t* _scr_hello = nullptr;
static bool      _is_hello  = false;

static std::unique_ptr<wifi_station::WifiStationView> _wifi_view;

static lv_indev_t* _active_indev = nullptr;
static lv_point_t  _press_start  = {0, 0};
static lv_point_t  _press_last   = {0, 0};

static const int kSwipeThresholdPx = 60;

static void _swipe_timer_cb(lv_timer_t* timer)
{
    (void)timer;

    // 第二屏的周期刷新（扫描列表 / 连接状态 / 数据监视）
    if (_is_hello && _wifi_view) {
        _wifi_view->update();
    }

    if (_active_indev) {
        lv_indev_state_t state = lv_indev_get_state(_active_indev);
        lv_point_t pt;
        lv_indev_get_point(_active_indev, &pt);

        if (state == LV_INDEV_STATE_PRESSED) {
            _press_last = pt;
        } else {
            int32_t dx = _press_last.x - _press_start.x;
            int32_t dy = _press_last.y - _press_start.y;

            // 密码弹窗打开（含软键盘）时忽略滑动，避免误触返回
            bool modal_open = (_is_hello && _wifi_view && _wifi_view->isModalOpen());

            if (!modal_open && abs((int)dx) > kSwipeThresholdPx && abs((int)dx) > abs((int)dy)) {
                if (!_is_hello && dx < 0 && _scr_hello) {
                    // 进入第二屏：切换 WiFi 到 Station 模式（内部异步执行，不阻塞 LVGL）
                    if (_wifi_view) {
                        _wifi_view->setActive(true);
                    }
                    GetHAL()->wifiSetMode(hal::HalBase::WIFI_MODE_STA);
                    lv_screen_load(_scr_hello);
                    _is_hello = true;
                } else if (_is_hello && dx > 0 && _scr_home) {
                    // 返回第一屏：恢复 AP 热点 + 网页
                    if (_wifi_view) {
                        _wifi_view->setActive(false);
                    }
                    GetHAL()->wifiSetMode(hal::HalBase::WIFI_MODE_AP);
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

    // 第二屏：WiFi Station 配网 + 本地服务端数据监视
    _scr_hello = lv_obj_create(nullptr);
    lv_obj_set_size(_scr_hello, w, h);
    lv_obj_set_style_bg_color(_scr_hello, lv_color_hex(0x0E1116), 0);
    lv_obj_set_style_bg_opa(_scr_hello, LV_OPA_COVER, 0);
    lv_obj_remove_flag(_scr_hello, LV_OBJ_FLAG_SCROLLABLE);

    _wifi_view = std::make_unique<wifi_station::WifiStationView>();
    _wifi_view->init(_scr_hello);

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

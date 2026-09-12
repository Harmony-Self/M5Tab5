/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <hal/hal.h>
#include <lvgl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wifi_station {

/**
 * @brief 第二屏：WiFi Station 配网 + 本地服务端数据监视
 *
 * 所有接口都必须在持有 LVGL 锁的前提下调用（由调用方保证，例如 LVGL 定时器回调）。
 * 视图自身不获取 LVGL 锁，内部只通过 HAL 的非阻塞接口访问 WiFi 服务。
 */
class WifiStationView {
public:
    void init(lv_obj_t* parent);
    void setActive(bool active);
    void update();
    bool isModalOpen();

private:
    using WifiMode_t     = hal::HalBase::WifiMode_t;
    using WifiStaState_t = hal::HalBase::WifiStaState_t;
    using WifiStaInfo_t  = hal::HalBase::WifiStaInfo_t;
    using WifiApInfo_t   = hal::HalBase::WifiApInfo_t;

    /* ------------------------------- UI 构建 -------------------------------- */
    void _build_ui(lv_obj_t* parent);
    void _build_password_modal(lv_obj_t* parent);

    /** @brief 发送输入框用的软键盘：屏幕的直接子对象，只能用 lv_obj_align 定位 */
    void _build_send_keyboard(lv_obj_t* screen);

    /** @brief 显隐发送键盘；弹出时上移内容层，保证输入框不被键盘遮挡 */
    void _show_send_keyboard(bool show);

    lv_obj_t* _create_card(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, const char* title,
                           lv_obj_t** title_out);

    /* ------------------------------- 周期刷新 -------------------------------- */
    void _update_status(const WifiStaInfo_t& info, WifiMode_t mode, bool scanning);
    void _update_connection_card(const WifiStaInfo_t& info, WifiMode_t mode);
    void _update_service_card(const WifiStaInfo_t& info);
    void _update_list(const WifiStaInfo_t& info, WifiMode_t mode, bool scanning);
    void _update_send_channel();
    bool _fetch_log();

    /* -------------------------------- 交互 ---------------------------------- */
    void _select_row(int32_t index);
    void _open_password_modal(const std::string& ssid);
    void _close_password_modal();
    void _submit_password();
    void _submit_send();

    static void _on_scan_clicked(lv_event_t* e);
    static void _on_row_clicked(lv_event_t* e);
    static void _on_send_clicked(lv_event_t* e);
    static void _on_clear_clicked(lv_event_t* e);
    static void _on_chan_tcp_clicked(lv_event_t* e);
    static void _on_chan_udp_clicked(lv_event_t* e);
    static void _on_modal_connect(lv_event_t* e);
    static void _on_modal_cancel(lv_event_t* e);
    static void _on_keyboard(lv_event_t* e);
    static void _on_send_focused(lv_event_t* e);
    static void _on_send_keyboard(lv_event_t* e);
    static void _on_page_clicked(lv_event_t* e);

    lv_obj_t* _root = nullptr;

    /* 顶部状态栏 */
    lv_obj_t* _pill      = nullptr;
    lv_obj_t* _pill_dot  = nullptr;
    lv_obj_t* _pill_text = nullptr;

    /* 网络列表 */
    lv_obj_t* _list         = nullptr;
    lv_obj_t* _scan_btn     = nullptr;
    lv_obj_t* _scan_btn_txt = nullptr;
    lv_obj_t* _list_hint    = nullptr;

    /* 连接信息卡片 */
    lv_obj_t* _v_ssid    = nullptr;
    lv_obj_t* _v_ip      = nullptr;
    lv_obj_t* _v_gw      = nullptr;
    lv_obj_t* _v_mac     = nullptr;
    lv_obj_t* _v_rssi    = nullptr;
    lv_obj_t* _conn_hint = nullptr;

    /* 服务卡片 */
    lv_obj_t* _v_web     = nullptr;
    lv_obj_t* _v_tcp     = nullptr;
    lv_obj_t* _v_udp     = nullptr;
    lv_obj_t* _v_clients = nullptr;
    lv_obj_t* _web_dot   = nullptr;
    lv_obj_t* _tcp_dot   = nullptr;
    lv_obj_t* _udp_dot   = nullptr;

    /* 数据监视 */
    lv_obj_t* _log_ta       = nullptr;
    lv_obj_t* _log_hint     = nullptr;
    lv_obj_t* _send_ta      = nullptr;
    lv_obj_t* _chan_tcp     = nullptr;
    lv_obj_t* _chan_udp     = nullptr;
    lv_obj_t* _chan_tcp_txt = nullptr;
    lv_obj_t* _chan_udp_txt = nullptr;

    /* 内容层与发送键盘 */
    lv_obj_t* _page         = nullptr;
    lv_obj_t* _send_kb      = nullptr;
    int32_t   _send_kb_h    = 0;
    int32_t   _page_shift   = 0;
    bool      _send_kb_open = false;

    /* 密码弹窗 */
    lv_obj_t* _modal             = nullptr;
    lv_obj_t* _modal_ssid        = nullptr;
    lv_obj_t* _modal_pass        = nullptr;
    lv_obj_t* _modal_err         = nullptr;
    lv_obj_t* _modal_kb          = nullptr;
    lv_obj_t* _modal_connect     = nullptr;
    lv_obj_t* _modal_connect_txt = nullptr;

    /* 状态 */
    bool _inited            = false;
    bool _active            = false;
    bool _modal_open        = false;
    bool _modal_connecting  = false;
    bool _use_udp           = false;
    bool _sta_kicked        = false;
    bool _scan_btn_busy     = false;
    bool _geom_logged       = false;
    int32_t _selected       = -1;
    int32_t _last_state     = -1;
    int32_t _last_mode      = -1;
    bool _last_scanning     = false;
    uint32_t _list_poll_tick = 0;
    std::string _last_list_key;
    std::string _modal_ssid_str;
    std::string _log_buf;
    std::vector<WifiApInfo_t> _ap_list;
};

}  // namespace wifi_station

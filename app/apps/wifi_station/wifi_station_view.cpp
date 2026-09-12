/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <apps/wifi_station/wifi_station_view.h>
#include <mooncake_log.h>
#include <cstdio>
#include <cstring>

using namespace wifi_station;

namespace {

const std::string _tag = "wifi-station-view";

/* --------------------------------- 主题色 --------------------------------- */
constexpr uint32_t COL_BG     = 0x0E1116;
constexpr uint32_t COL_CARD   = 0x1C212A;
constexpr uint32_t COL_CARD2  = 0x151920;
constexpr uint32_t COL_LINE   = 0x2A3140;
constexpr uint32_t COL_ROW    = 0x222833;
constexpr uint32_t COL_ROW_ON = 0x24324A;
constexpr uint32_t COL_TEXT   = 0xFFFFFF;
constexpr uint32_t COL_TEXT2  = 0xA9B1BD;
constexpr uint32_t COL_TEXT3  = 0x6B7280;
constexpr uint32_t COL_PRI    = 0x3B82F6;
constexpr uint32_t COL_OK     = 0x22C55E;
constexpr uint32_t COL_WARN   = 0xF59E0B;
constexpr uint32_t COL_ERR    = 0xEF4444;
constexpr uint32_t COL_INFO   = 0x38BDF8;
constexpr uint32_t COL_MUTE   = 0x4B5563;

/* WiFi 断开原因码（esp_wifi_types.h），此处只做展示用的可读化 */
std::string _reason_text(int reason)
{
    switch (reason) {
        case 201:
            return "network not found";
        case 202:
        case 15:
        case 204:
            return "wrong password";
        case 203:
            return "association failed";
        case 200:
            return "beacon timeout";
        case 205:
            return "connection failed";
        default:
            return "code " + std::to_string(reason);
    }
}

int _rssi_level(int8_t rssi)
{
    if (rssi >= -55) {
        return 5;
    }
    if (rssi >= -65) {
        return 4;
    }
    if (rssi >= -72) {
        return 3;
    }
    if (rssi >= -80) {
        return 2;
    }
    return 1;
}

lv_obj_t* _mk_label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color)
{
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    return lbl;
}

void _set_text(lv_obj_t* lbl, const std::string& text)
{
    if (lbl == nullptr) {
        return;
    }
    // 内容没变就不刷新，避免每帧重建文本与重绘
    const char* current = lv_label_get_text(lbl);
    if (current != nullptr && text == current) {
        return;
    }
    lv_label_set_text(lbl, text.c_str());
}

lv_obj_t* _mk_card(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(COL_CARD2), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 26, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_offset_y(card, 10, 0);
    return card;
}

lv_obj_t* _mk_pill_button(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bg, const char* text,
                          uint32_t text_color)
{
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t* lbl = _mk_label(btn, text, &lv_font_montserrat_18, text_color);
    lv_obj_center(lbl);
    return btn;
}

void _mk_kv_row(lv_obj_t* card, int32_t y, const char* key, lv_obj_t** value_out)
{
    lv_obj_t* k = _mk_label(card, key, &lv_font_montserrat_18, COL_TEXT3);
    lv_obj_align(k, LV_ALIGN_TOP_LEFT, 22, y);

    lv_obj_t* v = _mk_label(card, "--", &lv_font_montserrat_18, COL_TEXT);
    lv_obj_set_width(v, 200);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -22, y);

    if (value_out != nullptr) {
        *value_out = v;
    }
}

void _mk_service_row(lv_obj_t* card, int32_t y, const char* label, lv_obj_t** dot_out, lv_obj_t** value_out)
{
    lv_obj_t* dot = lv_obj_create(card);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 22, y + 7);

    lv_obj_t* k = _mk_label(card, label, &lv_font_montserrat_18, COL_TEXT3);
    lv_obj_align(k, LV_ALIGN_TOP_LEFT, 44, y);

    lv_obj_t* v = _mk_label(card, "--", &lv_font_montserrat_18, COL_TEXT);
    lv_obj_set_width(v, 220);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -22, y);

    if (dot_out != nullptr) {
        *dot_out = dot;
    }
    if (value_out != nullptr) {
        *value_out = v;
    }
}

void _set_dot_color(lv_obj_t* dot, uint32_t color)
{
    if (dot != nullptr) {
        lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    }
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                                    init                                    */
/* -------------------------------------------------------------------------- */

void WifiStationView::init(lv_obj_t* parent)
{
    if (_inited) {
        return;
    }
    _root   = parent;
    _inited = true;

    _build_ui(parent);
    _build_password_modal(parent);

    mclog::tagInfo(_tag, "view init");
}

/* -------------------------------- UI 构建 -------------------------------- */

lv_obj_t* WifiStationView::_create_card(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
                                        const char* title, lv_obj_t** title_out)
{
    lv_obj_t* card = _mk_card(parent, x, y, w, h);
    if (title != nullptr) {
        lv_obj_t* t = _mk_label(card, title, &lv_font_montserrat_16, COL_TEXT3);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 22, 20);
        if (title_out != nullptr) {
            *title_out = t;
        }
    }
    return card;
}

void WifiStationView::_build_ui(lv_obj_t* parent)
{
    // 屏幕只负责背景，所有内容都挂到内容层 _page 上，
    // 这样软键盘弹出时可以整体平移 _page，保证输入框不被键盘盖住。
    // 注意：_page 只能用 lv_obj_set_pos / lv_obj_set_y 控制位置，绝不能给它设 align
    // （LVGL 中 align 优先于 x/y，设了 align 之后平移就失效）。
    lv_obj_t* scr = parent;
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    _page = lv_obj_create(scr);
    lv_obj_set_size(_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(_page, 0, 0);
    lv_obj_remove_flag(_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(_page, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_bg_opa(_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_page, 0, 0);
    lv_obj_set_style_radius(_page, 0, 0);
    lv_obj_set_style_pad_all(_page, 0, 0);
    lv_obj_set_style_shadow_width(_page, 0, 0);
    lv_obj_add_event_cb(_page, _on_page_clicked, LV_EVENT_CLICKED, this);  // 点空白处收起键盘

    parent = _page;  // 之后所有卡片都挂到内容层

    /* -------------------------- 顶部状态栏 -------------------------- */
    lv_obj_t* accent = lv_obj_create(parent);
    lv_obj_set_size(accent, 6, 34);
    lv_obj_set_style_radius(accent, 3, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(COL_PRI), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(accent, 0, 0);
    lv_obj_remove_flag(accent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(accent, LV_ALIGN_TOP_LEFT, 40, 30);

    lv_obj_t* title = _mk_label(parent, "WiFi Station", &lv_font_montserrat_28, COL_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 60, 28);

    _pill = lv_obj_create(parent);
    lv_obj_set_size(_pill, 190, 36);
    lv_obj_set_style_radius(_pill, 18, 0);
    lv_obj_set_style_bg_color(_pill, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_bg_opa(_pill, LV_OPA_40, 0);
    lv_obj_set_style_border_width(_pill, 1, 0);
    lv_obj_set_style_border_color(_pill, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_pad_all(_pill, 0, 0);
    lv_obj_remove_flag(_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(_pill, LV_ALIGN_TOP_LEFT, 300, 30);

    _pill_dot = lv_obj_create(_pill);
    lv_obj_set_size(_pill_dot, 10, 10);
    lv_obj_set_style_radius(_pill_dot, 5, 0);
    lv_obj_set_style_bg_color(_pill_dot, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_style_bg_opa(_pill_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_pill_dot, 0, 0);
    lv_obj_remove_flag(_pill_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(_pill_dot, LV_ALIGN_LEFT_MID, 16, 0);

    _pill_text = _mk_label(_pill, "STANDBY", &lv_font_montserrat_16, COL_TEXT2);
    lv_obj_align(_pill_text, LV_ALIGN_LEFT_MID, 34, 0);

    lv_obj_t* hint = _mk_label(parent, "Swipe right to return", &lv_font_montserrat_18, COL_TEXT3);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -40, 36);

    /* -------------------------- 可用网络列表 ------------------------- */
    lv_obj_t* left_card = _create_card(parent, 40, 96, 400, 580, "AVAILABLE NETWORKS", nullptr);

    _scan_btn = _mk_pill_button(left_card, 290, 12, 92, 34, COL_PRI, "Scan", COL_TEXT);
    _scan_btn_txt = lv_obj_get_child(_scan_btn, 0);
    lv_obj_add_event_cb(_scan_btn, _on_scan_clicked, LV_EVENT_CLICKED, this);

    _list = lv_obj_create(left_card);
    lv_obj_set_pos(_list, 16, 62);
    lv_obj_set_size(_list, 368, 502);
    lv_obj_set_style_bg_opa(_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_pad_all(_list, 0, 0);
    lv_obj_set_style_pad_row(_list, 8, 0);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(_list, LV_SCROLLBAR_MODE_ACTIVE);

    _list_hint = _mk_label(left_card, "Tap Scan to search\nnearby networks", &lv_font_montserrat_18, COL_TEXT3);
    lv_obj_set_width(_list_hint, 340);
    lv_label_set_long_mode(_list_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_list_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_list_hint, LV_ALIGN_TOP_LEFT, 30, 260);

    /* --------------------------- 连接信息卡 -------------------------- */
    lv_obj_t* conn_card = _create_card(parent, 460, 96, 380, 250, "CONNECTION", nullptr);
    _mk_kv_row(conn_card, 58, "SSID", &_v_ssid);
    _mk_kv_row(conn_card, 88, "IP ADDRESS", &_v_ip);
    _mk_kv_row(conn_card, 118, "GATEWAY", &_v_gw);
    _mk_kv_row(conn_card, 148, "MAC", &_v_mac);
    _mk_kv_row(conn_card, 178, "SIGNAL", &_v_rssi);

    _conn_hint = _mk_label(conn_card, "Select a network to connect", &lv_font_montserrat_14, COL_TEXT3);
    lv_obj_align(_conn_hint, LV_ALIGN_TOP_LEFT, 22, 212);

    /* --------------------------- 服务信息卡 -------------------------- */
    lv_obj_t* svc_card = _create_card(parent, 860, 96, 380, 250, "LOCAL SERVICES", nullptr);
    _mk_service_row(svc_card, 58, "WEB", &_web_dot, &_v_web);
    _mk_service_row(svc_card, 94, "TCP", &_tcp_dot, &_v_tcp);
    _mk_service_row(svc_card, 130, "UDP", &_udp_dot, &_v_udp);

    _v_clients = _mk_label(svc_card, "TCP clients: 0", &lv_font_montserrat_14, COL_TEXT3);
    lv_obj_align(_v_clients, LV_ALIGN_TOP_LEFT, 22, 180);

    lv_obj_t* svc_hint = _mk_label(svc_card, "PC can browse the web page and use\nTCP / UDP tools in the same LAN",
                                  &lv_font_montserrat_14, COL_TEXT3);
    lv_obj_align(svc_hint, LV_ALIGN_TOP_LEFT, 22, 204);

    /* --------------------------- 数据监视卡 -------------------------- */
    lv_obj_t* data_card = _create_card(parent, 460, 358, 780, 318, "DATA MONITOR", nullptr);

    lv_obj_t* clear_btn = _mk_pill_button(data_card, 668, 12, 92, 34, 0x2A3140, "Clear", COL_TEXT2);
    lv_obj_add_event_cb(clear_btn, _on_clear_clicked, LV_EVENT_CLICKED, this);

    _log_ta = lv_textarea_create(data_card);
    lv_obj_set_pos(_log_ta, 18, 56);
    lv_obj_set_size(_log_ta, 744, 170);
    lv_textarea_set_max_length(_log_ta, 4096);
    lv_textarea_set_one_line(_log_ta, false);
    lv_textarea_set_cursor_click_pos(_log_ta, false);
    lv_textarea_set_password_mode(_log_ta, false);
    // 注意：CLICKABLE 必须保留 —— lv_obj_hit_test() 对不带该标志的对象直接返回 false，
    // 会导致触摸命中不到它、拖动事件落到不可滚动的父卡片上，监视窗就划不动了。
    // 只去掉 CLICK_FOCUSABLE：既能拖动滚动，又不会在点击时抢走发送框的焦点。
    lv_obj_remove_flag(_log_ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_border_width(_log_ta, 0, 0);
    lv_obj_set_style_bg_color(_log_ta, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(_log_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_log_ta, 14, 0);
    lv_obj_set_style_pad_all(_log_ta, 12, 0);
    lv_obj_set_style_text_font(_log_ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_log_ta, lv_color_hex(COL_TEXT2), 0);

    _log_hint = _mk_label(data_card, "No data yet", &lv_font_montserrat_18, COL_TEXT3);
    lv_obj_align(_log_hint, LV_ALIGN_TOP_MID, 0, 118);

    _send_ta = lv_textarea_create(data_card);
    lv_obj_set_pos(_send_ta, 18, 238);
    lv_obj_set_size(_send_ta, 424, 48);
    lv_textarea_set_one_line(_send_ta, true);
    lv_textarea_set_max_length(_send_ta, 200);
    lv_textarea_set_placeholder_text(_send_ta, "Type a message to send...");
    lv_obj_set_style_bg_color(_send_ta, lv_color_hex(0x0F131A), 0);
    lv_obj_set_style_bg_opa(_send_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_send_ta, 1, 0);
    lv_obj_set_style_border_color(_send_ta, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_radius(_send_ta, 12, 0);
    lv_obj_set_style_text_font(_send_ta, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(_send_ta, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_color(_send_ta, lv_color_hex(COL_TEXT3), LV_PART_TEXTAREA_PLACEHOLDER);

    _chan_tcp = _mk_pill_button(data_card, 452, 238, 88, 48, COL_PRI, "TCP", COL_TEXT);
    _chan_tcp_txt = lv_obj_get_child(_chan_tcp, 0);
    lv_obj_add_event_cb(_chan_tcp, _on_chan_tcp_clicked, LV_EVENT_CLICKED, this);

    _chan_udp = _mk_pill_button(data_card, 548, 238, 88, 48, 0x2A3140, "UDP", COL_TEXT2);
    _chan_udp_txt = lv_obj_get_child(_chan_udp, 0);
    lv_obj_add_event_cb(_chan_udp, _on_chan_udp_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* send_btn = _mk_pill_button(data_card, 648, 238, 114, 48, COL_PRI, "Send", COL_TEXT);
    lv_obj_add_event_cb(send_btn, _on_send_clicked, LV_EVENT_CLICKED, this);

    _update_send_channel();

    // 软键盘挂在屏幕上（不是内容层），这样页面平移时它固定贴底
    _build_send_keyboard(scr);
}

void WifiStationView::_build_password_modal(lv_obj_t* parent)
{
    lv_display_t* disp = lv_display_get_default();
    const int32_t sw   = lv_display_get_horizontal_resolution(disp);
    const int32_t sh   = lv_display_get_vertical_resolution(disp);

    // 键盘占屏幕下方约 42%（720p 上约 302px，每行约 75px，能放下 20 号字），
    // 输入卡片紧贴键盘上方，输入框与键盘之间不再隔着按钮行。
    const int32_t kb_h     = (sh * 42) / 100;
    const int32_t card_w   = sw - 160;
    const int32_t card_h   = 212;
    const int32_t card_gap = 18;  // 卡片底边到键盘顶边的间距

    _modal = lv_obj_create(parent);
    lv_obj_set_size(_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_center(_modal);
    lv_obj_remove_flag(_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(_modal, 0, 0);
    lv_obj_set_style_border_width(_modal, 0, 0);
    lv_obj_set_style_shadow_width(_modal, 0, 0);
    lv_obj_set_style_bg_color(_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_modal, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(_modal, 0, 0);
    lv_obj_add_flag(_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_modal, _on_modal_cancel, LV_EVENT_CLICKED, this);

    /* ------------------- 输入卡片：紧贴软键盘上方 ------------------- */
    lv_obj_t* card = _mk_card(_modal, 0, 0, card_w, card_h);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -(kb_h + card_gap));

    _modal_ssid = _mk_label(card, "Connect to network", &lv_font_montserrat_20, COL_TEXT);
    lv_obj_set_width(_modal_ssid, card_w - 68);
    lv_label_set_long_mode(_modal_ssid, LV_LABEL_LONG_DOT);
    lv_obj_align(_modal_ssid, LV_ALIGN_TOP_LEFT, 34, 22);

    _modal_pass = lv_textarea_create(card);
    lv_obj_align(_modal_pass, LV_ALIGN_TOP_LEFT, 34, 60);
    lv_obj_set_size(_modal_pass, card_w - 68, 58);
    lv_textarea_set_one_line(_modal_pass, true);
    lv_textarea_set_password_mode(_modal_pass, true);
    lv_textarea_set_max_length(_modal_pass, 63);
    lv_textarea_set_placeholder_text(_modal_pass, "Password (leave empty for open network)");
    lv_obj_set_style_bg_color(_modal_pass, lv_color_hex(0x0F131A), 0);
    lv_obj_set_style_bg_opa(_modal_pass, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_modal_pass, 1, 0);
    lv_obj_set_style_border_color(_modal_pass, lv_color_hex(COL_PRI), 0);
    lv_obj_set_style_radius(_modal_pass, 12, 0);
    lv_obj_set_style_text_font(_modal_pass, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(_modal_pass, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_color(_modal_pass, lv_color_hex(COL_TEXT3), LV_PART_TEXTAREA_PLACEHOLDER);

    _modal_err = _mk_label(card, "", &lv_font_montserrat_16, COL_ERR);
    lv_obj_align(_modal_err, LV_ALIGN_BOTTOM_LEFT, 34, -26);

    /* 按钮放在卡片内右下角（原来放在卡片底部，会被键盘盖住点不到） */
    lv_obj_t* cancel_btn = _mk_pill_button(card, 0, 0, 150, 54, 0x2A3140, "Cancel", COL_TEXT2);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -218, -18);
    lv_obj_add_event_cb(cancel_btn, _on_modal_cancel, LV_EVENT_CLICKED, this);

    _modal_connect = _mk_pill_button(card, 0, 0, 170, 54, COL_PRI, "Connect", COL_TEXT);
    lv_obj_align(_modal_connect, LV_ALIGN_BOTTOM_RIGHT, -32, -18);
    _modal_connect_txt = lv_obj_get_child(_modal_connect, 0);
    lv_obj_add_event_cb(_modal_connect, _on_modal_connect, LV_EVENT_CLICKED, this);

    /* ------------------------ 软键盘（屏幕底部） ------------------------
       注意：lv_keyboard 的构造函数内部会执行 lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, 0)，
       而 LVGL 中一旦对象带有 align，lv_obj_set_pos() 就会被忽略。这正是之前键盘
       跑到卡片最底部、盖住按钮且离输入框很远的根因。所以键盘必须用 lv_obj_align
       定位，且作为模态层的直接子对象，绝不能再对它调用 lv_obj_set_pos()。 */
    _modal_kb = lv_keyboard_create(_modal);
    lv_obj_set_width(_modal_kb, LV_PCT(100));
    lv_obj_set_height(_modal_kb, kb_h);
    lv_obj_align(_modal_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(_modal_kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_keyboard_set_mode(_modal_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(_modal_kb, _on_keyboard, LV_EVENT_READY, this);
    lv_obj_add_event_cb(_modal_kb, _on_keyboard, LV_EVENT_CANCEL, this);

    lv_keyboard_set_textarea(_modal_kb, _modal_pass);
    lv_obj_add_flag(_modal, LV_OBJ_FLAG_HIDDEN);

    mclog::tagInfo(_tag, "password modal layout: screen {}x{} kb_h={} card={}x{}", sw, sh, kb_h, card_w, card_h);
}

/* ----------------------------- 发送用软键盘 ----------------------------- */

void WifiStationView::_build_send_keyboard(lv_obj_t* screen)
{
    lv_display_t* disp = lv_display_get_default();
    const int32_t sh   = lv_display_get_vertical_resolution(disp);
    _send_kb_h = (sh * 40) / 100;

    /* 与密码弹窗里的键盘同理：lv_keyboard 构造函数内部会执行
       lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, 0)，一旦对象带 align，
       lv_obj_set_pos() 就会被忽略，所以这里只能用 lv_obj_align 定位。 */
    _send_kb = lv_keyboard_create(screen);
    lv_obj_set_width(_send_kb, LV_PCT(100));
    lv_obj_set_height(_send_kb, _send_kb_h);
    lv_obj_align(_send_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(_send_kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_keyboard_set_mode(_send_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(_send_kb, _send_ta);
    lv_obj_add_event_cb(_send_kb, _on_send_keyboard, LV_EVENT_READY, this);
    lv_obj_add_event_cb(_send_kb, _on_send_keyboard, LV_EVENT_CANCEL, this);

    lv_obj_add_event_cb(_send_ta, _on_send_focused, LV_EVENT_FOCUSED, this);
    lv_obj_add_event_cb(_send_ta, _on_send_focused, LV_EVENT_DEFOCUSED, this);

    lv_obj_add_flag(_send_kb, LV_OBJ_FLAG_HIDDEN);
    mclog::tagInfo(_tag, "send keyboard built: kb_h={}", _send_kb_h);
}

void WifiStationView::_show_send_keyboard(bool show)
{
    if (_send_kb == nullptr || _page == nullptr || show == _send_kb_open) {
        return;
    }
    _send_kb_open = show;

    if (show) {
        lv_obj_remove_flag(_send_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(_page);

        lv_area_t ta{};
        lv_area_t kb{};
        lv_obj_get_coords(_send_ta, &ta);
        lv_obj_get_coords(_send_kb, &kb);

        // 把内容层整体上移，使输入框底边露在键盘顶边之上（屏幕上移越多越靠上）
        int32_t shift = (ta.y2 + 12) - kb.y1;
        if (shift < 0) {
            shift = 0;
        }
        if (shift > kb.y1) {
            shift = kb.y1;  // 极端小屏时最多让出整个键盘高度
        }
        lv_obj_set_y(_page, -shift);
    } else {
        lv_obj_set_y(_page, 0);
        lv_obj_add_flag(_send_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* --------------------------------- 激活 ---------------------------------- */

void WifiStationView::setActive(bool active)
{
    if (_active == active) {
        return;
    }
    _active = active;

    if (active) {
        _sta_kicked = false;
        // 已连接时无需再扫描
        auto info = GetHAL()->wifiGetStaInfo();
        if (info.state == WifiStaState_t::WIFI_STA_CONNECTED) {
            _sta_kicked = true;
        }
    } else {
        _sta_kicked = false;
        if (_modal_open) {
            _close_password_modal();
        }
    }
}

bool WifiStationView::isModalOpen()
{
    return _modal_open;
}

/* --------------------------------- 刷新 ---------------------------------- */

void WifiStationView::update()
{
    if (!_inited) {
        return;
    }

    auto info     = GetHAL()->wifiGetStaInfo();
    auto mode     = GetHAL()->wifiGetMode();
    bool scanning = GetHAL()->wifiIsScanning();

    // 进入第二屏后：已保存凭据则自动重连，否则自动扫描一次
    if (_active && !_sta_kicked && mode == WifiMode_t::WIFI_MODE_STA) {
        _sta_kicked = true;
        if (GetHAL()->wifiHasSavedCredential()) {
            GetHAL()->wifiConnectSaved();
        } else {
            GetHAL()->wifiScanStart();
        }
    }

    _update_status(info, mode, scanning);
    _update_connection_card(info, mode);
    _update_service_card(info);
    _update_list(info, mode, scanning);
    _fetch_log();

    // 密码弹窗：连接结果反馈
    if (_modal_open && _modal_connecting) {
        if (info.state == WifiStaState_t::WIFI_STA_CONNECTED) {
            _modal_connecting = false;
            _close_password_modal();
        } else if (info.state == WifiStaState_t::WIFI_STA_FAILED ||
                   info.state == WifiStaState_t::WIFI_STA_TIMEOUT) {
            _modal_connecting = false;
            lv_obj_remove_state(_modal_connect, LV_STATE_DISABLED);
            _set_text(_modal_connect_txt, "Connect");
            if (info.state == WifiStaState_t::WIFI_STA_TIMEOUT) {
                _set_text(_modal_err, "Connection timed out, please check the password");
            } else {
                _set_text(_modal_err, "Failed: " + _reason_text(info.failReason));
            }
        }
    }
}

void WifiStationView::_update_status(const WifiStaInfo_t& info, WifiMode_t mode, bool scanning)
{
    int state = static_cast<int>(info.state);
    int mode_i = static_cast<int>(mode);
    if (state == _last_state && mode_i == _last_mode && scanning == _last_scanning) {
        return;
    }
    _last_state    = state;
    _last_mode     = mode_i;
    _last_scanning = scanning;

    std::string text;
    uint32_t color = COL_MUTE;

    if (scanning) {
        text  = "SCANNING";
        color = COL_INFO;
    } else if (mode != WifiMode_t::WIFI_MODE_STA) {
        text  = "HOTSPOT MODE";
        color = COL_MUTE;
    } else {
        switch (info.state) {
            case WifiStaState_t::WIFI_STA_SCANNING:
                text  = "SCANNING";
                color = COL_INFO;
                break;
            case WifiStaState_t::WIFI_STA_CONNECTING:
                text  = "CONNECTING";
                color = COL_PRI;
                break;
            case WifiStaState_t::WIFI_STA_CONNECTED:
                text  = "CONNECTED";
                color = COL_OK;
                break;
            case WifiStaState_t::WIFI_STA_FAILED:
                text  = "FAILED";
                color = COL_ERR;
                break;
            case WifiStaState_t::WIFI_STA_TIMEOUT:
                text  = "TIMEOUT";
                color = COL_WARN;
                break;
            default:
                text  = "IDLE";
                color = COL_MUTE;
                break;
        }
    }

    _set_text(_pill_text, text);
    _set_dot_color(_pill_dot, color);
    lv_obj_set_style_bg_color(_pill, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(_pill, LV_OPA_30, 0);
    lv_obj_set_style_border_color(_pill, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(_pill_text, lv_color_hex(color), 0);
}

void WifiStationView::_update_connection_card(const WifiStaInfo_t& info, WifiMode_t mode)
{
    if (mode != WifiMode_t::WIFI_MODE_STA) {
        _set_text(_v_ssid, "--");
        _set_text(_v_ip, "--");
        _set_text(_v_gw, "--");
        _set_text(_v_mac, "--");
        _set_text(_v_rssi, "--");
        _set_text(_conn_hint, "Hotspot mode, swipe to STA screen");
        return;
    }

    _set_text(_v_ssid, info.ssid.empty() ? "--" : info.ssid);
    _set_text(_v_ip, info.ip.empty() ? "--" : info.ip);
    _set_text(_v_gw, info.gateway.empty() ? "--" : info.gateway);
    _set_text(_v_mac, info.mac.empty() ? "--" : info.mac);

    if (info.state == WifiStaState_t::WIFI_STA_CONNECTED && info.rssi != 0) {
        _set_text(_v_rssi, std::to_string(static_cast<int>(info.rssi)) + " dBm");
    } else {
        _set_text(_v_rssi, "--");
    }

    if (info.state == WifiStaState_t::WIFI_STA_CONNECTED) {
        _set_text(_conn_hint, "Services running, peer can reach the device");
    } else if (info.state == WifiStaState_t::WIFI_STA_FAILED) {
        _set_text(_conn_hint, "Last failure: " + _reason_text(info.failReason));
    } else if (info.state == WifiStaState_t::WIFI_STA_TIMEOUT) {
        _set_text(_conn_hint, "Last attempt timed out");
    } else if (info.ssid.empty()) {
        _set_text(_conn_hint, "Select a network to connect");
    } else {
        _set_text(_conn_hint, "Trying " + info.ssid + " ...");
    }
}

void WifiStationView::_update_service_card(const WifiStaInfo_t& info)
{
    bool up = info.servicesUp;

    std::string web = up ? ("http://" + info.ip + "/") : "not running";
    if (up && info.ip.empty()) {
        web = "not running";
    }

    _set_text(_v_web, web);
    _set_text(_v_tcp, up ? (std::to_string(info.tcpPort)) : std::string("not running"));
    _set_text(_v_udp, up ? (std::to_string(info.udpPort)) : std::string("not running"));
    _set_text(_v_clients, "TCP clients: " + std::to_string(info.tcpClients));

    bool data_up = up && !info.ip.empty();
    _set_dot_color(_web_dot, up ? COL_OK : COL_MUTE);
    _set_dot_color(_tcp_dot, data_up ? COL_OK : COL_MUTE);
    _set_dot_color(_udp_dot, data_up ? COL_OK : COL_MUTE);
}

void WifiStationView::_update_list(const WifiStaInfo_t& info, WifiMode_t mode, bool scanning)
{
    (void)info;

    // 扫描按钮状态
    if (scanning != _scan_btn_busy) {
        _scan_btn_busy = scanning;
        if (scanning) {
            _set_text(_scan_btn_txt, "Scanning");
            lv_obj_add_state(_scan_btn, LV_STATE_DISABLED);
            _set_text(_list_hint, "Scanning nearby networks...");
            lv_obj_remove_flag(_list_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            _set_text(_scan_btn_txt, "Scan");
            lv_obj_remove_state(_scan_btn, LV_STATE_DISABLED);
        }
    }

    if (mode != WifiMode_t::WIFI_MODE_STA) {
        if (_last_list_key != "__hotspot__") {
            _last_list_key = "__hotspot__";
            _ap_list.clear();
            lv_obj_clean(_list);
            _set_text(_list_hint, "WiFi is in hotspot mode.\nSwipe here again to switch to Station");
            lv_obj_remove_flag(_list_hint, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // 扫描结果拷贝开销较大，400ms 拉取一次即可
    uint32_t now = GetHAL()->millis();
    if (scanning || (now - _list_poll_tick) > 400) {
        _list_poll_tick = now;
        _ap_list        = GetHAL()->wifiGetScanResults();
    }

    std::string key;
    key.reserve(_ap_list.size() * 24);
    for (const auto& ap : _ap_list) {
        key += ap.ssid;
        key += '|';
        key += std::to_string(static_cast<int>(ap.rssi));
        key += ';';
    }

    if (key != _last_list_key) {
        _last_list_key = key;

        lv_obj_clean(_list);

        for (size_t i = 0; i < _ap_list.size(); i++) {
            const auto& ap = _ap_list[i];

            lv_obj_t* row = lv_obj_create(_list);
            lv_obj_set_size(row, 356, 66);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_radius(row, 14, 0);
            bool selected = (static_cast<int32_t>(i) == _selected);
            lv_obj_set_style_bg_color(row, lv_color_hex(selected ? COL_ROW_ON : COL_ROW), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(selected ? COL_PRI : COL_LINE), 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(i) + 1));
            lv_obj_add_event_cb(row, _on_row_clicked, LV_EVENT_CLICKED, this);

            lv_obj_t* ssid = _mk_label(row, ap.ssid.c_str(), &lv_font_montserrat_20, COL_TEXT);
            lv_obj_set_width(ssid, 190);
            lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
            lv_obj_align(ssid, LV_ALIGN_TOP_LEFT, 16, 12);

            char detail[64];
            snprintf(detail, sizeof(detail), "%d dBm   %s", static_cast<int>(ap.rssi), ap.encrypted ? "LOCKED" : "OPEN");
            lv_obj_t* sub = _mk_label(row, detail, &lv_font_montserrat_14, COL_TEXT3);
            lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 16, -12);

            int level = _rssi_level(ap.rssi);
            for (int b = 0; b < 5; b++) {
                lv_obj_t* bar = lv_obj_create(row);
                int32_t bar_h = 8 + b * 5;
                lv_obj_set_size(bar, 5, bar_h);
                lv_obj_set_style_radius(bar, 2, 0);
                lv_obj_set_style_bg_color(bar, lv_color_hex(b < level ? (level >= 4 ? COL_OK : COL_WARN) : 0x333B48), 0);
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(bar, 0, 0);
                lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 300 + b * 9, 52 - bar_h);
            }
        }

        if (_ap_list.empty()) {
            _set_text(_list_hint, "No networks found\nTap Scan to search again");
            lv_obj_remove_flag(_list_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_list_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void WifiStationView::_update_send_channel()
{
    if (_use_udp) {
        lv_obj_set_style_bg_color(_chan_udp, lv_color_hex(COL_PRI), 0);
        lv_obj_set_style_text_color(_chan_udp_txt, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_bg_color(_chan_tcp, lv_color_hex(0x2A3140), 0);
        lv_obj_set_style_text_color(_chan_tcp_txt, lv_color_hex(COL_TEXT2), 0);
    } else {
        lv_obj_set_style_bg_color(_chan_tcp, lv_color_hex(COL_PRI), 0);
        lv_obj_set_style_text_color(_chan_tcp_txt, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_bg_color(_chan_udp, lv_color_hex(0x2A3140), 0);
        lv_obj_set_style_text_color(_chan_udp_txt, lv_color_hex(COL_TEXT2), 0);
    }
}

bool WifiStationView::_fetch_log()
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(GetHAL()->wifiNetData.mutex);
        while (!GetHAL()->wifiNetData.rxQueue.empty()) {
            if (!_log_buf.empty()) {
                _log_buf += "\n";
            }
            _log_buf += GetHAL()->wifiNetData.rxQueue.front();
            GetHAL()->wifiNetData.rxQueue.pop();
            changed = true;
        }
    }

    if (!changed) {
        return false;
    }

    // 限制日志长度，避免长时间运行后内存与刷新开销过大
    if (_log_buf.size() > 2600) {
        size_t cut = _log_buf.find('\n', _log_buf.size() - 2000);
        if (cut != std::string::npos) {
            _log_buf.erase(0, cut + 1);
        }
    }

    lv_textarea_set_text(_log_ta, _log_buf.c_str());
    lv_obj_scroll_to_view(lv_textarea_get_label(_log_ta), LV_ANIM_OFF);

    if (!_log_buf.empty()) {
        lv_obj_add_flag(_log_hint, LV_OBJ_FLAG_HIDDEN);
    }
    return true;
}

/* --------------------------------- 交互 ---------------------------------- */

void WifiStationView::_select_row(int32_t index)
{
    _selected = index;
    _last_list_key.clear();  // 触发下一次刷新时重建列表以更新选中态
}

void WifiStationView::_open_password_modal(const std::string& ssid)
{
    _modal_ssid_str    = ssid;
    _modal_connecting  = false;

    _set_text(_modal_ssid, "Connect to  " + ssid);
    _set_text(_modal_err, "");
    lv_textarea_set_text(_modal_pass, "");
    lv_obj_remove_state(_modal_connect, LV_STATE_DISABLED);
    _set_text(_modal_connect_txt, "Connect");

    lv_obj_remove_flag(_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_modal);
    lv_keyboard_set_textarea(_modal_kb, _modal_pass);

    // 密码弹窗打开时收起发送键盘，避免两个键盘同时抢输入
    if (_send_kb != nullptr) {
        lv_keyboard_set_textarea(_send_kb, nullptr);
    }
    _show_send_keyboard(false);

    // 首次打开时打印真实几何，便于在真机上核对：键盘应紧贴输入框下方且完整落在屏内
    if (!_geom_logged) {
        _geom_logged = true;
        lv_obj_update_layout(_modal);
        lv_area_t ma{};
        lv_area_t ta{};
        lv_area_t ka{};
        lv_obj_get_coords(_modal, &ma);
        lv_obj_get_coords(_modal_pass, &ta);
        lv_obj_get_coords(_modal_kb, &ka);
        mclog::tagInfo(_tag, "modal geom: modal[x {}..{} y {}..{}] pass[x {}..{} y {}..{}] kb[x {}..{} y {}..{}]",
                       static_cast<int>(ma.x1), static_cast<int>(ma.x2), static_cast<int>(ma.y1),
                       static_cast<int>(ma.y2), static_cast<int>(ta.x1), static_cast<int>(ta.x2),
                       static_cast<int>(ta.y1), static_cast<int>(ta.y2), static_cast<int>(ka.x1),
                       static_cast<int>(ka.x2), static_cast<int>(ka.y1), static_cast<int>(ka.y2));
    }

    _modal_open = true;
    mclog::tagInfo(_tag, "password modal opened for {}", ssid);
}

void WifiStationView::_close_password_modal()
{
    lv_keyboard_set_textarea(_modal_kb, nullptr);
    lv_obj_add_flag(_modal, LV_OBJ_FLAG_HIDDEN);
    if (_send_kb != nullptr) {
        lv_keyboard_set_textarea(_send_kb, _send_ta);
    }
    _modal_open       = false;
    _modal_connecting = false;
}

void WifiStationView::_submit_password()
{
    if (_modal_ssid_str.empty()) {
        return;
    }

    const char* pass = lv_textarea_get_text(_modal_pass);
    std::string password = (pass != nullptr) ? pass : "";

    mclog::tagInfo(_tag, "connecting to {} ({} chars)", _modal_ssid_str, password.size());
    GetHAL()->wifiConnect(_modal_ssid_str, password);

    _modal_connecting = true;
    _set_text(_modal_err, "");
    _set_text(_modal_connect_txt, "Connecting...");
    lv_obj_add_state(_modal_connect, LV_STATE_DISABLED);
}

void WifiStationView::_submit_send()
{
    const char* text = lv_textarea_get_text(_send_ta);
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    GetHAL()->wifiSendData(std::string(text), _use_udp);
    lv_textarea_set_text(_send_ta, "");
}

void WifiStationView::_on_row_clicked(lv_event_t* e)
{
    auto* self = static_cast<WifiStationView*>(lv_event_get_user_data(e));
    auto* row  = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (self == nullptr || row == nullptr) {
        return;
    }

    intptr_t index = reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)) - 1;
    if (index < 0 || index >= static_cast<intptr_t>(self->_ap_list.size())) {
        return;
    }

    self->_select_row(static_cast<int32_t>(index));
    self->_open_password_modal(self->_ap_list[static_cast<size_t>(index)].ssid);
}

void WifiStationView::_on_scan_clicked(lv_event_t* e)
{
    auto* self = static_cast<WifiStationView*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    _set_text(self->_list_hint, "Scanning nearby networks...");
    GetHAL()->wifiScanStart();
}

void WifiStationView::_on_send_clicked(lv_event_t* e)
{
    auto* self = static_cast<WifiStationView*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }

    self->_submit_send();
    self->_show_send_keyboard(false);
}

void WifiStationView::_on_clear_clicked(lv_event_t* e)
{
    auto* self = static_cast<WifiStationView*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }

    // 同步清空：直接把待拉取的队列在这里清掉，不等服务任务异步处理，
    // 否则 30ms 的刷新周期可能先把旧数据回填，界面看上去"没反应"。
    size_t queue_before = 0;
    {
        std::lock_guard<std::mutex> lock(GetHAL()->wifiNetData.mutex);
        auto& q = GetHAL()->wifiNetData.rxQueue;
        queue_before = q.size();
        while (!q.empty()) {
            q.pop();
        }
    }

    const size_t buf_before = self->_log_buf.size();
    self->_log_buf.clear();
    lv_textarea_set_text(self->_log_ta, "");
    lv_obj_remove_flag(self->_log_hint, LV_OBJ_FLAG_HIDDEN);

    // 仍然通知服务端清空网页侧历史记录（异步）
    GetHAL()->wifiClearRxLog();

    mclog::tagInfo(_tag, "clear clicked: buf_before={} queue_before={}", buf_before, queue_before);
}

void WifiStationView::_on_chan_tcp_clicked(lv_event_t* e)
{
    auto* self = static_cast<WifiStationView*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    self->_use_udp = false;
    self->_update_send_channel();
}

void WifiStationView::_on_chan_udp_clicked(lv_event_t* e)
{
    auto* self = static_cast<WifiStationView*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    self->_use_udp = true;
    self->_update_send_channel();
}

void WifiStationView::_on_modal_connect(lv_event_t* e)
{
    auto* self = static_cast<WifiStationView*>(lv_event_get_user_data(e));
    if (self == nullptr || self->_modal_connecting) {
        return;
    }
    self->_submit_password();
}

void WifiStationView::_on_modal_cancel(lv_event_t* e)
{
    auto* self = static_cast<WifiStationView*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    self->_close_password_modal();
}

void WifiStationView::_on_keyboard(lv_event_t* e)
{
    auto* self = static_cast<WifiStationView*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }

    if (lv_event_get_code(e) == LV_EVENT_READY) {
        if (!self->_modal_connecting) {
            self->_submit_password();
        }
    } else {
        self->_close_password_modal();
    }
}

void WifiStationView::_on_send_focused(lv_event_t* e)
{
    auto* self = static_cast<WifiStationView*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    self->_show_send_keyboard(lv_event_get_code(e) == LV_EVENT_FOCUSED);
}

void WifiStationView::_on_send_keyboard(lv_event_t* e)
{
    auto* self = static_cast<WifiStationView*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }

    // 键盘的确认键直接发送，关闭键只收起
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        self->_submit_send();
    }
    self->_show_send_keyboard(false);
}

void WifiStationView::_on_page_clicked(lv_event_t* e)
{
    auto* self = static_cast<WifiStationView*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    // 点击内容层空白处收起发送键盘
    self->_show_send_keyboard(false);
}

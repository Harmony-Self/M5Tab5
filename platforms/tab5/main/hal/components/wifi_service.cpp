/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/components/wifi_service.h"
#include "hal/components/wifi_server.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi_default.h>
#include <esp_wifi_netif.h>
#include <esp_private/wifi.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <mooncake_log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* TAG            = "wifi_svc";
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kRssiIntervalMs   = 2000;
constexpr int kMaxScanEntries        = 20;
constexpr int kMaxLogItems           = 64;
constexpr int kServiceTaskStack      = 6144;

constexpr const char* kNvsNamespace = "wifi_cfg";
constexpr const char* kApSsid       = "M5Tab5-UserDemo-WiFi";
constexpr uint8_t kApMaxConn        = 4;

esp_netif_t* s_netif_ap  = nullptr;
esp_netif_t* s_netif_sta = nullptr;

/**
 * @brief 服务启停前后的诊断快照（只在模式/连接状态真正变化时打印，不会常态刷屏）
 *
 * 目的：如果 httpd 任务删除路径里真的存在堆破坏，这里能在 wifi_svc 任务（6144B 栈）
 * 上安全地把情况打印出来，而不是让 IDF 内部的 1536B 临时任务以栈溢出形式掩盖问题。
 */
void log_service_diagnostics(const char* phase)
{
    WifiServer& server = WifiServer::instance();

    // 只校验内部 RAM 堆（任务栈/TCB 都在这里），避免遍历 27MB PSRAM 堆拖慢切换
    const bool heap_ok         = heap_caps_check_integrity(MALLOC_CAP_INTERNAL, false);
    const size_t heap_int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t heap_int_max  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const UBaseType_t stack_wh = uxTaskGetStackHighWaterMark(nullptr);

    ESP_LOGI(TAG, "[diag:%s] heap_int_free=%u largest=%u heap_ok=%d http=%d data=%d tcp_clients=%d svc_stack_lwm=%u",
             phase, static_cast<unsigned>(heap_int_free), static_cast<unsigned>(heap_int_max), heap_ok ? 1 : 0,
             server.running() ? 1 : 0, server.dataRunning() ? 1 : 0, server.tcp_client_count(),
             static_cast<unsigned>(stack_wh));

    if (!heap_ok) {
        ESP_LOGE(TAG, "[diag:%s] HEAP INTEGRITY CHECK FAILED, possible heap corruption!", phase);
    }
}

/**
 * @brief 复刻 IDF wifi_default.c 中的 wifi_start()，完成远端 WiFi 下 netif 的启动动作
 */
void wifi_netif_start(esp_netif_t* netif, esp_event_base_t base, int32_t event_id, void* data)
{
    auto driver = static_cast<wifi_netif_driver_t>(esp_netif_get_io_driver(netif));
    if (driver == nullptr) {
        ESP_LOGE(TAG, "netif has no io driver");
        return;
    }

    uint8_t mac[6] = {};
    esp_err_t ret  = esp_wifi_get_if_mac(driver, mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_if_mac failed: %s", esp_err_to_name(ret));
        return;
    }

    if (esp_wifi_is_if_ready_when_started(driver)) {
        ret = esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_register_if_rxcb failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    ret = esp_wifi_internal_reg_netstack_buf_cb(esp_netif_netstack_buf_ref, esp_netif_netstack_buf_free);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "netstack cb register failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_netif_set_mac(netif, mac);
    esp_netif_action_start(netif, base, event_id, data);
}

/* --------------------------------- AP 侧 ---------------------------------- */

void ap_start_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (esp_netif_is_netif_up(netif)) {
        return;
    }
    wifi_netif_start(netif, base, event_id, data);
}

void ap_stop_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (!esp_netif_is_netif_up(netif)) {
        return;
    }
    esp_netif_action_stop(netif, base, event_id, data);
}

/* -------------------------------- STA 侧 ---------------------------------- */

void sta_start_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (esp_netif_is_netif_up(netif)) {
        return;
    }
    wifi_netif_start(netif, base, event_id, data);
}

void sta_stop_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (!esp_netif_is_netif_up(netif)) {
        return;
    }
    esp_netif_action_stop(netif, base, event_id, data);
}

void sta_connected_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    auto driver = static_cast<wifi_netif_driver_t>(esp_netif_get_io_driver(netif));

    // STA 的 rxcb 在连接后才就绪
    if (driver != nullptr && !esp_wifi_is_if_ready_when_started(driver)) {
        esp_err_t ret = esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "sta register rxcb failed: %s", esp_err_to_name(ret));
            return;
        }
    }
    esp_netif_action_connected(netif, base, event_id, data);
}

void sta_disconnected_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    esp_netif_action_disconnected(static_cast<esp_netif_t*>(arg), base, event_id, data);
}

void sta_got_ip_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    esp_wifi_internal_set_sta_ip();
    esp_netif_action_got_ip(static_cast<esp_netif_t*>(arg), base, event_id, data);
}

/* ------------------------------- netif 管理 -------------------------------- */

void detach_and_destroy(esp_netif_t* netif)
{
    auto driver = static_cast<wifi_netif_driver_t>(esp_netif_get_io_driver(netif));
    esp_netif_driver_ifconfig_t empty_cfg = {};
    esp_netif_set_driver_config(netif, &empty_cfg);
    if (driver != nullptr) {
        esp_wifi_destroy_if_driver(driver);
    }
    esp_netif_destroy(netif);
}

bool create_ap_netif()
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_AP();
    s_netif_ap             = esp_netif_new(&cfg);
    if (s_netif_ap == nullptr) {
        ESP_LOGE(TAG, "create ap netif failed");
        return false;
    }

    esp_err_t ret = esp_netif_attach_wifi_ap(s_netif_ap);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "attach ap netif failed: %s", esp_err_to_name(ret));
        esp_netif_destroy(s_netif_ap);
        s_netif_ap = nullptr;
        return false;
    }

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, ap_start_handler, s_netif_ap);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, ap_stop_handler, s_netif_ap);
    return true;
}

void destroy_ap_netif()
{
    if (s_netif_ap == nullptr) {
        return;
    }
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_START, ap_start_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STOP, ap_stop_handler);
    detach_and_destroy(s_netif_ap);
    s_netif_ap = nullptr;
}

bool create_sta_netif()
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    s_netif_sta            = esp_netif_new(&cfg);
    if (s_netif_sta == nullptr) {
        ESP_LOGE(TAG, "create sta netif failed");
        return false;
    }

    esp_err_t ret = esp_netif_attach_wifi_station(s_netif_sta);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "attach sta netif failed: %s", esp_err_to_name(ret));
        esp_netif_destroy(s_netif_sta);
        s_netif_sta = nullptr;
        return false;
    }

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, sta_start_handler, s_netif_sta);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP, sta_stop_handler, s_netif_sta);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, sta_connected_handler, s_netif_sta);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, sta_disconnected_handler, s_netif_sta);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, sta_got_ip_handler, s_netif_sta);
    return true;
}

void destroy_sta_netif()
{
    if (s_netif_sta == nullptr) {
        return;
    }
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START, sta_start_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_STOP, sta_stop_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, sta_connected_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, sta_disconnected_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, sta_got_ip_handler);
    detach_and_destroy(s_netif_sta);
    s_netif_sta = nullptr;
}

/* --------------------------- 应用层事件分发（轻量） -------------------------- */

void app_event_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    if (base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_SCAN_DONE:
                WifiService::instance().notifyScanDone();
                break;
            case WIFI_EVENT_STA_CONNECTED:
                WifiService::instance().notifyStaConnected();
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                auto* event = static_cast<wifi_event_sta_disconnected_t*>(data);
                WifiService::instance().notifyStaDisconnected(event ? event->reason : -1);
                break;
            }
            default:
                break;
        }
    } else if (base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            auto* event  = static_cast<ip_event_got_ip_t*>(data);
            char ip[32]  = {};
            char gw[32]  = {};
            if (event != nullptr) {
                esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));
                esp_ip4addr_ntoa(&event->ip_info.gw, gw, sizeof(gw));
            }
            WifiService::instance().notifyGotIp(ip, gw);
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            WifiService::instance().notifyLostIp();
        }
    }
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                                 WifiService                                */
/* -------------------------------------------------------------------------- */

WifiService& WifiService::instance()
{
    static WifiService s_instance;
    return s_instance;
}

void WifiService::start()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_started) {
            return;
        }
        _started = true;
        _wake    = xSemaphoreCreateBinary();
    }

    if (xTaskCreate(_task_trampoline, "wifi_svc", kServiceTaskStack, this, 5, &_task) != pdPASS) {
        ESP_LOGE(TAG, "create wifi service task failed");
        std::lock_guard<std::mutex> lock(_mutex);
        _started = false;
    }
}

void WifiService::_task_trampoline(void* arg)
{
    static_cast<WifiService*>(arg)->_run();
    vTaskDelete(nullptr);
}

void WifiService::_run()
{
    _init_once();

    while (1) {
        if (_wake != nullptr) {
            xSemaphoreTake(_wake, pdMS_TO_TICKS(50));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        std::deque<Command_t> commands;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            commands.swap(_cmds);
        }
        for (const auto& cmd : commands) {
            _handle_command(cmd);
        }

        _fetch_scan_results();
        _check_connect_timeout();
        _update_rssi();
        _refresh_services();
    }
}

void WifiService::_post(const Command_t& cmd)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_cmds.size() >= 32) {
            _cmds.pop_front();
        }
        _cmds.push_back(cmd);
    }
    if (_wake != nullptr) {
        xSemaphoreGive(_wake);
    }
}

/* ------------------------------- 对外接口 -------------------------------- */

void WifiService::setMode(WifiMode_t mode)
{
    Command_t cmd;
    cmd.type = Cmd_t::SetMode;
    cmd.mode = mode;
    _post(cmd);
}

void WifiService::scanStart()
{
    Command_t cmd;
    cmd.type = Cmd_t::Scan;
    _post(cmd);
}

void WifiService::connect(const std::string& ssid, const std::string& pass)
{
    Command_t cmd;
    cmd.type  = Cmd_t::Connect;
    cmd.text  = ssid;
    cmd.text2 = pass;
    _post(cmd);
}

void WifiService::connectSaved()
{
    Command_t cmd;
    cmd.type = Cmd_t::ConnectSaved;
    _post(cmd);
}

void WifiService::disconnect()
{
    Command_t cmd;
    cmd.type = Cmd_t::Disconnect;
    _post(cmd);
}

void WifiService::sendData(const std::string& data, bool useUdp)
{
    Command_t cmd;
    cmd.type = Cmd_t::Send;
    cmd.text = data;
    cmd.flag = useUdp;
    _post(cmd);
}

void WifiService::clearLog()
{
    Command_t cmd;
    cmd.type = Cmd_t::ClearLog;
    _post(cmd);
}

WifiService::WifiStaInfo_t WifiService::staInfo()
{
    WifiStaInfo_t info;
    std::lock_guard<std::mutex> lock(_mutex);
    info.state       = _sta_state;
    info.ssid        = _sta_ssid;
    info.ip          = _sta_ip;
    info.gateway     = _sta_gw;
    info.mac         = _sta_mac;
    info.rssi        = _sta_rssi;
    info.failReason  = _sta_fail_reason;
    info.servicesUp  = _services_up;
    info.httpPort    = WifiServer::kHttpPort;
    info.tcpPort     = WifiServer::kTcpPort;
    info.udpPort     = WifiServer::kUdpPort;
    info.tcpClients  = _tcp_clients;
    return info;
}

bool WifiService::isScanning()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _scanning;
}

WifiService::WifiMode_t WifiService::mode()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _mode;
}

std::vector<WifiService::WifiApInfo_t> WifiService::scanResults()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _scan_results;
}

bool WifiService::hasSavedCredential()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _has_saved;
}

void WifiService::setTcpClientCount(int count)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _tcp_clients = count;
}

void WifiService::pushRxLog(const std::string& origin, const std::string& data)
{
    _push_log(origin, data);
}

void WifiService::_push_log(const std::string& origin, const std::string& data)
{
    std::string body = data;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
        body.pop_back();
    }

    std::string line = "[" + origin + "] " + body;

    auto* hal = hal::Get();
    std::lock_guard<std::mutex> lock(hal->wifiNetData.mutex);
    hal->wifiNetData.rxQueue.push(line);
    while (hal->wifiNetData.rxQueue.size() > static_cast<size_t>(kMaxLogItems)) {
        hal->wifiNetData.rxQueue.pop();
    }
}

/* ------------------------------- 事件通知 -------------------------------- */

void WifiService::notifyScanDone()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _scan_fetch_pending = true;
}

void WifiService::notifyStaConnected()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mode != hal::HalBase::WIFI_MODE_STA) {
        return;
    }
    mclog::tagInfo(TAG, "sta connected, waiting for dhcp");
}

void WifiService::notifyStaDisconnected(int reason)
{
    bool was_connected = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_mode != hal::HalBase::WIFI_MODE_STA) {
            return;
        }
        // 由自身 disconnect / 模式切换引发的事件在 1s 宽限期内忽略
        if (_intentional_disc_tick != 0 &&
            (xTaskGetTickCount() - _intentional_disc_tick) < pdMS_TO_TICKS(1000)) {
            return;
        }
        was_connected = (_sta_state == hal::HalBase::WIFI_STA_CONNECTED);
        _sta_ip.clear();
        _sta_gw.clear();
        _sta_rssi     = 0;
        _save_pending = false;

        if (_sta_state != hal::HalBase::WIFI_STA_TIMEOUT) {
            _sta_state       = hal::HalBase::WIFI_STA_FAILED;
            _sta_fail_reason = reason;
        }
    }

    ESP_LOGW(TAG, "sta disconnected, reason %d%s", reason, was_connected ? " (was connected)" : "");
    if (was_connected) {
        _push_log("SYS", "Disconnected, reason " + std::to_string(reason));
    }
}

void WifiService::notifyGotIp(const std::string& ip, const std::string& gateway)
{
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_mode != hal::HalBase::WIFI_MODE_STA) {
            return;
        }
        _sta_ip             = ip;
        _sta_gw             = gateway;
        _sta_state          = hal::HalBase::WIFI_STA_CONNECTED;
        _sta_fail_reason    = 0;
        _credential_pending = true;
        _rssi_tick          = xTaskGetTickCount();
        ok                  = true;
    }
    if (ok) {
        mclog::tagInfo(TAG, "got ip: {}", ip);
        _push_log("SYS", "Connected, IP " + ip);
    }
}

void WifiService::notifyLostIp()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _sta_ip.clear();
    _sta_gw.clear();
}

/* -------------------------------- 命令处理 -------------------------------- */

void WifiService::_handle_command(const Command_t& cmd)
{
    switch (cmd.type) {
        case Cmd_t::SetMode:
            if (cmd.mode == hal::HalBase::WIFI_MODE_AP) {
                _enter_ap();
            } else if (cmd.mode == hal::HalBase::WIFI_MODE_STA) {
                _enter_sta();
            }
            break;

        case Cmd_t::Scan:
            _handle_scan();
            break;

        case Cmd_t::Connect:
            _start_connect(cmd.text, cmd.text2, true);
            break;

        case Cmd_t::ConnectSaved: {
            std::string ssid;
            std::string pass;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                ssid = _saved_ssid;
                pass = _saved_pass;
            }
            if (ssid.empty()) {
                ESP_LOGW(TAG, "no saved credential");
                _push_log("SYS", "No saved network");
                break;
            }
            _start_connect(ssid, pass, false);
            break;
        }

        case Cmd_t::Disconnect:
            _handle_disconnect();
            break;

        case Cmd_t::Send: {
            bool up = false;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                up = _services_up;
            }
            if (!up) {
                ESP_LOGW(TAG, "send dropped: services not running");
                _push_log("SYS", "Services not running, send dropped");
                break;
            }
            WifiServer::instance().sendAll(cmd.text, cmd.flag);
            _push_log(cmd.flag ? "TX-UDP" : "TX-TCP", cmd.text);
            break;
        }

        case Cmd_t::ClearLog: {
            // 先在小作用域内清空屏幕日志队列并立刻释放锁，
            // 再清历史记录，避免 wifiNetData.mutex 与 _history_mutex 嵌套加锁
            {
                auto* hal = hal::Get();
                std::lock_guard<std::mutex> lock(hal->wifiNetData.mutex);
                std::queue<std::string> empty;
                std::swap(hal->wifiNetData.rxQueue, empty);
            }
            WifiServer::instance().clearHistory();
            break;
        }
    }
}

/* --------------------------------- 初始化 --------------------------------- */

void WifiService::_init_once()
{
    if (_inited.load()) {
        return;
    }

    mclog::tagInfo(TAG, "init once");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(ret));
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "create default event loop failed: %s", esp_err_to_name(ret));
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret                    = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, app_event_handler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, app_event_handler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, app_event_handler, nullptr);

    _load_credential();

    // HTTP 服务常驻：初始化完成后立即拉起来（首页内容随后由 setApMode 决定），
    // 之后无论 AP ↔ STA 如何切换都不再停止它
    if (!WifiServer::instance().startHttp()) {
        ESP_LOGE(TAG, "start http server failed");
    }

    log_service_diagnostics("init");

    _inited.store(true);
}

void WifiService::_load_credential()
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    char ssid[33] = {};
    char pass[65] = {};
    size_t len    = sizeof(ssid);
    bool ok       = false;

    if (nvs_get_str(handle, "ssid", ssid, &len) == ESP_OK && ssid[0] != '\0') {
        len = sizeof(pass);
        if (nvs_get_str(handle, "pass", pass, &len) != ESP_OK) {
            pass[0] = '\0';
        }
        ok = true;
    }
    nvs_close(handle);

    if (!ok) {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _has_saved  = true;
    _saved_ssid = ssid;
    _saved_pass = pass;
    mclog::tagInfo(TAG, "saved network found: {}", _saved_ssid);
}

void WifiService::_save_credential(const std::string& ssid, const std::string& pass)
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "open nvs for write failed");
        return;
    }

    nvs_set_str(handle, "ssid", ssid.c_str());
    nvs_set_str(handle, "pass", pass.c_str());
    nvs_commit(handle);
    nvs_close(handle);

    std::lock_guard<std::mutex> lock(_mutex);
    _has_saved  = true;
    _saved_ssid = ssid;
    _saved_pass = pass;

    mclog::tagInfo(TAG, "credential saved: {}", ssid);
}

/* ------------------------------ 模式切换 --------------------------------- */

void WifiService::_teardown_wifi()
{
    _mark_intentional_disconnect();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _mode               = hal::HalBase::WIFI_MODE_OFF;
        _sta_state          = hal::HalBase::WIFI_STA_IDLE;
        _sta_fail_reason    = 0;
        _save_pending       = false;
        _credential_pending = false;
        _scanning           = false;
    }

    // 只停 TCP/UDP 数据通道；HTTP 服务常驻，切模式也不会停止（详见 WifiServer 类注释）
    WifiServer::instance().stop();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _services_up = false;
        _tcp_clients = 0;
        _srv_data_on = false;
    }

    if (_wifi_started) {
        esp_wifi_disconnect();
        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(ret));
        }
        _wifi_started = false;
    }

    destroy_ap_netif();
    destroy_sta_netif();
}

void WifiService::_mark_intentional_disconnect()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _intentional_disc_tick = xTaskGetTickCount();
}

void WifiService::_enter_ap()
{
    if (_mode == hal::HalBase::WIFI_MODE_AP && _wifi_started) {
        return;
    }

    mclog::tagInfo(TAG, "switch to AP mode");
    _teardown_wifi();

    if (!create_ap_netif()) {
        return;
    }

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.ap.ssid), kApSsid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len       = std::strlen(kApSsid);
    wifi_config.ap.max_connection = kApMaxConn;
    wifi_config.ap.authmode       = WIFI_AUTH_OPEN;

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set mode AP failed: %s", esp_err_to_name(ret));
        destroy_ap_netif();
        return;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set AP config failed: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        destroy_ap_netif();
        return;
    }

    _wifi_started = true;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _mode = hal::HalBase::WIFI_MODE_AP;
        _sta_ssid.clear();
        _sta_pass.clear();
        _sta_ip.clear();
        _sta_gw.clear();
        _sta_rssi = 0;
    }

    ESP_LOGI(TAG, "Wi-Fi AP started. SSID:%s url:http://192.168.4.1", kApSsid);
    _push_log("SYS", "Web page http://192.168.4.1");
}

void WifiService::_enter_sta()
{
    if (_mode == hal::HalBase::WIFI_MODE_STA && _wifi_started) {
        return;
    }

    mclog::tagInfo(TAG, "switch to STA mode");
    _teardown_wifi();

    if (!create_sta_netif()) {
        return;
    }

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set mode STA failed: %s", esp_err_to_name(ret));
        destroy_sta_netif();
        return;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        destroy_sta_netif();
        return;
    }

    uint8_t mac[6]  = {};
    char mac_str[18] = {};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                 mac[5]);
    }

    _wifi_started = true;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _mode          = hal::HalBase::WIFI_MODE_STA;
        _sta_state     = hal::HalBase::WIFI_STA_IDLE;
        _sta_fail_reason = 0;
        _sta_mac       = mac_str;
        _sta_ssid.clear();
        _sta_pass.clear();
        _sta_ip.clear();
        _sta_gw.clear();
        _sta_rssi = 0;
    }

    ESP_LOGI(TAG, "STA mode ready, mac %s", mac_str);
}

/* -------------------------------- 扫描 ----------------------------------- */

void WifiService::_handle_scan()
{
    if (_mode != hal::HalBase::WIFI_MODE_STA || !_wifi_started) {
        ESP_LOGW(TAG, "scan ignored: not in STA mode");
        _push_log("SYS", "Scan requires STA mode");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _scanning = true;
    }

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden        = false;

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(ret));
        std::lock_guard<std::mutex> lock(_mutex);
        _scanning = false;
    } else {
        ESP_LOGI(TAG, "scan started");
    }
}

void WifiService::_fetch_scan_results()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_scan_fetch_pending) {
            return;
        }
        _scan_fetch_pending = false;
    }

    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) {
        count = 0;
    }

    std::vector<WifiApInfo_t> results;
    if (count > 0) {
        // 防御：远端 esp_wifi_remote 的 RPC 实现未必严格遵守入参上限，
        // 因此按上报数量分配完整缓冲（不做展示用途的截断），避免越界写；
        // 展示条数在排序之后再截断。
        std::vector<wifi_ap_record_t> records(count);
        uint16_t got = count;
        if (esp_wifi_scan_get_ap_records(&got, records.data()) == ESP_OK) {
            if (got > count) {
                ESP_LOGW(TAG, "scan returned %u records but only %u slots, truncating", got, count);
                got = count;
            }
            results.reserve(got);
            for (uint16_t i = 0; i < got; i++) {
                if (records[i].ssid[0] == '\0') {
                    continue;
                }
                WifiApInfo_t info;
                info.ssid      = reinterpret_cast<const char*>(records[i].ssid);
                info.rssi      = records[i].rssi;
                info.encrypted = (records[i].authmode != WIFI_AUTH_OPEN);
                results.push_back(info);
            }
        }
    }

    std::sort(results.begin(), results.end(),
              [](const WifiApInfo_t& a, const WifiApInfo_t& b) { return a.rssi > b.rssi; });

    // 只展示信号最强的若干条，避免列表过长拖慢刷新
    if (results.size() > static_cast<size_t>(kMaxScanEntries)) {
        results.resize(kMaxScanEntries);
    }

    size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _scan_results = std::move(results);
        _scanning     = false;
        total         = _scan_results.size();
    }

    ESP_LOGI(TAG, "scan done, %u networks", static_cast<unsigned>(total));
}

/* -------------------------------- 连接 ----------------------------------- */

void WifiService::_start_connect(const std::string& ssid, const std::string& pass, bool save)
{
    if (_mode != hal::HalBase::WIFI_MODE_STA || !_wifi_started) {
        ESP_LOGW(TAG, "connect ignored: not in STA mode");
        _push_log("SYS", "STA mode not ready");
        return;
    }
    if (ssid.empty()) {
        return;
    }

    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid.c_str(), sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), pass.c_str(), sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(ret));
        _push_log("SYS", "Set config failed");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _sta_ssid        = ssid;
        _sta_pass        = pass;
        _sta_ip.clear();
        _sta_gw.clear();
        _sta_rssi        = 0;
        _sta_state       = hal::HalBase::WIFI_STA_CONNECTING;
        _sta_fail_reason = 0;
        _save_pending    = save;
        _connect_tick    = xTaskGetTickCount();
    }

    _mark_intentional_disconnect();
    esp_wifi_disconnect();

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        _set_state(hal::HalBase::WIFI_STA_FAILED, static_cast<int>(ret));
        _push_log("SYS", "Connect failed to start");
        return;
    }

    ESP_LOGI(TAG, "connecting to %s ...", ssid.c_str());
    _push_log("SYS", "Connecting to " + ssid);
}

void WifiService::_handle_disconnect()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _save_pending = false;
        _sta_ip.clear();
        _sta_gw.clear();
        _sta_rssi  = 0;
        _sta_state = hal::HalBase::WIFI_STA_IDLE;
    }

    _mark_intentional_disconnect();
    esp_wifi_disconnect();

    _push_log("SYS", "Disconnected by user");
}

void WifiService::_check_connect_timeout()
{
    bool timeout = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_sta_state == hal::HalBase::WIFI_STA_CONNECTING &&
            (xTaskGetTickCount() - _connect_tick) > pdMS_TO_TICKS(kConnectTimeoutMs)) {
            _sta_state    = hal::HalBase::WIFI_STA_TIMEOUT;
            _save_pending = false;
            timeout       = true;
        }
    }

    if (!timeout) {
        return;
    }

    ESP_LOGW(TAG, "connect timeout");
    _mark_intentional_disconnect();
    esp_wifi_disconnect();
    _push_log("SYS", "Connect timeout");
}

void WifiService::_update_rssi()
{
    bool connected = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        connected = (_mode == hal::HalBase::WIFI_MODE_STA && _sta_state == hal::HalBase::WIFI_STA_CONNECTED);
    }
    if (!connected) {
        return;
    }

    uint32_t now = xTaskGetTickCount();
    if ((now - _rssi_tick) < pdMS_TO_TICKS(kRssiIntervalMs)) {
        return;
    }
    _rssi_tick = now;

    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        std::lock_guard<std::mutex> lock(_mutex);
        _sta_rssi = ap.rssi;
    }
}

/* ------------------------------ 服务端管理 ------------------------------- */

void WifiService::_refresh_services()
{
    bool want_up         = false;
    bool ap_mode         = false;
    bool save_credential = false;
    std::string save_ssid;
    std::string save_pass;
    std::string sta_ip;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        // 首页策略：只要不是 STA 模式，就显示第一屏的 Hello 页
        ap_mode   = (_mode != hal::HalBase::WIFI_MODE_STA);
        // 数据通道只在 STA 且拿到 IP 之后才有意义
        want_up   = ap_mode ? false
                            : (_sta_state == hal::HalBase::WIFI_STA_CONNECTED);
        sta_ip    = _sta_ip;

        if (_credential_pending) {
            _credential_pending = false;
            save_credential     = _save_pending;
            save_ssid           = _sta_ssid;
            save_pass           = _sta_pass;
            _save_pending       = false;
        }
    }

    if (save_credential && !save_ssid.empty()) {
        _save_credential(save_ssid, save_pass);
    }

    // 状态没变化就不碰服务端，避免每 50ms 重复启停
    if (ap_mode == _srv_ap_mode && want_up == _srv_data_on) {
        return;
    }
    _srv_ap_mode = ap_mode;
    _srv_data_on = want_up;

    WifiServer& server = WifiServer::instance();

    // HTTP 常驻（幂等，仅首次真正启动）+ 首页随模式即时切换
    if (!server.startHttp()) {
        ESP_LOGE(TAG, "ensure http server failed");
    }
    server.setApMode(ap_mode);

    log_service_diagnostics("before");
    bool data_ok = false;
    if (want_up) {
        data_ok = server.startData();
        if (!data_ok) {
            ESP_LOGE(TAG, "start data channels failed");
            _push_log("SYS", "Start services failed");
        }
    } else {
        server.stop();  // 只停 TCP/UDP
    }
    log_service_diagnostics("after");

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _services_up = (want_up && data_ok);
    }

    if (want_up && data_ok) {
        mclog::tagInfo(TAG, "local services ready");
        _push_log("SYS", "Web http://" + sta_ip + "/  TCP:8888  UDP:8889");
    }
}

void WifiService::_set_state(WifiStaState_t state, int reason)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _sta_state       = state;
    _sta_fail_reason = reason;
}

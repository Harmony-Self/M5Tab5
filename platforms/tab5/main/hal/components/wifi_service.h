/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <hal/hal.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief WiFi 服务
 *
 * 负责 AP（第一屏：热点 + 网页）与 STA（第二屏：连接外部路由器）两种工作模式的
 * 互斥切换。两种模式不共存：切换时先停掉服务端，再 stop / 销毁 netif，最后按新
 * 模式重建，避免模式冲突导致功能异常。
 *
 * 所有对外接口均为**非阻塞**（只向命令队列投递 / 加锁读状态快照），因此可以在
 * LVGL 定时器回调里安全调用；真正阻塞的操作（esp_wifi_stop/start、scan、connect、
 * NVS 读写、服务端启停）全部在内部 wifi_svc 任务中串行执行。
 */
class WifiService {
public:
    using WifiMode_t     = hal::HalBase::WifiMode_t;
    using WifiStaState_t = hal::HalBase::WifiStaState_t;
    using WifiApInfo_t   = hal::HalBase::WifiApInfo_t;
    using WifiStaInfo_t  = hal::HalBase::WifiStaInfo_t;

    static WifiService& instance();

    /* ------------------------------- 生命周期 -------------------------------- */

    /** @brief 非阻塞启动内部任务（幂等）。NVS / esp_netif / esp_wifi 初始化在该任务中完成 */
    void start();

    /* ------------------------------ 非阻塞命令 ------------------------------- */

    /** @brief 切换工作模式：WIFI_MODE_AP（第一屏） / WIFI_MODE_STA（第二屏） */
    void setMode(WifiMode_t mode);

    /** @brief 触发一次扫描（仅 STA 模式下有意义） */
    void scanStart();

    /** @brief 连接指定热点，成功后凭据写入 NVS */
    void connect(const std::string& ssid, const std::string& pass);

    /** @brief 使用 NVS 中保存的凭据连接（自动重连） */
    void connectSaved();

    /** @brief 断开 STA 连接 */
    void disconnect();

    /** @brief 通过 TCP / UDP 服务端发送数据（useUdp=true 走 UDP） */
    void sendData(const std::string& data, bool useUdp);

    /** @brief 清空屏幕数据监视记录 */
    void clearLog();

    /* ------------------------------- 状态快照 -------------------------------- */

    WifiStaInfo_t staInfo();
    bool isScanning();
    std::vector<WifiApInfo_t> scanResults();

    /** @brief 当前工作模式（AP / STA / OFF） */
    WifiMode_t mode();

    /** @brief NVS 中是否保存过凭据（开机时由内部任务读取并缓存） */
    bool hasSavedCredential();

    /* -------------------- 供事件回调 / 服务端模块使用（内部） ------------------ */

    void pushRxLog(const std::string& origin, const std::string& data);
    void setTcpClientCount(int count);

    void notifyScanDone();
    void notifyStaConnected();
    void notifyStaDisconnected(int reason);
    void notifyGotIp(const std::string& ip, const std::string& gateway);
    void notifyLostIp();

private:
    WifiService() = default;
    WifiService(const WifiService&)            = delete;
    WifiService& operator=(const WifiService&) = delete;

    enum class Cmd_t { SetMode, Scan, Connect, ConnectSaved, Disconnect, Send, ClearLog };

    struct Command_t {
        Cmd_t type      = Cmd_t::Scan;
        WifiMode_t mode = hal::HalBase::WIFI_MODE_OFF;
        std::string text;   // ssid / 待发送数据
        std::string text2;  // password
        bool flag = false;  // sendData: useUdp
    };

    static void _task_trampoline(void* arg);
    void _run();
    void _post(const Command_t& cmd);
    void _handle_command(const Command_t& cmd);

    void _init_once();
    void _load_credential();
    void _save_credential(const std::string& ssid, const std::string& pass);

    void _enter_ap();
    void _enter_sta();
    void _teardown_wifi();
    void _start_connect(const std::string& ssid, const std::string& pass, bool save);
    void _handle_scan();
    void _fetch_scan_results();
    void _handle_disconnect();

    void _refresh_services();
    void _check_connect_timeout();
    void _update_rssi();

    void _set_state(WifiStaState_t state, int reason = 0);
    void _push_log(const std::string& origin, const std::string& data);
    void _mark_intentional_disconnect();

    std::mutex _mutex;
    TaskHandle_t _task      = nullptr;
    SemaphoreHandle_t _wake = nullptr;
    bool _started           = false;
    std::atomic<bool> _inited{false};

    std::deque<Command_t> _cmds;

    /* 模式与 STA 状态 */
    WifiMode_t _mode          = hal::HalBase::WIFI_MODE_OFF;
    WifiStaState_t _sta_state = hal::HalBase::WIFI_STA_IDLE;
    int _sta_fail_reason      = 0;
    bool _wifi_started        = false;

    /* 扫描 */
    bool _scanning           = false;
    bool _scan_fetch_pending = false;
    std::vector<WifiApInfo_t> _scan_results;

    /* 当前连接信息 */
    std::string _sta_ssid;
    std::string _sta_pass;
    std::string _sta_ip;
    std::string _sta_gw;
    std::string _sta_mac;
    int8_t _sta_rssi = 0;

    /* 连接过程 */
    bool _save_pending            = false;
    bool _credential_pending      = false;
    uint32_t _connect_tick        = 0;
    uint32_t _rssi_tick           = 0;
    /* 主动断开的时间戳：用于屏蔽由自身 disconnect / 模式切换引起的 DISCONNECTED 事件 */
    uint32_t _intentional_disc_tick = 0;

    /* NVS 凭据 */
    bool _has_saved = false;
    std::string _saved_ssid;
    std::string _saved_pass;

    /* 服务端 */
    bool _services_up = false;
    int _tcp_clients  = 0;
    /* WifiServer 侧已应用的状态，避免每轮循环重复调用启停 */
    bool _srv_ap_mode = false;
    bool _srv_data_on = false;
};

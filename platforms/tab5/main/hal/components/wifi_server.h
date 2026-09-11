/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_http_server.h"

/**
 * @brief 局域网服务端：HTTP 调试页 + TCP 透传 + UDP 透传
 *
 * 生命周期（重要）：
 *  - **HTTP 服务进程内只启动一次并一直常驻**，绝不调用 httpd_stop()。
 *    监听 socket 绑定 INADDR_ANY，与具体 netif 无关，因此 AP ↔ STA 切换后
 *    无需重建即可继续服务；首页按当前模式在「第一屏 Hello 页」与「第二屏调试页」
 *    之间动态切换。
 *    之所以禁止 httpd_stop()：httpd 线程由 xTaskCreatePinnedToCoreWithCaps() 创建，
 *    退出时走 vTaskDeleteWithCaps()，ESP-IDF 为此创建仅有 configMINIMAL_STACK_SIZE
 *    (CONFIG_FREERTOS_IDLE_TASK_STACKSIZE) 的临时清理任务 prvTaskDeleteWithCapsTask，
 *    该任务内部一旦发生 printf（断言/堆诊断）就会栈溢出并触发 panic。
 *  - **TCP / UDP 数据通道按需启停**，用普通 xTaskCreate()/vTaskDelete()，
 *    走 idle 任务回收路径，不会触发上述 WithCaps 清理路径。
 */
class WifiServer {
public:
    static constexpr uint16_t kHttpPort = 80;
    static constexpr uint16_t kTcpPort  = 8888;
    static constexpr uint16_t kUdpPort  = 8889;

    static constexpr int kMaxTcpClients = 4;

    static WifiServer& instance();

    /**
     * @brief 确保 HTTP 服务已启动（幂等）
     *
     * 只有首次调用会真正 httpd_start，之后一直常驻、永不停止。
     * @return HTTP 服务是否就绪
     */
    bool startHttp();

    /**
     * @brief 设置首页分发模式（仅切换判据，不启停任何服务，线程安全）
     * @param ap_mode true: 返回第一屏 Hello 页；false: 返回第二屏调试页
     */
    void setApMode(bool ap_mode);

    /** @brief 启动 TCP/UDP 数据通道（幂等） */
    bool startData();

    /** @brief 停止 TCP/UDP 数据通道（幂等）；HTTP 常驻不受影响 */
    void stop();

    /** @brief HTTP 已启动或数据通道正在运行 */
    bool running();

    /** @brief TCP/UDP 数据通道是否正在运行 */
    bool dataRunning();

    /** @brief 当前是否为 AP 模式（决定首页内容），可被 httpd 线程安全读取 */
    bool ap_mode() const;

    /** @brief 向所有已连接 TCP 客户端（useUdp=false）或最近一次 UDP 对端发送数据 */
    void sendAll(const std::string& data, bool useUdp);

    /** @brief 记录一条“外部 -> 设备”的数据（供网页轮询），同时推送到屏幕监视队列 */
    void recordInbound(const std::string& origin, const std::string& data);

    /** @brief 清空历史记录 */
    void clearHistory();

    int tcp_client_count();
    bool udp_peer_alive();
    std::string udp_peer_addr();

    /** @brief 返回自 since 之后的历史记录（JSON 数组），并回传下一个游标 */
    std::string history_json(uint32_t since, uint32_t& next_index);

private:
    WifiServer() = default;
    WifiServer(const WifiServer&)            = delete;
    WifiServer& operator=(const WifiServer&) = delete;

    struct HistoryItem_t {
        uint32_t index = 0;
        std::string origin;
        std::string data;
    };

    /* 注意：没有 _stop_http()，HTTP 服务常驻，详见类注释 */
    bool _start_http();
    bool _start_tcp();
    void _stop_tcp();
    bool _start_udp();
    void _stop_udp();

    static void _tcp_task_trampoline(void* arg);
    void _tcp_loop();
    static void _udp_task_trampoline(void* arg);
    void _udp_loop();

    void _tcp_add_client(int fd);
    void _tcp_remove_client(int fd);
    void _tcp_broadcast(const std::string& data);

    httpd_handle_t _httpd  = nullptr;
    TaskHandle_t _tcp_task = nullptr;
    TaskHandle_t _udp_task = nullptr;
    std::atomic<bool> _tcp_run{false};
    std::atomic<bool> _udp_run{false};
    std::atomic<bool> _tcp_exited{true};
    std::atomic<bool> _udp_exited{true};
    int _tcp_listen = -1;
    int _udp_sock   = -1;
    std::atomic<bool> _ap_mode{true};

    std::mutex _client_mutex;
    std::vector<int> _tcp_clients;

    std::mutex _udp_mutex;
    std::string _udp_peer_addr;
    uint32_t _udp_peer_tick = 0;

    std::mutex _history_mutex;
    std::deque<HistoryItem_t> _history;
    uint32_t _history_next = 1;
};

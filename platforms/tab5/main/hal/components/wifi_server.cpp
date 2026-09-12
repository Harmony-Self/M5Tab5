/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/components/wifi_server.h"
#include "hal/components/wifi_service.h"
#include <esp_log.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#define TAG "wifi_server"

namespace {

constexpr int kTcpTaskStack    = 4096;
constexpr int kUdpTaskStack    = 4096;
constexpr int kHttpTaskStack   = 8192;
constexpr int kMaxHistoryItems = 64;
constexpr int kMaxBodyLen      = 512;

/* ------------------------------- JSON 辅助 -------------------------------- */

std::string json_escape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

bool query_get(httpd_req_t* req, const char* key, char* out, size_t out_len)
{
    char query[128] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, key, out, out_len) == ESP_OK;
}

/* ------------------------------ HTTP 处理函数 ------------------------------ */

/* 第一屏（AP 模式）沿用原有 Hello 页面，保持行为不变 */
const char* kHelloPage = R"HTML(<!DOCTYPE html>
<html>
<head>
    <title>Hello</title>
    <style>
        body {
            display: flex;
            flex-direction: column;
            justify-content: center;
            align-items: center;
            height: 100vh;
            margin: 0;
            font-family: sans-serif;
            background-color: #f0f0f0;
        }
        h1 { font-size: 48px; color: #333; margin: 0; }
        p  { font-size: 18px; color: #666; margin-top: 10px; }
    </style>
</head>
<body>
    <h1>Hello World</h1>
    <p>From M5Tab5</p>
</body>
</html>)HTML";

const char* kIndexPage = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tab5 WiFi Bridge</title>
<style>
:root{--bg:#0E1116;--card:#171B22;--line:#232833;--pri:#3B82F6;--ok:#22C55E;--warn:#F59E0B;--err:#EF4444;--tx:#FFFFFF;--tx2:#A9B1BD;--tx3:#6B7280}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;background:radial-gradient(1000px 600px at 10% -10%,#1b2740,transparent),radial-gradient(900px 500px at 100% 0,#10202b,transparent),var(--bg);color:var(--tx);font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;padding:28px}
.wrap{max-width:1080px;margin:0 auto;display:flex;flex-direction:column;gap:18px}
.head{display:flex;align-items:center;gap:14px;flex-wrap:wrap}
.logo{width:38px;height:38px;border-radius:12px;background:linear-gradient(135deg,#3B82F6,#1D4ED8);display:flex;align-items:center;justify-content:center;font-weight:700;font-size:16px;box-shadow:0 8px 24px rgba(59,130,246,.35)}
h1{font-size:20px;font-weight:600;margin:0;letter-spacing:.2px}
.badge{padding:4px 12px;border-radius:999px;font-size:12px;font-weight:600;border:1px solid transparent}
.b-ok{background:rgba(34,197,94,.14);color:var(--ok);border-color:rgba(34,197,94,.35)}
.b-no{background:rgba(239,68,68,.14);color:var(--err);border-color:rgba(239,68,68,.35)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:18px}
@media(max-width:820px){.grid{grid-template-columns:1fr}}
.card{background:linear-gradient(180deg,rgba(35,40,51,.85),rgba(23,27,34,.85));border:1px solid var(--line);border-radius:18px;padding:18px 20px;backdrop-filter:blur(8px);box-shadow:0 12px 32px rgba(0,0,0,.35)}
.card h2{margin:0 0 14px;font-size:13px;font-weight:600;letter-spacing:1.4px;color:var(--tx2);text-transform:uppercase}
.row{display:flex;justify-content:space-between;gap:12px;padding:7px 0;border-bottom:1px dashed rgba(255,255,255,.06)}
.row:last-child{border-bottom:none}
.k{color:var(--tx3)}
.v{font-weight:600;font-variant-numeric:tabular-nums;text-align:right;word-break:break-all}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
.seg{display:flex;gap:8px;margin-bottom:12px}
.seg button{flex:1;padding:10px;border-radius:12px;border:1px solid var(--line);background:#12161d;color:var(--tx2);cursor:pointer;font-weight:600;transition:.18s}
.seg button.on{background:linear-gradient(135deg,#3B82F6,#1D4ED8);border-color:transparent;color:#fff;box-shadow:0 6px 18px rgba(59,130,246,.35)}
textarea,input{width:100%;background:#0f131a;border:1px solid var(--line);border-radius:12px;color:var(--tx);padding:11px 13px;font:inherit;outline:none;transition:.18s}
textarea:focus,input:focus{border-color:rgba(59,130,246,.7);box-shadow:0 0 0 3px rgba(59,130,246,.15)}
.apply{display:flex;gap:10px;margin-top:12px}
button.act{padding:11px 22px;border-radius:12px;border:none;background:linear-gradient(135deg,#3B82F6,#1D4ED8);color:#fff;font-weight:600;cursor:pointer;transition:.18s}
button.act:hover{transform:translateY(-1px);box-shadow:0 10px 22px rgba(59,130,246,.4)}
button.ghost{background:#12161d;border:1px solid var(--line);color:var(--tx2)}
#log{height:260px;overflow:auto;background:#0d1117;border:1px solid var(--line);border-radius:14px;padding:12px;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:13px}
#log div{padding:3px 0;word-break:break-all;border-bottom:1px solid rgba(255,255,255,.04)}
.src{display:inline-block;min-width:52px;font-weight:700;margin-right:8px}
.s-WEB{color:#38BDF8}.s-TCP{color:#22C55E}.s-UDP{color:#F59E0B}.s-TX{color:#A78BFA}
.s-TX-TCP{color:#A78BFA}.s-TX-UDP{color:#C084FC}
.s-RX-TCP{color:#22C55E}.s-RX-UDP{color:#F59E0B}
.empty{color:var(--tx3);text-align:center;padding:26px 0}
</style>
</head>
<body>
<div class="wrap">
  <div class="head">
    <div class="logo">T5</div>
    <h1>M5Stack Tab5 &middot; WiFi Bridge</h1>
    <span id="badge" class="badge b-no">OFFLINE</span>
    <span style="color:var(--tx3);margin-left:auto" id="clients">TCP clients: 0</span>
  </div>

  <div class="grid">
    <div class="card">
      <h2>Status</h2>
      <div class="row"><span class="k">SSID</span><span class="v" id="f-ssid">—</span></div>
      <div class="row"><span class="k">IP address</span><span class="v mono" id="f-ip">—</span></div>
      <div class="row"><span class="k">Gateway</span><span class="v mono" id="f-gw">—</span></div>
      <div class="row"><span class="k">MAC</span><span class="v mono" id="f-mac">—</span></div>
      <div class="row"><span class="k">Signal</span><span class="v" id="f-rssi">—</span></div>
      <div class="row"><span class="k">Services</span><span class="v" id="f-svc">—</span></div>
      <div class="row"><span class="k">UDP peer</span><span class="v mono" id="f-peer">—</span></div>
    </div>

    <div class="card">
      <h2>Send to device</h2>
      <div class="seg">
        <button id="p-tcp" class="on" onclick="pick(false)">TCP 8888</button>
        <button id="p-udp" onclick="pick(true)">UDP 8889</button>
      </div>
      <textarea id="msg" rows="4" placeholder="Type a message, then press Send"></textarea>
      <div class="apply">
        <button class="act" onclick="send()">Send</button>
        <button class="act ghost" onclick="clearLog()">Clear log</button>
      </div>
      <p style="color:var(--tx3);margin:12px 0 0">TCP clients talk to port <b class="mono">8888</b>; UDP datagrams to port <b class="mono">8889</b> on the device IP above.</p>
    </div>
  </div>

  <div class="card">
    <h2>Data monitor</h2>
    <div id="log"><div class="empty">No data yet</div></div>
  </div>
</div>

<script>
let udp=false, cursor=0, first=true;
function pick(u){udp=u;document.getElementById('p-tcp').className=u?'':'on';document.getElementById('p-udp').className=u?'on':'';}
function add(src,msg){
  const log=document.getElementById('log');
  if(first){log.innerHTML='';first=false;}
  const d=document.createElement('div');
  const s=document.createElement('span');s.className='src s-'+src;s.textContent='['+src+']';
  const t=document.createElement('span');t.textContent=msg;
  d.appendChild(s);d.appendChild(t);log.appendChild(d);log.scrollTop=log.scrollHeight;
}
async function poll(){
  try{
    const r=await fetch('/status');const j=await r.json();
    document.getElementById('badge').className='badge '+(j.services?'b-ok':'b-no');
    document.getElementById('badge').textContent=j.services?'RUNNING':'OFFLINE';
    const sv=(v)=>v? v : '—';
    document.getElementById('f-ssid').textContent=sv(j.ssid);
    document.getElementById('f-ip').textContent=sv(j.ip);
    document.getElementById('f-gw').textContent=sv(j.gw);
    document.getElementById('f-mac').textContent=sv(j.mac);
    document.getElementById('f-rssi').textContent=j.rssi? j.rssi+' dBm':'—';
    document.getElementById('f-svc').textContent='HTTP '+j.http+' · TCP '+j.tcp_port+' · UDP '+j.udp_port;
    document.getElementById('f-peer').textContent=sv(j.udp_peer);
    document.getElementById('clients').textContent='TCP clients: '+j.tcp_clients;
  }catch(e){document.getElementById('badge').className='badge b-no';document.getElementById('badge').textContent='OFFLINE';}
  try{
    const r=await fetch('/data?since='+cursor);const j=await r.json();
    cursor=j.next;
    (j.items||[]).forEach(it=>add(it.src,it.msg));
  }catch(e){}
}
let sending=false;  // 防连点：双击/触屏重复触发会造成一次点击发两条
async function send(){
  if(sending)return;
  const el=document.getElementById('msg');const t=el.value.trim();
  if(!t)return;
  sending=true;
  try{
    const target=udp?'udp':'tcp';
    await fetch('/send?proto='+target,{method:'POST',body:t});
    el.value='';  // 不再本地 add：设备会把发送也记进历史，由 poll 统一渲染，避免重复
  }finally{
    setTimeout(()=>{sending=false;},400);
  }
}
async function clearLog(){await fetch('/clear');cursor=0;first=true;document.getElementById('log').innerHTML='<div class="empty">No data yet</div>';}
setInterval(poll,1000);poll();
</script>
</body>
</html>)HTML";

esp_err_t index_get_handler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (WifiServer::instance().ap_mode()) {
        return httpd_resp_send(req, kHelloPage, HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req, kIndexPage, HTTPD_RESP_USE_STRLEN);
}

std::string build_status_json()
{
    auto info = WifiService::instance().staInfo();
    auto& server = WifiServer::instance();

    std::string peer = server.udp_peer_alive() ? server.udp_peer_addr() : "";

    char buf[640];
    snprintf(buf, sizeof(buf),
             "{\"state\":%d,\"ssid\":\"%s\",\"ip\":\"%s\",\"gw\":\"%s\",\"mac\":\"%s\",\"rssi\":%d,"
             "\"http\":%u,\"tcp_port\":%u,\"udp_port\":%u,\"tcp_clients\":%d,\"services\":%d,\"udp_peer\":\"%s\"}",
             static_cast<int>(info.state), json_escape(info.ssid).c_str(), json_escape(info.ip).c_str(),
             json_escape(info.gateway).c_str(), json_escape(info.mac).c_str(), static_cast<int>(info.rssi),
             static_cast<unsigned>(WifiServer::kHttpPort), static_cast<unsigned>(WifiServer::kTcpPort),
             static_cast<unsigned>(WifiServer::kUdpPort), server.tcp_client_count(), info.servicesUp ? 1 : 0,
             json_escape(peer).c_str());
    return std::string(buf);
}

esp_err_t status_get_handler(httpd_req_t* req)
{
    std::string json = build_status_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), json.size());
}

esp_err_t data_get_handler(httpd_req_t* req)
{
    char since_str[16] = "0";
    query_get(req, "since", since_str, sizeof(since_str));
    uint32_t since = static_cast<uint32_t>(strtoul(since_str, nullptr, 10));

    uint32_t next = since;
    std::string items = WifiServer::instance().history_json(since, next);

    std::string json = "{\"next\":" + std::to_string(next) + ",\"items\":[" + items + "]}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), json.size());
}

esp_err_t clear_get_handler(httpd_req_t* req)
{
    WifiServer::instance().clearHistory();
    WifiService::instance().clearLog();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t send_post_handler(httpd_req_t* req)
{
    char proto[8] = "tcp";
    query_get(req, "proto", proto, sizeof(proto));
    bool use_udp = (strcmp(proto, "udp") == 0);

    if (req->content_len <= 0 || req->content_len > kMaxBodyLen) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body length");
        return ESP_FAIL;
    }

    std::string body(static_cast<size_t>(req->content_len), '\0');
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, &body[received], req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += ret;
    }

    ESP_LOGI(TAG, "web send via %s, %d bytes", use_udp ? "udp" : "tcp", received);
    WifiService::instance().sendData(body, use_udp);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

httpd_uri_t _uri_index = {.uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = nullptr};
httpd_uri_t _uri_status = {.uri = "/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = nullptr};
httpd_uri_t _uri_data   = {.uri = "/data", .method = HTTP_GET, .handler = data_get_handler, .user_ctx = nullptr};
httpd_uri_t _uri_clear  = {.uri = "/clear", .method = HTTP_GET, .handler = clear_get_handler, .user_ctx = nullptr};
httpd_uri_t _uri_send   = {.uri = "/send", .method = HTTP_POST, .handler = send_post_handler, .user_ctx = nullptr};

}  // namespace

/* -------------------------------------------------------------------------- */
/*                                  WifiServer                                */
/* -------------------------------------------------------------------------- */

WifiServer& WifiServer::instance()
{
    static WifiServer s_instance;
    return s_instance;
}

bool WifiServer::running()
{
    return _httpd != nullptr || dataRunning();
}

bool WifiServer::dataRunning()
{
    return _tcp_run.load() || _udp_run.load();
}

bool WifiServer::ap_mode() const
{
    return _ap_mode.load();
}

bool WifiServer::startHttp()
{
    // 幂等：只有首次调用真正启动，之后一直常驻，永不停止
    return _start_http();
}

void WifiServer::setApMode(bool ap_mode)
{
    _ap_mode.store(ap_mode);
}

bool WifiServer::startData()
{
    bool ok = _start_tcp();
    ok      = _start_udp() && ok;
    return ok;
}

void WifiServer::stop()
{
    // 只停数据通道。HTTP 服务常驻，禁止 httpd_stop()（详见头文件说明）
    _stop_udp();
    _stop_tcp();
}

/* --------------------------------- HTTP ----------------------------------- */

bool WifiServer::_start_http()
{
    if (_httpd) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port     = kHttpPort;
    config.stack_size      = kHttpTaskStack;
    config.max_open_sockets = kMaxTcpClients;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&_httpd, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        _httpd = nullptr;
        return false;
    }

    httpd_register_uri_handler(_httpd, &_uri_index);
    httpd_register_uri_handler(_httpd, &_uri_status);
    httpd_register_uri_handler(_httpd, &_uri_data);
    httpd_register_uri_handler(_httpd, &_uri_clear);
    httpd_register_uri_handler(_httpd, &_uri_send);

    ESP_LOGI(TAG, "http server started on port %u", static_cast<unsigned>(kHttpPort));
    return true;
}

/* ---------------------------------- TCP ----------------------------------- */

bool WifiServer::_start_tcp()
{
    if (_tcp_run.load()) {
        return true;
    }

    _tcp_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_tcp_listen < 0) {
        ESP_LOGE(TAG, "tcp socket create failed: errno %d", errno);
        return false;
    }

    int opt = 1;
    setsockopt(_tcp_listen, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family         = AF_INET;
    addr.sin_addr.s_addr    = htonl(INADDR_ANY);
    addr.sin_port           = htons(kTcpPort);

    if (bind(_tcp_listen, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "tcp bind failed: errno %d", errno);
        close(_tcp_listen);
        _tcp_listen = -1;
        return false;
    }
    if (listen(_tcp_listen, kMaxTcpClients) < 0) {
        ESP_LOGE(TAG, "tcp listen failed: errno %d", errno);
        close(_tcp_listen);
        _tcp_listen = -1;
        return false;
    }

    if (_tcp_gate == nullptr) {
        _tcp_gate = xSemaphoreCreateBinary();
        if (_tcp_gate == nullptr) {
            ESP_LOGE(TAG, "create tcp gate failed");
            close(_tcp_listen);
            _tcp_listen = -1;
            return false;
        }
    }

    _tcp_run.store(true);
    _tcp_exited.store(false);

    if (_tcp_task == nullptr) {
        // 首次启动：创建常驻任务（永不删除，原因见头文件说明）
        if (xTaskCreate(_tcp_task_trampoline, "wifi_tcp", kTcpTaskStack, this, 5, &_tcp_task) != pdPASS) {
            ESP_LOGE(TAG, "create tcp task failed");
            _tcp_task = nullptr;
            _tcp_run.store(false);
            _tcp_exited.store(true);
            close(_tcp_listen);
            _tcp_listen = -1;
            return false;
        }
    } else {
        // 任务已存在（处于停止态），唤醒它继续工作
        xSemaphoreGive(_tcp_gate);
    }

    ESP_LOGI(TAG, "tcp server started on port %u", static_cast<unsigned>(kTcpPort));
    return true;
}

void WifiServer::_stop_tcp()
{
    // 未启动时直接返回：避免每次模式切换都做无意义的等待与误导性日志
    if (!_tcp_run.load() && _tcp_listen < 0 && _tcp_task == nullptr) {
        return;
    }

    // 先让任务退出（select 超时 100ms），再关闭 socket，避免任务访问已释放的资源
    _tcp_run.store(false);
    for (int i = 0; i < 30 && !_tcp_exited.load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 任务常驻、不删除：它回到循环顶部时已经关过 socket，这里再关一次是幂等的
    _tcp_close_all();
    WifiService::instance().setTcpClientCount(0);
    ESP_LOGI(TAG, "tcp server stopped");
}

void WifiServer::_tcp_task_trampoline(void* arg)
{
    static_cast<WifiServer*>(arg)->_tcp_loop();
    // 常驻任务不应返回；即使异常返回也绝不能 vTaskDelete（会触发 TLS 删除回调检查并 abort）
    ESP_LOGE(TAG, "tcp task returned unexpectedly");
    vTaskDelay(portMAX_DELAY);
}

void WifiServer::_tcp_close_all()
{
    {
        std::lock_guard<std::mutex> lock(_client_mutex);
        for (int fd : _tcp_clients) {
            close(fd);
        }
        _tcp_clients.clear();
    }

    if (_tcp_listen >= 0) {
        close(_tcp_listen);
        _tcp_listen = -1;
    }
}

void WifiServer::_tcp_loop()
{
    while (true) {
        if (!_tcp_run.load()) {
            // 停止态：关闭监听与客户端，然后阻塞在 gate 上等待下次启动（任务不退出、不删除）
            _tcp_close_all();
            WifiService::instance().setTcpClientCount(0);
            _tcp_exited.store(true);
            xSemaphoreTake(_tcp_gate, portMAX_DELAY);
            continue;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(_tcp_listen, &readfds);
        int max_fd = _tcp_listen;

        std::vector<int> snapshot;
        {
            std::lock_guard<std::mutex> lock(_client_mutex);
            snapshot = _tcp_clients;
        }
        for (int fd : snapshot) {
            FD_SET(fd, &readfds);
            if (fd > max_fd) {
                max_fd = fd;
            }
        }

        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        int ret           = select(max_fd + 1, &readfds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGW(TAG, "tcp select failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (ret == 0) {
            continue;
        }

        if (FD_ISSET(_tcp_listen, &readfds)) {
            struct sockaddr_in from = {};
            socklen_t len           = sizeof(from);
            int fd                  = accept(_tcp_listen, reinterpret_cast<struct sockaddr*>(&from), &len);
            if (fd >= 0) {
                _tcp_add_client(fd);
                char ip[32] = {};
                inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
                ESP_LOGI(TAG, "tcp client connected: %s:%d", ip, ntohs(from.sin_port));
            }
        }

        for (int fd : snapshot) {
            if (!FD_ISSET(fd, &readfds)) {
                continue;
            }

            char buf[513];
            int len = recv(fd, buf, sizeof(buf) - 1, 0);
            if (len > 0) {
                buf[len] = '\0';
                recordInbound("RX-TCP", std::string(buf));
            } else if (len == 0) {
                ESP_LOGI(TAG, "tcp client disconnected");
                _tcp_remove_client(fd);
            } else if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
                ESP_LOGW(TAG, "tcp recv failed: errno %d", errno);
                _tcp_remove_client(fd);
            }
        }
    }
}

void WifiServer::_tcp_add_client(int fd)
{
    // 非阻塞，配合 select 使用
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    std::lock_guard<std::mutex> lock(_client_mutex);
    if (static_cast<int>(_tcp_clients.size()) >= kMaxTcpClients) {
        ESP_LOGW(TAG, "too many tcp clients, reject");
        close(fd);
        return;
    }
    _tcp_clients.push_back(fd);
    WifiService::instance().setTcpClientCount(static_cast<int>(_tcp_clients.size()));
}

void WifiServer::_tcp_remove_client(int fd)
{
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(_client_mutex);
        auto it = std::find(_tcp_clients.begin(), _tcp_clients.end(), fd);
        if (it == _tcp_clients.end()) {
            return;
        }
        _tcp_clients.erase(it);
        close(fd);
        count = static_cast<int>(_tcp_clients.size());
    }
    WifiService::instance().setTcpClientCount(count);
}

void WifiServer::_tcp_broadcast(const std::string& data)
{
    std::vector<int> targets;
    {
        std::lock_guard<std::mutex> lock(_client_mutex);
        targets = _tcp_clients;
    }

    for (int fd : targets) {
        int sent = send(fd, data.c_str(), data.size(), 0);
        if (sent < 0) {
            ESP_LOGW(TAG, "tcp send failed: errno %d", errno);
        }
    }
}

int WifiServer::tcp_client_count()
{
    std::lock_guard<std::mutex> lock(_client_mutex);
    return static_cast<int>(_tcp_clients.size());
}

/* ---------------------------------- UDP ----------------------------------- */

bool WifiServer::_start_udp()
{
    if (_udp_run.load()) {
        return true;
    }

    _udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_udp_sock < 0) {
        ESP_LOGE(TAG, "udp socket create failed: errno %d", errno);
        return false;
    }

    int opt = 1;
    setsockopt(_udp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(_udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {};
    addr.sin_family         = AF_INET;
    addr.sin_addr.s_addr    = htonl(INADDR_ANY);
    addr.sin_port           = htons(kUdpPort);

    if (bind(_udp_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "udp bind failed: errno %d", errno);
        close(_udp_sock);
        _udp_sock = -1;
        return false;
    }

    if (_udp_gate == nullptr) {
        _udp_gate = xSemaphoreCreateBinary();
        if (_udp_gate == nullptr) {
            ESP_LOGE(TAG, "create udp gate failed");
            close(_udp_sock);
            _udp_sock = -1;
            return false;
        }
    }

    _udp_run.store(true);
    _udp_exited.store(false);

    if (_udp_task == nullptr) {
        // 首次启动：创建常驻任务（永不删除，原因见头文件说明）
        if (xTaskCreate(_udp_task_trampoline, "wifi_udp", kUdpTaskStack, this, 5, &_udp_task) != pdPASS) {
            ESP_LOGE(TAG, "create udp task failed");
            _udp_task = nullptr;
            _udp_run.store(false);
            _udp_exited.store(true);
            close(_udp_sock);
            _udp_sock = -1;
            return false;
        }
    } else {
        // 任务已存在（处于停止态），唤醒它继续工作
        xSemaphoreGive(_udp_gate);
    }

    ESP_LOGI(TAG, "udp server started on port %u", static_cast<unsigned>(kUdpPort));
    return true;
}

void WifiServer::_stop_udp()
{
    // 未启动时直接返回：避免每次模式切换都做无意义的等待与误导性日志
    if (!_udp_run.load() && _udp_sock < 0 && _udp_task == nullptr) {
        return;
    }

    // recvfrom 带 200ms 超时，先等任务退出再关闭 socket
    _udp_run.store(false);
    for (int i = 0; i < 30 && !_udp_exited.load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 任务常驻、不删除：它回到循环顶部时已经关过 socket，这里再关一次是幂等的
    _udp_close_all();

    {
        std::lock_guard<std::mutex> lock(_udp_mutex);
        _udp_peer_addr.clear();
        _udp_peer_tick = 0;
    }
    ESP_LOGI(TAG, "udp server stopped");
}

void WifiServer::_udp_close_all()
{
    if (_udp_sock >= 0) {
        close(_udp_sock);
        _udp_sock = -1;
    }
}

void WifiServer::_udp_task_trampoline(void* arg)
{
    static_cast<WifiServer*>(arg)->_udp_loop();
    // 常驻任务不应返回；即使异常返回也绝不能 vTaskDelete（会触发 TLS 删除回调检查并 abort）
    ESP_LOGE(TAG, "udp task returned unexpectedly");
    vTaskDelay(portMAX_DELAY);
}

void WifiServer::_udp_loop()
{
    while (true) {
        if (!_udp_run.load()) {
            // 停止态：关闭 socket 后阻塞在 gate 上等待下次启动（任务不退出、不删除）
            _udp_close_all();
            _udp_exited.store(true);
            xSemaphoreTake(_udp_gate, portMAX_DELAY);
            continue;
        }

        struct sockaddr_in from = {};
        socklen_t len           = sizeof(from);
        char buf[513];
        int ret = recvfrom(_udp_sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<struct sockaddr*>(&from), &len);
        if (ret < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
                continue;
            }
            ESP_LOGW(TAG, "udp recv failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        char ip[32] = {};
        inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
        {
            std::lock_guard<std::mutex> lock(_udp_mutex);
            char peer[64];
            snprintf(peer, sizeof(peer), "%s:%d", ip, ntohs(from.sin_port));
            _udp_peer_addr = peer;
            _udp_peer_tick = xTaskGetTickCount();
        }

        buf[ret] = '\0';
        recordInbound("RX-UDP", std::string(buf));
    }
}

bool WifiServer::udp_peer_alive()
{
    std::lock_guard<std::mutex> lock(_udp_mutex);
    if (_udp_peer_addr.empty()) {
        return false;
    }
    return (xTaskGetTickCount() - _udp_peer_tick) < pdMS_TO_TICKS(60000);
}

std::string WifiServer::udp_peer_addr()
{
    std::lock_guard<std::mutex> lock(_udp_mutex);
    return _udp_peer_addr;
}

/* --------------------------------- 发送 ----------------------------------- */

void WifiServer::sendAll(const std::string& data, bool useUdp)
{
    std::string payload = data;
    if (payload.empty()) {
        return;
    }
    if (payload.back() != '\n') {
        payload += '\n';
    }

    if (useUdp) {
        std::string peer;
        {
            std::lock_guard<std::mutex> lock(_udp_mutex);
            peer = _udp_peer_addr;
        }
        if (peer.empty() || _udp_sock < 0) {
            ESP_LOGW(TAG, "udp send skipped: no peer yet");
            // 上屏提示，否则用户只会觉得“点了发送没反应”
            WifiService::instance().pushRxLog("SYS", "UDP send skipped: no peer yet");
            return;
        }

        struct sockaddr_in addr = {};
        addr.sin_family         = AF_INET;
        addr.sin_port           = htons(kUdpPort);

        char ip[32]    = {};
        int port       = 0;
        const char* colon = strrchr(peer.c_str(), ':');
        if (colon == nullptr) {
            return;
        }
        size_t ip_len = static_cast<size_t>(colon - peer.c_str());
        if (ip_len >= sizeof(ip)) {
            ip_len = sizeof(ip) - 1;
        }
        memcpy(ip, peer.c_str(), ip_len);
        port = atoi(colon + 1);
        if (port <= 0) {
            port = kUdpPort;
        }
        addr.sin_port = htons(static_cast<uint16_t>(port));

        if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
            ESP_LOGW(TAG, "udp peer address invalid: %s", peer.c_str());
            return;
        }

        int sent = sendto(_udp_sock, payload.c_str(), payload.size(), 0, reinterpret_cast<struct sockaddr*>(&addr),
                          sizeof(addr));
        if (sent < 0) {
            ESP_LOGW(TAG, "udp send failed: errno %d", errno);
        }
    } else {
        _tcp_broadcast(payload);
    }
}

/* -------------------------------- 历史记录 -------------------------------- */

void WifiServer::recordInbound(const std::string& origin, const std::string& data)
{
    _record(origin, data);
}

void WifiServer::recordOutbound(const std::string& origin, const std::string& data)
{
    _record(origin, data);
}

void WifiServer::_record(const std::string& origin, const std::string& data)
{
    {
        std::lock_guard<std::mutex> lock(_history_mutex);
        if (static_cast<int>(_history.size()) >= kMaxHistoryItems) {
            _history.pop_front();
        }
        HistoryItem_t item;
        item.index  = _history_next++;
        item.origin = origin;
        item.data   = data;
        _history.push_back(item);
    }

    WifiService::instance().pushRxLog(origin, data);
}

void WifiServer::clearHistory()
{
    std::lock_guard<std::mutex> lock(_history_mutex);
    _history.clear();
}

std::string WifiServer::history_json(uint32_t since, uint32_t& next_index)
{
    std::lock_guard<std::mutex> lock(_history_mutex);
    next_index = _history_next;

    std::string out;
    bool first = true;
    for (const auto& item : _history) {
        // 游标语义：since 是客户端还没拿到的第一个序号，因此只跳过更小的
        if (item.index < since) {
            continue;
        }
        if (!first) {
            out += ",";
        }
        first = false;
        out += "{\"src\":\"" + json_escape(item.origin) + "\",\"msg\":\"" + json_escape(item.data) + "\"}";
    }
    return out;
}

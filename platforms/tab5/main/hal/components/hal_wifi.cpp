/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include "hal/components/wifi_service.h"
#include <bsp/m5stack_tab5.h>
#include <mooncake_log.h>
#include <string>
#include <vector>

#define TAG "wifi"

/**
 * @brief WiFi 的 HAL 层薄封装
 *
 * 具体的模式切换 / 扫描 / 连接 / 服务端管理都在 WifiService 中实现，
 * 这里只做接口转发，所有接口均不阻塞调用方（LVGL 任务可安全调用）。
 */

bool HalEsp32::wifi_init()
{
    mclog::tagInfo(TAG, "wifi service start");

    // 启动内部任务（非阻塞），随后请求进入 AP 模式（第一屏的热点 + 网页）
    WifiService::instance().start();
    WifiService::instance().setMode(hal::HalBase::WIFI_MODE_AP);

    return true;
}

void HalEsp32::startWifiAp()
{
    // 幂等：第一屏启动动画与需要热点的场景都可以重复调用
    wifi_init();
}

void HalEsp32::wifiSetMode(WifiMode_t mode)
{
    WifiService::instance().setMode(mode);
}

bool HalEsp32::wifiHasSavedCredential()
{
    return WifiService::instance().hasSavedCredential();
}

void HalEsp32::wifiConnectSaved()
{
    WifiService::instance().connectSaved();
}

void HalEsp32::wifiScanStart()
{
    WifiService::instance().scanStart();
}

bool HalEsp32::wifiIsScanning()
{
    return WifiService::instance().isScanning();
}

std::vector<hal::HalBase::WifiApInfo_t> HalEsp32::wifiGetScanResults()
{
    return WifiService::instance().scanResults();
}

void HalEsp32::wifiConnect(const std::string& ssid, const std::string& pass)
{
    WifiService::instance().connect(ssid, pass);
}

void HalEsp32::wifiDisconnect()
{
    WifiService::instance().disconnect();
}

hal::HalBase::WifiStaInfo_t HalEsp32::wifiGetStaInfo()
{
    return WifiService::instance().staInfo();
}

hal::HalBase::WifiMode_t HalEsp32::wifiGetMode()
{
    return WifiService::instance().mode();
}

void HalEsp32::wifiSendData(const std::string& data, bool useUdp)
{
    WifiService::instance().sendData(data, useUdp);
}

void HalEsp32::wifiClearRxLog()
{
    WifiService::instance().clearLog();
}

void HalEsp32::setExtAntennaEnable(bool enable)
{
    _ext_antenna_enable = enable;
    mclog::tagInfo(TAG, "set ext antenna enable: {}", _ext_antenna_enable);
    bsp_set_ext_antenna_enable(_ext_antenna_enable);
}

bool HalEsp32::getExtAntennaEnable()
{
    return _ext_antenna_enable;
}

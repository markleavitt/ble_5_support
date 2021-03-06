/*
 * Copyright (c) 2020 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHAN'TABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#undef LOG_COMPILE_TIME_LEVEL

#include "platform_ncp.h"
#include "network/ncp/wifi/ncp.h"
#include "network/ncp/wifi/wifi_network_manager.h"
#include "network/ncp/wifi/wifi_ncp_client.h"
#include "network/ncp/ncp_client.h"
#include "led_service.h"
#include "check.h"
#include "scope_guard.h"
#include "system_led_signal.h"
#include "stream.h"
#include "debug.h"
#include "ota_flash_hal_impl.h"
#include "system_cache.h"

#include <algorithm>

namespace {

class OtaUpdateSourceStream : public particle::InputStream {
public:
    OtaUpdateSourceStream(const uint8_t* buffer, size_t length) : buffer(buffer), remaining(length) {}

    int read(char* data, size_t size) override {
        CHECK(peek(data, size));
        return skip(size);
    }

    int peek(char* data, size_t size) override {
        if (!remaining) {
            return SYSTEM_ERROR_END_OF_STREAM;
        }
        size = std::min(size, remaining);
        memcpy(data, buffer, size);
        return size;
    }

    int skip(size_t size) override {
        if (!remaining) {
            return SYSTEM_ERROR_END_OF_STREAM;
        }
        size = std::min(size, remaining);
        buffer += size;
        remaining -= size;
        LED_Toggle(PARTICLE_LED_RGB);
        return size;
    }

    int availForRead() override {
        return remaining;
    }

    int waitEvent(unsigned flags, unsigned timeout) override {
        if (!flags) {
            return 0;
        }
        if (!(flags & InputStream::READABLE)) {
            return SYSTEM_ERROR_INVALID_ARGUMENT;
        }
        if (!remaining) {
            return SYSTEM_ERROR_END_OF_STREAM;
        }
        return InputStream::READABLE;
    }

private:
    const uint8_t* buffer;
    size_t remaining;
};

int invalidateWifiNcpVersionCache() {
#if HAL_PLATFORM_NCP_COUNT > 1
    using namespace particle::services;
    // Cache will be updated by the NCP client itself
    LOG(TRACE, "Invalidating cached ESP32 NCP firmware version");
    return SystemCache::instance().del(SystemCacheKey::WIFI_NCP_FIRMWARE_VERSION);
#else
    return 0;
#endif // HAL_PLATFORM_NCP_COUNT > 1
}

int getWifiNcpFirmwareVersion(uint16_t* ncpVersion) {
    uint16_t version = 0;
#if HAL_PLATFORM_NCP_COUNT > 1
    using namespace particle::services;
    int res = SystemCache::instance().get(SystemCacheKey::WIFI_NCP_FIRMWARE_VERSION, &version, sizeof(version));
    if (res == sizeof(version)) {
        LOG(TRACE, "Cached ESP32 NCP firmware version: %d", (int)version);
        *ncpVersion = version;
        return 0;
    }

    if (res >= 0) {
        invalidateWifiNcpVersionCache();
    }
#endif // HAL_PLATFORM_NCP_COUNT > 1

    // Not present in cache or caching not supported, call into NCP client
    const auto ncpClient = particle::wifiNetworkManager()->ncpClient();
    SPARK_ASSERT(ncpClient);
    const particle::NcpClientLock lock(ncpClient);
    CHECK(ncpClient->on());
    CHECK(ncpClient->getFirmwareModuleVersion(&version));
    *ncpVersion = version;

    return 0;
}

} // anonymous

// FIXME: This function accesses the module info via XIP and may fail to parse it correctly under
// some not entirely clear circumstances. Disabling compiler optimizations helps to work around
// the problem
__attribute__((optimize("O0"))) int platform_ncp_update_module(const hal_module_t* module) {
    const auto ncpClient = particle::wifiNetworkManager()->ncpClient();
    SPARK_ASSERT(ncpClient);
    // Holding a lock for the whole duration of the operation, otherwise the netif may potentially poweroff the NCP
    const particle::NcpClientLock lock(ncpClient);
    CHECK(ncpClient->on());
    // we pass only the actual binary after the module info and up to the suffix
    const uint8_t* start = (const uint8_t*)module->info;
    static_assert(sizeof(module_info_t)==24, "expected module info size to be 24");
    start += sizeof(module_info_t); // skip the module info
    const uint8_t* end = start + (uint32_t(module->info->module_end_address) - uint32_t(module->info->module_start_address));
    const unsigned length = end-start;
    OtaUpdateSourceStream moduleStream(start, length);
    uint16_t version = 0;
    int r = ncpClient->getFirmwareModuleVersion(&version);
    if (r == 0) {
        LOG(INFO, "Updating ESP32 firmware from version %d to version %d", version, module->info->module_version);
    }
    invalidateWifiNcpVersionCache();
    r = ncpClient->updateFirmware(&moduleStream, length);
    LED_On(PARTICLE_LED_RGB);
    CHECK(r);
    r = ncpClient->getFirmwareModuleVersion(&version);
    if (r == 0) {
        LOG(INFO, "ESP32 firmware version updated to version %d", version);
    }
    return HAL_UPDATE_APPLIED;
}

int platform_ncp_fetch_module_info(hal_system_info_t* sys_info, bool create) {
    for (int i=0; i<sys_info->module_count; i++) {
        hal_module_t* module = sys_info->modules + i;
        if (!memcmp(&module->bounds, &module_ncp_mono, sizeof(module_ncp_mono))) {
            if (create) {
                uint16_t version = 0;
                // Defaults to zero in case of failure
                getWifiNcpFirmwareVersion(&version);

                // todo - we could augment the getFirmwareModuleVersion command to retrieve more details
                auto info = new module_info_t();
                CHECK_TRUE(info, SYSTEM_ERROR_NO_MEMORY);

                info->module_version = version;
                info->platform_id = PLATFORM_ID;
                info->module_function = MODULE_FUNCTION_NCP_FIRMWARE;

                // assume all checks pass since it was validated when being flashed to the NCP
                module->validity_checked = MODULE_VALIDATION_RANGE | MODULE_VALIDATION_DEPENDENCIES |
                        MODULE_VALIDATION_PLATFORM | MODULE_VALIDATION_INTEGRITY;
                module->validity_result = module->validity_checked;

                // IMPORTANT: a valid suffix with SHA is required for the communication layer to detect a change
                // in the SYSTEM DESCRIBE state and send a HELLO after the NCP update to
                // cause the DS to request new DESCRIBE info
                auto suffix = new module_info_suffix_t();
                if (!suffix) {
                    delete info;
                    return SYSTEM_ERROR_NO_MEMORY;
                }
                memset(suffix, 0, sizeof(module_info_suffix_t));

                // FIXME: NCP firmware should return some kind of a unique string/hash
                // For now we simply fill the SHA field with version
                for (uint16_t* sha = (uint16_t*)suffix->sha;
                        sha < (uint16_t*)(suffix->sha + sizeof(suffix->sha));
                        ++sha) {
                    *sha = version;
                }

                module->info = info;
                module->suffix = suffix;
                module->module_info_offset = 0;
            }
            else {
                delete module->info;
                delete ((module_info_suffix_t*)module->suffix);
            }
        }
    }
    return 0;
}

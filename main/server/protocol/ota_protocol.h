#pragma once

#include <stdint.h>
#include <stddef.h>

namespace ota {
namespace protocol {

/**
 * @brief Pack MQTT upgrade progress message
 * 
 * @param cmd_flag Command flag (0x0002 for MCU upgrade)
 * @param progress Upgrade progress (0-100)
 * @param hw_version Hardware version string
 * @param sw_version Software version string
 * @param upgrade_info Upgrade status information
 * @param out_buf Output buffer
 * @param buf_size Size of output buffer
 * @return size_t Length of packed message, 0 if failed
 */
size_t pack_mqtt_upgrade_progress(
    uint16_t cmd_flag,
    uint8_t progress,
    const char* hw_version,
    const char* sw_version,
    const char* upgrade_info,
    uint8_t* out_buf,
    size_t buf_size
);

} // namespace protocol
} // namespace ota 
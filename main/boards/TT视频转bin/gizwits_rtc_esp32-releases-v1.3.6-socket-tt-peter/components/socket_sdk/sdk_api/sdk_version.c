#include "sdk_version.h"
#include <string.h>

static char hardware_version[16] = "UNKNOWN0";
static char software_version[16] = "00000000";

void sdk_version_init(const char* hard_version, const char* soft_version) {
    if (hard_version) {
        strncpy(hardware_version, hard_version, sizeof(hardware_version) - 1);
        hardware_version[sizeof(hardware_version) - 1] = '\0';
    }
    
    if (soft_version) {
        strncpy(software_version, soft_version, sizeof(software_version) - 1);
        software_version[sizeof(software_version) - 1] = '\0';
    }
}

const char* sdk_version_get_hardware(void) {
    return hardware_version;
}

const char* sdk_version_get_software(void) {
    return software_version;
} 
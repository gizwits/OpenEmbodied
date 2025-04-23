#include "auth.h"

std::string Auth::getAuthKey() {
    return CONFIG_AUTH_KEY;
}

std::string Auth::getDeviceId() {
    return CONFIG_DEVICE_ID;
}

std::string Auth::getProductKey() {
    return CONFIG_PRODUCT_KEY;
}

std::string Auth::getProductSecret() {
    return CONFIG_PRODUCT_SECRET;
}

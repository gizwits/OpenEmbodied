#pragma once
#include <string>

class Auth {
public:
    static Auth& getInstance() {
        static Auth instance;
        return instance;
    }

    void init();
    std::string getAuthKey();
    std::string getDeviceId();
    std::string getProductKey();
    std::string getProductSecret();

private:
    Auth() = default;
    ~Auth() = default;
    Auth(const Auth&) = delete;
    Auth& operator=(const Auth&) = delete;

    std::string m_auth_key;
    std::string m_device_id;
    std::string m_product_key;
    std::string m_product_secret;
    bool m_is_initialized = false;
};

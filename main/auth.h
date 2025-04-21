#ifndef AUTH_H
#define AUTH_H

#include <string>

class Auth {
public:
    static std::string getAuthKey();
    static std::string getDeviceId();
    static std::string getProductKey();
    static std::string getProductSecret();
};

#endif // AUTH_H

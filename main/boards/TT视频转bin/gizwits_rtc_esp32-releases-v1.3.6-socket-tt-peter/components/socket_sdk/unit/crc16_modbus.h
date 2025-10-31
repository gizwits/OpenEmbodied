#ifndef __CRC16_MODBUS_H__
#define __CRC16_MODBUS_H__

#include <stdint.h>

#define crc16_modbus(data, length)  __crc16_modbus(__FUNCTION__, __LINE__, data, length)

uint16_t __crc16_modbus(const char * funName, int line, const uint8_t *data, size_t length);

#endif // __CRC16_H__

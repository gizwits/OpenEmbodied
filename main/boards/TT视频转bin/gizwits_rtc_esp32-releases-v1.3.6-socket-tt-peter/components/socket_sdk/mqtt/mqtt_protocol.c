#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>


/**
 * 将len按mqtt格式填充到buf中
 * @param len
 * @param buf
 * @return 填充数据的headerSize
 */
int8_t mqtt_len_of_bytes(int32_t len, uint8_t *buf) {
    int32_t datalen = len;
    int8_t headerSize = 0, digit = 0;
    do {
        digit = datalen % 128;
        datalen = datalen / 128;
        if (datalen > 0) {
            digit = digit | 0x80;
        }
        buf[headerSize++] = digit;
        // mqttPrintf( GAT_DEBUG,"%s buf[%d]=%02x\n",__FUNCTION__,headerSize-1,(uint8)buf[headerSize-1] );
    } while (datalen > 0);

    return headerSize;
}


uint8_t mqtt_num_rem_len_bytes(const uint8_t *buf) {
    uint8_t num_bytes = 0;
    uint32_t multiplier = 1;
    uint32_t value = 0;
    uint8_t encoded_byte;

    do {
        if (num_bytes >= 4) {
            // 超过最大字节数，返回错误
            return 0;
        }
        encoded_byte = buf[num_bytes++];
        value += (encoded_byte & 0x7F) * multiplier;
        multiplier *= 128;
    } while ((encoded_byte & 0x80) != 0);

    printf("%s Buffer contents[%d]: ",__func__, num_bytes);
    for (uint8_t i = 0; i < num_bytes; i++) {
        printf("%02x ", buf[i]);
    }
    printf("\n");
    printf("%s value:%u\n",__func__, value);
    return num_bytes;
}
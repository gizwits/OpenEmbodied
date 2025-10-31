#ifndef __SDK_VERSION_H__
#define __SDK_VERSION_H__

/* 初始化版本信息 */
void sdk_version_init(const char* hard_version, const char* soft_version);

/* 获取硬件版本号 */
const char* sdk_version_get_hardware(void);

/* 获取软件版本号 */
const char* sdk_version_get_software(void);

#endif 
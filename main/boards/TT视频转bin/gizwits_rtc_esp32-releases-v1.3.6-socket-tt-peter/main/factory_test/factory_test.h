#ifndef __FACTORY_TEST_H__
#define __FACTORY_TEST_H__

#include <stdint.h>
#include <stdbool.h>
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_FACTORY_TEST_MODE_ENABLE

#define FACTORY_TEST_SSID       "FACTORY_TEST"
#define FACTORY_TEST_PASSWORD   "12345678"

// 初始化工厂测试
void factory_test_init(void);
// 启动老化测试任务
void factory_start_aging_task(void);
// 启动LCD测试任务
void factory_start_lcd_task(void);
// 产测是否开启
bool factory_test_is_enabled(void);
bool factory_test_is_aging(void);
void handle_aging_test_mode(void);

#else

#define factory_test_init(mode)
#define factory_start_aging_task()
#define factory_test_is_enabled() false
#define factory_test_is_aging() false
#define handle_aging_test_mode() false


#endif

#ifdef __cplusplus
}
#endif

#endif /* __FACTORY_TEST_H__ */

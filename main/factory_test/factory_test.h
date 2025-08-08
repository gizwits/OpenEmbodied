#ifndef __FACTORY_TEST_HH__
#define __FACTORY_TEST_HH__

#include <cstdint>
#include <cstdbool>

// 产测模式
typedef enum {
    FACTORY_TEST_MODE_NONE = 0,
    FACTORY_TEST_MODE_IN_FACTORY,
    FACTORY_TEST_MODE_AGING,
} factory_test_mode_t;


// 启用工厂测试模式
#define CONFIG_FACTORY_TEST_MODE_ENABLE 1

#ifdef CONFIG_FACTORY_TEST_MODE_ENABLE

#define FACTORY_TEST_SSID       "FACTORY_TEST"
#define FACTORY_TEST_PASSWORD   "12345678"

// 初始化工厂测试
void factory_test_init(void);
// 启动老化测试任务
void factory_start_aging_task(void);

// 启动产测
void factory_test_start(void);

// 测试函数
void test_factory_test_uart(void);
void test_at_commands(void);
// 产测是否开启
bool factory_test_is_enabled(void);
bool factory_test_is_aging(void);

#else

#define factory_test_init(mode)
#define factory_start_aging_task()
#define factory_test_start()
#define factory_test_is_enabled() false
#define factory_test_is_aging() false

#endif

#endif /* __FACTORY_TEST_HH__ */ 
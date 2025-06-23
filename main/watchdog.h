#ifndef _WATCHDOG_H_
#define _WATCHDOG_H_

#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class Watchdog {
public:
    static Watchdog& GetInstance() {
        static Watchdog instance;
        return instance;
    }

    void Initialize(int timeout_seconds = 10, bool panic_on_timeout = true) {
        // esp_task_wdt_init(timeout_seconds, panic_on_timeout);
    }

    void SubscribeTask(TaskHandle_t task_handle) {
        esp_task_wdt_add(task_handle);
    }

    void UnsubscribeTask(TaskHandle_t task_handle) {
        esp_task_wdt_delete(task_handle);
    }

    void Reset() {
        esp_task_wdt_reset();
    }

private:
    Watchdog() = default;
    ~Watchdog() = default;
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
};

#endif // _WATCHDOG_H_ 
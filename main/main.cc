#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>

#include "application.h"
#include "system_info.h"
#include "watchdog.h"
#include "w25q64_flash.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 获取 Flash 单例实例并初始化
    // auto& flash = W25Q64Flash::GetInstance();
        
    // // 初始化 Flash
    // esp_err_t flash_ret = flash.Initialize(FLASH_PIN_MOSI, FLASH_PIN_MISO, 
    //                       FLASH_PIN_CLK, FLASH_PIN_CS, 10000);
    // if (flash_ret != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to initialize Flash: %s", esp_err_to_name(flash_ret));
    //     // return;
    // } else {
    //     ESP_LOGI(TAG, "Flash initialized successfully!");
    // }
    
    
    // // 运行自检
    // ESP_LOGI(TAG, "Running self test...");
    // if (!flash.SelfTest()) {
    //     ESP_LOGE(TAG, "Self test failed!");
    // } else {
    //     ESP_LOGI(TAG, "Self test passed!");
    // }

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
    auto& watchdog = Watchdog::GetInstance();
    watchdog.SubscribeTask(xTaskGetCurrentTaskHandle());
    app.MainEventLoop();
}

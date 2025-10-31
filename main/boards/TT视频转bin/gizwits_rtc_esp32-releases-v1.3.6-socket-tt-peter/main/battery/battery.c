#include "battery.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board/charge.h"
#include "config.h"
#include "battery.h"

static const char *TAG = "BATTERY";
static esp_adc_cal_characteristics_t adc_chars;
uint32_t average_voltage = 0;

uint32_t get_average_voltage(void)
{
    return average_voltage;
}

bool battery_is_usb_plugged(void)
{
    uint32_t voltage = 0;
    voltage = get_average_voltage() == 0 ? battery_get_voltage():get_average_voltage();
    return voltage >= BATTERY_FULL_VOLTAGE;
}

void battery_check_charging_100ms_callback(void)
{
    static uint8_t not_charging_count = 0;
    static bool is_charging = false;
    uint32_t voltage = battery_get_voltage();
    if (voltage > BATTERY_FULL_VOLTAGE) {
        if (!is_charging) {
            // 播放提示音
            audio_tone_play(0, 1, "spiffs://spiffs/T2_positive_feedback_96k.mp3");
            is_charging = true;
        }
        not_charging_count = 0; // 重置计数器
    } else if(voltage < BATTERY_NOT_CHARGING_VOLTAGE){
        if (is_charging) {
            not_charging_count++;
            if (not_charging_count >= 20) {
                is_charging = false;
                not_charging_count = 0;
            }
        }
    }
}

void battery_power_loss_check_100ms_callback(void)
{
    static uint32_t voltage_buffer[50] = {0};   // 50*100ms 5秒滤波
    static uint8_t index = 0;
    static bool buffer_filled = false;
    
    // 更新电压缓冲区
    voltage_buffer[index] = battery_get_voltage();
    index = (index + 1) % 50;
    
    // 检查缓冲区是否已填满
    if (index == 0) {
        buffer_filled = true;
    }

    if(buffer_filled)
    {
        // 计算平均电压
        uint32_t total_voltage = 0;
        for (uint8_t i = 0; i < 50; i++) {
            total_voltage += voltage_buffer[i];
        }
        average_voltage = total_voltage / 50;
        
        if (average_voltage < LOST_POWER_VOLTAGE) {
            ESP_LOGI(TAG, "average_voltage %d, gpio_set_power_status(0) \n", average_voltage);
            user_storage_save_power_loss_state(1);
            gpio_set_power_status(0);
        }
        // if(index%10 == 0)
        // {
        //     ESP_LOGI(TAG, "\t average_voltage: %d", average_voltage);
        // }
        if(index%50 == 0)
        {
            void dump_debug_info(void);
            // dump_debug_info();
        }
    }

    
}

static void battery_task(void *pvParameters)
{
    while (1) {
        #if 0
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // 构造二进制数据
        uint8_t binary_data[16] = {
            0x00, 0x00, 0x00, 0x03,  // 固定头部
            0x0a, 0x00, 0x00, 0x93,  // 命令标识
            0x00, 0x00, 0x00, 0x01,  // 数据长度
            0x14, 0x01,              // 数据类型
            0x00                     // 电池电量，将在下面更新
        };
        
        // 更新电池电量
        uint8_t level = battery_get_level();
        binary_data[14] = level;
        ESP_LOGI(TAG, "Battery ADC level:%d", level);
        
        
        ESP_LOGI(TAG, "Upload battery data: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                 binary_data[0], binary_data[1], binary_data[2], binary_data[3],
                 binary_data[4], binary_data[5], binary_data[6], binary_data[7],
                 binary_data[8], binary_data[9], binary_data[10], binary_data[11],
                 binary_data[12], binary_data[13], binary_data[14]);
        
        sdk_upload_p0_data((const char*)binary_data, sizeof(binary_data));
        #else
        // battery_get_level();
        vTaskDelay(pdMS_TO_TICKS(10000));
        battery_get_estimate(TYPE_AVERAGE);
        #endif

    }
    vTaskDelete(NULL);
}

void battery_init(void)
{
    // Configure ADC
    adc1_config_width(BATTERY_ADC_WIDTH);
    adc1_config_channel_atten(BATTERY_ADC_CHANNEL, BATTERY_ADC_ATTEN);
    
    // Characterize ADC
    esp_adc_cal_characterize(ADC_UNIT_1, BATTERY_ADC_ATTEN, BATTERY_ADC_WIDTH, 1100, &adc_chars);
    
    ESP_LOGI(TAG, "Battery ADC initialized on IO6");
    // xTaskCreate(battery_task, "battery_task", 2048 * 2, NULL, 5, NULL);
}

uint32_t battery_get_voltage(void)
{
    // return 3439;

    // Read ADC value
    uint32_t adc_reading = adc1_get_raw(BATTERY_ADC_CHANNEL);
    
    // Convert ADC reading to voltage (mV)
    uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, &adc_chars);
    
    // Since we're using a voltage divider, multiply by the divider ratio
    // Assuming a 2:1 voltage divider (R1 = R2)
    voltage *= 2;

    voltage += BATTERY_VOLTAGE_COMPENSATION; // 新版补偿二极管器件压降

    // INSERT_YOUR_CODE
    static uint32_t last_log_time = 0;
    uint32_t current_time = esp_log_timestamp();

    if (current_time - last_log_time > 1900) // 超过2秒才打印
    {
        ESP_LOGI(TAG, "Battery ADC voltage:%ld, average_voltage: %d", voltage, average_voltage);
        last_log_time = current_time;
    }

    return voltage;
}

uint8_t battery_get_level(void)
{
    uint32_t voltage = battery_get_voltage();
    uint8_t  estimate = 0;

    // Calculate battery level percentage
    if (voltage >= BATTERY_FULL_VOLTAGE)
    {
        estimate = 100;
    }
    else if (voltage <= BATTERY_LOW_VOLTAGE)
    {
        estimate = 0;
    }
    else
    {
        estimate = ((voltage - BATTERY_LOW_VOLTAGE) * 100) /
               (BATTERY_FULL_VOLTAGE - BATTERY_LOW_VOLTAGE);
    }
    ESP_LOGI(TAG, "Battery level:%d", estimate);
    return estimate;

}

// Define the relationship between voltage and state of charge
typedef struct
{
    uint16_t adcValue; // ADC value
    uint8_t soc;       // State of Charge (percentage of battery capacity)
} VoltageSocPair;

// Array of voltage and state of charge relationships
// Discharge curve
#if defined(CONFIG_TT_MUSIC_HW_1V5)
#pragma message("BATTERY_VERSION is BATTERY_1_5")
const VoltageSocPair dischargeCurve[] = {
    {4140, 100}, // 100%
    {4104, 95},  // 下降36mV
    {4068, 90},  // 下降36mV
    {4032, 85},  // 下降36mV
    {3996, 80},  // 下降36mV
    {3960, 75},  // 下降36mV
    {3924, 70},  // 下降36mV
    {3888, 65},  // 下降36mV
    {3852, 60},  // 下降36mV
    {3829, 55},  // 下降23mV（过渡段开始）
    {3808, 50},  // 下降21mV
    {3787, 45},  // 下降21mV
    {3766, 40},  // 下降21mV
    {3745, 35},  // 下降21mV
    {3724, 30},  // 下降21mV
    {3703, 25},  // 下降21mV
    {3672, 20},  // 下降31mV（低电量段开始）
    {3570, 15},  // 下降102mV
    {3420, 10},  // 下降150mV
    {3220, 5},   // 下降200mV
    {3000, 0}    // 下降220mV
};
#elif defined(CONFIG_TT_MUSIC_HW_1V3)
#pragma message("BATTERY_VERSION is BATTERY_1_3")
const VoltageSocPair dischargeCurve[] = {
    {4140, 100}, {4104, 95}, {4068, 90}, {4032, 85}, {3996, 80}, {3960, 75}, {3924, 70}, {3888, 65}, {3852, 60}, {3829, 55},
    {3808, 50}, {3787, 45}, {3766, 40}, {3745, 35}, {3724, 30}, {3703, 25}, {3672, 20}, {3631, 15}, {3600, 10}, {3580, 5}, {3570, 0}
};
#else 
#pragma error("CONFIG_TT_MUSIC_HW_1V3 CONFIG_TT_MUSIC_HW_1V5 is not defined")
#endif
// 查表，根据电压估算电量
static uint8_t estimate_soc(uint16_t voltage, const VoltageSocPair *soc_pairs, int voltage_soc_pairs_length)
{
    if (soc_pairs == NULL)
    {
        return 0;
    }
    uint16_t closest_voltage = soc_pairs[0].adcValue; // ADC value
    uint8_t closest_soc = soc_pairs[0].soc;          // State of Charge (percentage of battery capacity)
    uint16_t min_diff = abs(voltage - closest_voltage);

    // 遍历数组，找到最接近的电压值
    for (int i = 1; i < voltage_soc_pairs_length; i++)
    {
        uint16_t diff = abs(voltage - soc_pairs[i].adcValue);
        if (diff < min_diff)
        {
            min_diff = diff;
            closest_voltage = soc_pairs[i].adcValue;
            closest_soc = soc_pairs[i].soc;
        }
    }
    return closest_soc;
}

// 根据ADC值和SOC对应关系数组估算电池电量百分比
static uint8_t estimate_battery_level(uint16_t adcValue, const VoltageSocPair *soc_pairs, int voltage_soc_pairs_length)
{
    static uint16_t lastAdcValue = 0;
    // ESP_LOGI(TAG, "adcValue:%d,lastAdcValue:%d", adcValue, lastAdcValue);

    // 通过电压判断设备是否在充电
    if (adcValue > USB_CHRG_VOLTAGE)
    {
        lastAdcValue = USB_CHRG_VOLTAGE;
    }

    // 更新上一次ADC读数，只保留降低的ADC值
    if (adcValue < lastAdcValue || lastAdcValue == 0) {
        lastAdcValue = adcValue;
    }

    // 根据上一次ADC读数估算电池电量百分比
    uint8_t battery_percentage = estimate_soc(lastAdcValue, soc_pairs, voltage_soc_pairs_length);
    // ESP_LOGI(TAG, "adcValue:%d,lastAdcValue:%d,estimate_soc:%d", adcValue, lastAdcValue, battery_percentage);
    return battery_percentage;
}

uint8_t battery_get_estimate(uint8_t type)
{
    uint8_t estimate = 0;
    uint32_t voltage = 0;
    
    if(type == TYPE_INSTANT)
    {
        voltage = battery_get_voltage();
    }
    else if(type == TYPE_AVERAGE)
    {
        voltage = get_average_voltage() == 0 ? battery_get_voltage():get_average_voltage();
    }
    
    // 瞬间电压达到充满电时 直接返回100
    if(battery_get_voltage() > BATTERY_FULL_VOLTAGE)
    {
        return 100;
    }
    
    // INSERT_YOUR_CODE

    estimate = estimate_battery_level(voltage, dischargeCurve, sizeof(dischargeCurve) / sizeof(dischargeCurve[0]));
    if(estimate != 100)
    {
        ESP_LOGI(TAG, "Battery estimate:%d, %s", estimate, get_battery_state_str());
    }
    return estimate;
}
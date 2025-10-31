#ifndef __BATTERY_H__
#define __BATTERY_H__

#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "audio_processor.h"
#include "config.h"

// #define BATTERY_VOLTAGE_COMPENSATION 300
#define BATTERY_VOLTAGE_COMPENSATION 50  // 电池电量补偿

// Battery voltage constants
#define BATTERY_ADC_CHANNEL ADC1_CHANNEL_5  // IO6 corresponds to ADC1_CHANNEL_5
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_11   // 11dB attenuation for 0-3.3V range
#define BATTERY_ADC_WIDTH ADC_WIDTH_BIT_12  // 12-bit ADC resolution

// Battery voltage thresholds (in mV)
#if defined(CONFIG_TT_MUSIC_HW_1V3)
// <= TT V1.3
#pragma message("BATTERY_VERSION is CONFIG_TT_MUSIC_HW_1V3")
#define BATTERY_FULL_VOLTAGE 4300
#define BATTERY_NOT_CHARGING_VOLTAGE 4200
#define BATTERY_LOW_VOLTAGE 3570
#define USB_CHRG_VOLTAGE 4300
#define LOST_POWER_VOLTAGE 3580
#elif defined(CONFIG_TT_MUSIC_HW_1V5)
// TT V1.5
#pragma message("BATTERY_VERSION is CONFIG_TT_MUSIC_HW_1V5")
#define BATTERY_FULL_VOLTAGE 4300
#define BATTERY_NOT_CHARGING_VOLTAGE 4200
#define BATTERY_LOW_VOLTAGE 3250
#define USB_CHRG_VOLTAGE 4300
#define LOST_POWER_VOLTAGE 3000
#else
#error "Invalid TT Music Hardware Version"
#endif

#define RED_ON_PERCENTAGE    20
#define RED_OFF_PERCENTAGE   25

// Function declarations
void battery_init(void);
uint32_t battery_get_voltage(void);
uint8_t battery_get_level(void);

#define TYPE_INSTANT 0
#define TYPE_AVERAGE 1
uint8_t battery_get_estimate(uint8_t type);
bool battery_is_usb_plugged(void);


#endif /* __BATTERY_H__ */

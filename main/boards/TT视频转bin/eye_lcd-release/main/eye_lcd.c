/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <stdint.h>
#include "esp_err.h"
#include <string.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
// #include "catEye.h"
#include "esp_lcd_gc9d01n.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_partition.h"
#include "eye_lcd.h"

// Using SPI2 in the example, as it also supports octal modes on some targets
#define LCD_HOST       SPI2_HOST
// To speed up transfers, every SPI transfer sends a bunch of lines. This define specifies how many.
// More means more memory use, but less overhead for setting up / finishing transfers. Make sure 240
// is dividable by this.
#define PARALLEL_LINES 16
// The number of frames to show before rotate the graph
#define EXAMPLE_LCD_H_RES   160
#define EXAMPLE_LCD_V_RES   160

#define DISPLAY_MIRROR_X 0
#define DISPLAY_MIRROR_Y 0
#define DISPLAY_SWAP_XY 0
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL  1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
// FSPI
#define EXAMPLE_PIN_NUM_DATA0          8 //41  /*!< for 1-line SPI, this also refereed as MOSI */
#define EXAMPLE_PIN_NUM_PCLK           7 // 42
#define EXAMPLE_PIN_NUM_CS             6 // 2
// #define EXAMPLE_PIN_NUM_DATA0          41  /*!< for 1-line SPI, this also refereed as MOSI */
// #define EXAMPLE_PIN_NUM_PCLK           42
// #define EXAMPLE_PIN_NUM_CS             2
#define EXAMPLE_PIN_NUM_DC             5
#define EXAMPLE_PIN_NUM_RST            10
#define EXAMPLE_PIN_NUM_BK_LIGHT       4

// Bit number used to represent command and parameter
#define EXAMPLE_LCD_CMD_BITS           8
#define EXAMPLE_LCD_PARAM_BITS         8


// 定义LCD面板句柄
static esp_lcd_panel_handle_t panel_handle = NULL;

uint8_t last_video = 0;

void set_last_video(uint8_t video)
{
    last_video = video;
}
uint8_t get_last_video(void)
{
    return last_video;
}

static uint16_t factory_color = FACTORY_COLOR_RED;
void set_factory_color(uint16_t color)
{
    factory_color = color;
}

// 产测阻塞处理
uint16_t get_factory_auto_color(void);
// 绘制眼球图像
void render_eye(esp_lcd_panel_handle_t panel_handle);
// 绘制眼球图像 先用 1 后面的判断逻辑不正确
static uint16_t play_index = 1;
static int direction = 1; // 1表示递增,-1表示递减
static video_type_t current_video = VIDEO_WAKEUP; // 默认播放待机动画
static int8_t next_video = -1; // 默认播放待机动画

void set_next_video(int8_t video_type)
{
    next_video = video_type;
}
int8_t get_next_video(void)
{
    return next_video;
}
void set_video(video_type_t video_type)
{
    if (current_video == VIDEO_WAKEUP) {
        // VIDEO_WAKEUP 不可打断
        set_next_video(video_type);
        return;
    }
    else {
        set_next_video(-1);
        current_video = video_type;
    }
}

void set_direction(int dir)
{
    direction = dir;
}
void reverse_direction()
{
    if(direction == 1)
    {
        direction = -1;
    }
    else
    {
        direction = 1;
    }
}

void display_black_screen(void)
{
    // 使用静态数组来存储黑色帧数据
    static uint16_t black_buffer[160 * 160];
    memset(black_buffer, 0, sizeof(black_buffer));
    
    // 分块显示黑色，避免一次性传输过多数据
    for(int y = 0; y < 160; y += 16) {
        int height = (y + 16 > 160) ? (160 - y) : 16;
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, 160, y + height, black_buffer + y * 160);
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
    
    // 延迟一小段时间确保黑色显示完成
    vTaskDelay(100 / portTICK_PERIOD_MS);
}

void turn_off_screen(void)
{
    // 关闭背光
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL));
    // 关闭显示
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, false));
    // 设置当前视频为关闭状态
    current_video = VIDEO_OFF;
}

void _turn_on_screen(void)
{
     // 显示黑色屏幕
    display_black_screen();
    // 打开背光
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL));
    // 打开显示
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    vTaskDelete(NULL);
}
void turn_on_screen(void)
{
    xTaskCreate(_turn_on_screen, "_turn_on_screen", 2048, NULL, 10, NULL);
}



uint8_t video_index = 1;

void render_eye(esp_lcd_panel_handle_t panel_handle) {
    static uint32_t index_max = 0;
    static int count = 0;     // 计数器,记录当前方向连续移动次数

    // 打开storage分区
    const esp_partition_t *storage_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x81, "storage");
    if (storage_partition == NULL) {
        ESP_LOGE("LCD", "can't find storage partition");
        return;
    }
    // 定义视频信息结构体
    typedef struct {
        uint8_t video_id;
        uint32_t frame_count;
    } video_info_t;
    
    static uint8_t video_count = 0;
    static video_info_t *video_infos = NULL;
    
    if (video_infos == NULL) {
        // 读取视频组数
        esp_partition_read(storage_partition, 0, &video_count, 1);
        ESP_LOGI("LCD", "video_count: %d", video_count);
        
        // 为视频信息分配内存
        video_infos = malloc(video_count * sizeof(video_info_t));
        
        // 读取每组视频的帧数
        uint32_t *frames = malloc(video_count * sizeof(uint32_t));
        esp_partition_read(storage_partition, 1, frames, video_count * 4);
        
        // 初始化视频信息结构体数组
        for(int i = 0; i < video_count; i++) {
            video_infos[i].video_id = i;
            video_infos[i].frame_count = frames[i];
            ESP_LOGI("LCD", "video_infos[%d]: id=%d, frames=%ld", i, video_infos[i].video_id, video_infos[i].frame_count);
        }
        free(frames);
        
        // 默认使用第一组视频
        index_max = video_infos[current_video].frame_count;
        // ESP_LOGI("LCD", "video_infos[%d] index_max: %ld", current_video, index_max);
    }

    // 0 思考 1 说话 2 聆听
    static uint16_t read_frame_data[160 * 160];

    // 计算当前视频的帧偏移
    uint32_t video_offset = 0;
    uint32_t total_frames = 0;
        // 使用读取的数据进行绘制
    int polar_x_start = 0;
    int polar_y_start = 0;
    int polar_x_end = polar_x_start + 160;
    int polar_y_end = polar_y_start + 160;

    if(last_video != current_video || video_index == 0xff) {
        // 根据视频类型选择对应的video_infos下标
        switch(current_video) {
            case VIDEO_REPLY:
                video_index = 2;
                break;
            case VIDEO_WAKEUP:
                video_index = 1;
                if (get_last_video() != VIDEO_WAKEUP) {
                    play_index = 1;
                }
                break;
            case VIDEO_LISTEN:
                video_index = 0;
                if (get_last_video() == VIDEO_REPLY) {
                    play_index = 1;
                }
                break;
            case VIDEO_THINK:
                break;
            case VIDEO_OFF:
                video_index = 0;
                break;
            case VIDEO_FACTORY_TEST:
                video_index = 0;
                break;
            default:
                break;
        }

        set_last_video(current_video);
    }
    if( video_index != 0xff)
    {
        // 打开背光
        ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL));
        // 计算帧偏移
        for(int i = 0; i < video_index; i++) {
            total_frames += video_infos[i].frame_count;
            video_offset += video_infos[i].frame_count * 160 * 160 * sizeof(uint16_t);
        }
        // ESP_LOGI("LCD", "video_index=%d, total_frames=%ld, video_offset=%ld", video_index, total_frames, video_offset);
        size_t frame_offset = 1 + 4 * video_count + (total_frames + play_index) * 160 * 160 * sizeof(uint16_t);
        // ESP_LOGI("LCD", "frame_offset=%d", frame_offset);
        esp_partition_read(storage_partition, frame_offset, read_frame_data, 160 * 160 * sizeof(uint16_t));
    }
    else if(current_video != VIDEO_OFF)
    {
        memset(read_frame_data, 0, 160 * 160 * sizeof(uint16_t));
    }

    if (current_video == VIDEO_FACTORY_TEST) {
        uint16_t color = get_factory_auto_color();
        for (int i = 0; i < 160 * 160; i++) {
            ((uint16_t *)read_frame_data)[i] = color;
        }
        // 使用读取的数据进行绘制
        esp_lcd_panel_draw_bitmap(panel_handle, polar_x_start, polar_y_start, polar_x_end, polar_y_end, read_frame_data);
        return;
    }

    if (current_video == VIDEO_SET_COLOR) {
        for (int i = 0; i < 160 * 160; i++) {
            ((uint16_t *)read_frame_data)[i] = factory_color;
        }
        esp_lcd_panel_draw_bitmap(panel_handle, polar_x_start, polar_y_start, polar_x_end, polar_y_end, read_frame_data);
        return;
    }

    // ESP_LOGI("LCD", "draw_bitmap");
    esp_lcd_panel_draw_bitmap(panel_handle, polar_x_start, polar_y_start, polar_x_end, polar_y_end, read_frame_data);


    if(current_video != VIDEO_OFF)
    {
        // ESP_LOGI("LCD", "video_index=%d, video_infos[video_index].frame_count=%ld",  video_index, video_infos[video_index].frame_count);
        if (current_video == VIDEO_WAKEUP) {
            if(play_index >= video_infos[video_index].frame_count - 1) {
                ESP_LOGI("LCD", "RUN_LISTEN %d, %ld", play_index, video_infos[video_index].frame_count);
                if(get_next_video() != -1) {
                    current_video = get_next_video();
                }
                else {
                    current_video = VIDEO_LISTEN;
                }
            }
        }
        // 循环播放策略
        if(current_video == VIDEO_THINK)
        {
            direction = -1;
        }
        else if(current_video != VIDEO_OFF)
        {
            direction = 1;
        }
        if(play_index >= video_infos[video_index].frame_count - 1)
        {
            play_index = 0;
        }
        else if(play_index <= 0)
        {
            play_index = video_infos[video_index].frame_count - 1;
        }

        play_index += direction;

    }

    const char* video_type_name[] = {
        "VIDEO_THINK",
        "VIDEO_REPLY",
        "VIDEO_WAKEUP",
        "VIDEO_LISTEN",
        "VIDEO_OFF",
        "VIDEO_FACTORY_TEST",
        "VIDEO_SET_COLOR"
    };
    // Print current frame play_index
    if(play_index %10 == 0)
    {
        ESP_LOGI("LCD", " %s Current frame play_index: %d", video_type_name[current_video], play_index);
    }
    // ESP_LOGI("LCD", "Current frame play_index: %d", play_index);
// 定义视频序号

}

uint16_t get_factory_auto_color(void)
{
    // 增加产测逻辑
    static uint16_t color = 0x00F8;
    static uint8_t color_index = 0;
    static TickType_t last_tick = 0;
    TickType_t current_tick = xTaskGetTickCount();
    if(last_tick == 0) {
        last_tick = current_tick;
    }
    // while (current_video == VIDEO_FACTORY_TEST) {
        current_tick = xTaskGetTickCount();
        // INSERT_YOUR_CODE
        if ((current_tick - last_tick) * portTICK_PERIOD_MS >= 1000) { // 每秒切换颜色
            last_tick = current_tick;
            color_index = (color_index + 1) % 4; // 循环切换颜色

            switch (color_index) {
                case 0:
                    color = FACTORY_COLOR_RED; // 红色 小端在前
                    break;
                case 1:
                    color = FACTORY_COLOR_GREEN; // 绿色
                    break;
                case 2:
                    color = FACTORY_COLOR_BLUE; // 蓝色
                    break;
                case 3:
                    color = FACTORY_COLOR_YELLOW; // 黄色
                    break;
            }
            ESP_LOGI("LCD", "color_index %d, color 0x%x", color_index, color);

            // 填充颜色到读取的数据

        // }
        // ESP_LOGI("LCD", "Current tick: %lu, Last tick: %lu", current_tick, last_tick);
    }
    return color;
}

static void eye_lcd_app_main(void *arg)
{
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_BK_LIGHT
    };
    // Initialize the GPIO of backlight
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    spi_bus_config_t buscfg = {
        .sclk_io_num = EXAMPLE_PIN_NUM_PCLK,
        .mosi_io_num = EXAMPLE_PIN_NUM_DATA0,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EXAMPLE_LCD_V_RES * EXAMPLE_LCD_H_RES * 2 + 8
    };
    // Initialize the SPI bus
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = EXAMPLE_PIN_NUM_DC,
        .cs_gpio_num = EXAMPLE_PIN_NUM_CS,
        .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = EXAMPLE_LCD_CMD_BITS,
        .lcd_param_bits = EXAMPLE_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };

    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    // Initialize the LCD configuration
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01n(io_handle, &panel_config, &panel_handle));

    // 初始化时关闭背光
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL));

    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_invert_color(panel_handle, false);
    esp_lcd_panel_swap_xy(panel_handle, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel_handle, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    display_black_screen();
    // 延迟一小段时间确保黑色显示完成
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    // 然后打开背光
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL));

    // render_eye(panel_handle);
    while (1)
    {
        static uint16_t time = 33;
        // if(play_index == 11)
        // {
        //     time += 100;
        //     time %= 501;
        // }
        // /* code */
        render_eye(panel_handle);
        vTaskDelay(time/portTICK_PERIOD_MS);
    }
}


void app_main(void)
{

    void uart_echo_app_main(void);
    uart_echo_app_main();

    xTaskCreate(eye_lcd_app_main, "eye_lcd_task", 10240, NULL, 10, NULL);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    set_video(VIDEO_WAKEUP);

    vTaskDelete(NULL);

}
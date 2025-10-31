#ifndef __EYE_LCD_H__
#define __EYE_LCD_H__

// 定义视频序号
typedef enum {
    VIDEO_THINK = 0,
    VIDEO_REPLY = 1,
    VIDEO_WAKEUP = 2,
    VIDEO_LISTEN = 3,
    VIDEO_OFF = 4,
    VIDEO_FACTORY_TEST = 5,
    VIDEO_SET_COLOR = 6
} video_type_t;

#define FACTORY_COLOR_RED       0x00F8
#define FACTORY_COLOR_GREEN     0xE007
#define FACTORY_COLOR_BLUE      0x1F00
#define FACTORY_COLOR_YELLOW    0x07FF

void set_video(video_type_t video_type);
void set_direction(int dir);
void reverse_direction();
void turn_off_screen(void);
void turn_on_screen(void);
void set_factory_color(uint16_t color);

#endif

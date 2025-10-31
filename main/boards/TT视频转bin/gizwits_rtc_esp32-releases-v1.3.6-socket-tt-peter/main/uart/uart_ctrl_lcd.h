#ifndef __UART_CTRL_LCD_H__
#define __UART_CTRL_LCD_H__

#define STR_EVENT_LEFT   "left"
#define STR_EVENT_RIGHT  "right"
#define STR_EVENT_REVERSE "reverse"
#define STR_EVENT_REPLY  "reply"
#define STR_EVENT_THINK  "think"
#define STR_EVENT_WAKEUP "wakeup"
#define STR_EVENT_LISTEN "listen"
#define STR_EVENT_OFF    "off"
#define STR_EVENT_FACTORY_TEST    "factory_test"
#define STR_EVENT_RED    "red"
#define STR_EVENT_GREEN  "green"
#define STR_EVENT_BLUE   "blue"
#define STR_EVENT_YELLOW "yellow"
#define STR_EVENT_INIT   "wakeup"


typedef enum {
    EVENT_REPLY,
    EVENT_THINK,
    EVENT_WAKEUP,
    EVENT_LISTEN,
    EVENT_OFF,
    EVENT_FACTORY_TEST,
    EVENT_RED,
    EVENT_GREEN,
    EVENT_BLUE,
    EVENT_YELLOW,
    EVENT_INIT,
    EVENT_LIST_SIZE,
} event_type_t;

extern const char *EVENT_LIST[];

#define lcd_state_event_send(event) __lcd_state_event_send(__func__, __LINE__, event)
void __lcd_state_event_send(const char * func, uint32_t line, event_type_t event);
void uart_ctrl_lcd_task_init(void);

#endif

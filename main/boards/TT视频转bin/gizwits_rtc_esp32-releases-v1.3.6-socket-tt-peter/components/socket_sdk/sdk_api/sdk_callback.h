#ifndef __SDK_CALLBACK_H__
#define __SDK_CALLBACK_H__

#include "sdk_api.h"

/* Get callback functions */
coze_plugin_notify_cb get_coze_plugin_notify_callback(void);
user_event_notify_cb get_user_event_notify_callback(void);

/* Set callback functions */
void set_coze_plugin_notify_callback(coze_plugin_notify_cb cb);
void set_user_event_notify_callback(user_event_notify_cb cb);

/* Notify functions */
void user_event_notify(user_event_t event);
void user_event_notify_with_json(user_event_t event, cJSON *json_data);

#endif 
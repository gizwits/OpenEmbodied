#include "sdk_callback.h"

static coze_plugin_notify_cb g_coze_plugin_notify_cb = NULL;
static user_event_notify_cb g_user_event_notify_cb = NULL;

coze_plugin_notify_cb get_coze_plugin_notify_callback(void) {
    return g_coze_plugin_notify_cb;
}

user_event_notify_cb get_user_event_notify_callback(void) {
    return g_user_event_notify_cb;
}

void set_coze_plugin_notify_callback(coze_plugin_notify_cb cb) {
    g_coze_plugin_notify_cb = cb;
}

void set_user_event_notify_callback(user_event_notify_cb cb) {
    g_user_event_notify_cb = cb;
} 


void user_event_notify(user_event_t event) {
    user_event_notify_cb cb = get_user_event_notify_callback();
    if (cb) {
        cb(event, NULL);
    }
}

void user_event_notify_with_json(user_event_t event, cJSON *json_data) {
    user_event_notify_cb cb = get_user_event_notify_callback();
    if (cb) {
        cb(event, json_data);
    }
}

void plugin_notify(char *data) {
    coze_plugin_notify_cb cb = get_coze_plugin_notify_callback();
    if (cb) {
        cb(data);
    }
}
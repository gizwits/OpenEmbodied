change file list:
adf:
/root/giz_adf/esp-adf/components/audio_pipeline/include/audio_event_iface.h
/root/giz_adf/esp-adf/components/audio_pipeline/audio_element.c
idf:
/root/giz_adf/esp-adf/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c


改动原因:
0. \\wsl.localhost\Ubuntu\root\gizwits_rtc_esp32\components\audio_processor\audio_processor.c的url_play
无法播放第二次url,且管道状态播放中与停止播放会有异常
1. 收到第3条url推送时, 会报队列assert, 暂屏蔽assert, 与调整 audio_event_iface.h队列深度
        // todo mark Peter
        // configASSERT( pxQueueSetContainer->uxMessagesWaiting < pxQueueSetContainer->uxLength );

2. 双管道配置如下
[1] spiff stream + mp3 decoder + i2s[常态存在]
[2] http stream + mp3 decoder + i2s [动态管理,播完1条url删除自身]

乐鑫SDK管道事件推送可能会直推给[1]管道,需要在 audio_element的done或者finish的事件里上报状态到[2]管道用于自删

/root/giz_adf/esp-adf/esp-idf/components/spi_flash/cache_utils.c



/root/giz_adf/esp-adf/esp-idf/components/tcp_transport/transport_ws.c


esp-adf/components/esp-sr/Kconfig.projbuild
esp-adf/components/esp-sr/model/wakenet_model/wn9_hey_jackson

/root/giz_adf/esp-adf/esp-idf/components/log/include/esp_log.h
/root/giz_adf/esp-adf/components/audio_pipeline/audio_event_iface.c

/root/giz_adf/esp-adf/components/audio_recorder/audio_recorder.c
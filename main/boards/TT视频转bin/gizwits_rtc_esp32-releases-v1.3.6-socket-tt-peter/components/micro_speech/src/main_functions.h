/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_LITE_MICRO_EXAMPLES_MICRO_SPEECH_MAIN_FUNCTIONS_H_
#define TENSORFLOW_LITE_MICRO_EXAMPLES_MICRO_SPEECH_MAIN_FUNCTIONS_H_

// Expose a C interface for main functions.
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*speech_recognition_callback)(const char* command, float score);

// Initializes all data needed for the example. The name is important, and needs
// to be setup() for Arduino compatibility.
void micro_speech_setup(speech_recognition_callback callback);

// Runs one iteration of data gathering and inference. This should be called
// repeatedly from the application code. The name needs to be loop() for Arduino
// compatibility.
void micro_speech_loop();

// 启动语音识别
void micro_speech_start();
void micro_speech_destory();

// 停止语音识别
void micro_speech_stop();


#ifdef __cplusplus
}
#endif

#endif  // TENSORFLOW_LITE_MICRO_EXAMPLES_MICRO_SPEECH_MAIN_FUNCTIONS_H_

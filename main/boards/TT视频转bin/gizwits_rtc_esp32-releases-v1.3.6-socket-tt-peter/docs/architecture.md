### Gizwits\_RTC Software Architecture for the Device

#### Major Functions of the Device:

- **LCD Display**
- **Speaker**
- **Dual Mics (or 3\~4 mics for some models)**
- **Wi-Fi Network**:
  - Connect to an MQTT service
  - Connect to a remote RTC service
- **BLE Connection**:
  - Used during on-boarding (send Wi-Fi SSID/Password from mobile phone to device)
  - Occasionally used for OTA updates

### 1. Proposed Tasks

#### **1.1 UI / LCD Display Task**

- **Purpose**: Update and refresh the LCD with user-facing information.
- **Why a Dedicated Task**:
  - LCD updates may need to happen at a moderate rate (e.g., 30–60 Hz for a fluid display, or less for static text).
  - Allows the system to queue or signal events for screen updates without blocking other tasks.
- **Key Responsibilities**:
  - Render or refresh the display periodically or upon state changes.
  - Handle any UI input events (if a touchscreen or keypad exists).

---

#### **1.2 Audio Playback / Speaker Task**

- **Purpose**: Manage audio data playback to the speaker.
- **Why a Dedicated Task**:
  - Audio playback involves timing-sensitive operations (DAC output, streaming buffers).
  - Helps keep the main loop responsive and prevents blocking other tasks.
- **Key Responsibilities**:
  - Receive audio data (from local files, RTC streams, etc.).
  - Buffer and play the audio.

---

#### **1.3 Microphone Processing / Audio Capture Task**

- **Purpose**: Capture and optionally preprocess audio from multiple mics (2–4).
- **Why a Dedicated Task**:
  - Real-time constraints for voice detection, noise suppression, etc.
  - Higher priority required if real-time streaming or offline SR is involved.
- **Key Responsibilities**:
  - Sample data from the mics.
  - Apply audio algorithms (AEC, beamforming, noise suppression).
  - Pass processed audio onward (e.g., to RTC or local SR engine).

---

#### **1.4 MQTT Client Task**

- **Purpose**: Maintain an MQTT connection for sending/receiving messages.
- **Why a Dedicated Task**:
  - MQTT libraries (like Eclipse Paho or others) are often event-driven but may include blocking operations.
  - A dedicated task ensures that MQTT operations do not block other parts of the system.
- **Key Responsibilities**:
  - Connect/disconnect from broker (reconnect if needed).
  - Publish device states or sensor data.
  - Receive commands from the cloud and dispatch events internally.

---

#### **1.5 RTC (Real-Time Communication) Task**

- **Purpose**: Manage communication with the remote RTC service (e.g., voice/video calls).
- **Why a Dedicated Task**:
  - Typically requires real-time audio/video streaming.
  - Must handle network I/O, buffering, and sending/receiving audio frames at low latency.
- **Key Responsibilities**:
  - Maintain RTC connection state.
  - Send captured mic data and receive remote audio data for playback.
  - Handle call signaling if applicable.

---

#### **1.6 BLE Task**

- **Purpose**: Handle BLE for on-boarding (sending Wi-Fi credentials to the device) and occasional OTA updates.
- **Why a Dedicated Task** (though it can be made dynamic):
  - BLE interaction is usually event-driven, but on-boarding might involve a conversation-like flow (request, response).
  - Tasks can be suspended or deleted when BLE is off, freeing resources.
- **Key Responsibilities**:
  - Handle pairing, service discovery, and data transfer.
  - Only run BLE if on-boarding or OTA is active; otherwise turn off radio to save power.
  - Potentially handle file transfer for OTA images.

---

#### **1.7 System Controller (Main / Supervisory) Task**

- **Purpose**: Oversee the system as a whole (power states, error conditions, orchestrating tasks).
- **Why a Dedicated Task**:
  - Coordinates interaction between tasks, manages global states (e.g., device mode, error recovery).
  - Collects signals/events to trigger state changes in an orderly fashion.
- **Key Responsibilities**:
  - Monitor device health.
  - Decide when to power down certain peripherals (like BLE) or change system modes (normal, low-power, etc.).
  - Handle transitions between functional states (e.g., start RTC, stop BLE, etc.).

---

### 2. Features That Can Use EventGroups Instead of Dedicated Tasks

**EventGroups** in FreeRTOS (or similar RTOS) are used to **synchronize or signal state changes** between tasks. They do not require a separate thread/task—rather, they are polled or waited on by existing tasks. Here are some candidate use cases:

#### **2.1 Wi-Fi Connectivity States**

- Events could be `WIFI_CONNECTED`, `WIFI_DISCONNECTED`, `WIFI_GOT_IP`, etc.
- Tasks like MQTT or RTC can wait on these bits before attempting network operations.

#### **2.2 BLE On-boarding Progress**

- Instead of a dedicated “state machine” task solely for on-boarding states, you can use event bits such as `BLE_CONNECTED`, `BLE_CREDENTIALS_RECEIVED`, `BLE_COMPLETED`.
- The System Controller Task (or BLE Task) can set/clear these events; other tasks can wait for them if needed.

#### **2.3 OTA State**

- Events like `OTA_START`, `OTA_IN_PROGRESS`, `OTA_SUCCESS`, `OTA_FAIL`.
- Helps coordinate safe shutting down of non-essential services during OTA or ensuring certain tasks do not interfere.

#### **2.4 Audio / Playback Sync**

- If multiple tasks need to coordinate (e.g., mic capturing is paused or resumed when speaker playback happens), an EventGroup bit or two could manage this.

#### **2.5 Power Saving or Low-Power Mode**

- If the device transitions to a low-power mode, you can set a bit `LOW_POWER` that tasks check or wait upon to adjust their behavior.

---

### 3. Summary of the Architecture

| **Task**              | **Responsibilities**                                         | **Possible EventGroup Bits**                     |
| --------------------- | ------------------------------------------------------------ | ------------------------------------------------ |
| **LCD Display Task**  | Screen updates, user interface rendering                     | `DISPLAY_UPDATE`, `ERROR_STATE`                  |
| **Speaker Task**      | Audio playback, buffer management                            | N/A (triggered by queue, or event bit if needed) |
| **Microphone Task**   | Audio capture, preprocessing, streaming                      | `AUDIO_CAPTURE_START`, `AUDIO_CAPTURE_STOP`      |
| **MQTT Client Task**  | Connect to broker, publish/subscribe messages                | `WIFI_CONNECTED`                                 |
| **RTC Task**          | Real-time audio/video, handle RTC states                     | `WIFI_CONNECTED`, `RTC_CONNECTED`                |
| **BLE Task**          | On-boarding, occasional OTA, can disable BLE when not needed | `BLE_CONNECTED`, `BLE_DATA_READY`, `BLE_DONE`    |
| **System Controller** | Overall device state management, power modes, error handling | `SYSTEM_READY`, `LOW_POWER_MODE`, `ERROR_STATE`  |

- **EventGroups** help coordinate states (e.g., Wi-Fi, BLE, OTA).
- **Dedicated tasks** focus on continuous or time-critical operations (e.g., audio, display, RTC, MQTT).

---

### 4. Design Rationale

1. **Avoid Monolithic Design**

   - Splitting functionality into tasks keeps each subsystem independent and easier to maintain.

2. **Real-Time Constraints**

   - Microphone and RTC tasks likely need higher priority for reliable streaming.

3. **Low Power & On-Demand**

   - BLE can be shut down entirely outside of on-boarding/OTA.
   - This approach conserves power and frees resources.

4. **Ease of Event Synchronization**

   - EventGroups can be used liberally to avoid creating more threads than necessary for simple state signaling.

5. **Scalability**

   - New features or extended functionalities (e.g., more advanced UI, analytics, etc.) can be added as new tasks or by using EventGroups to coordinate with existing tasks.


**Task:**

Create a program based on [ESP32](https://www.espressif.com/en/products/socs/esp32) to meassure the air quality and show it on a mobile application. Use [MQTT](https://mqtt.org) to publish the measured values. A temporary test MQTT server (message broker) is provided.

The program needs to measure the concentration of carbon monoxide using the MQ-7 gas sensor and detect smoke using the MQ-135 gas sensor.

> [!NOTE]
> This task description is approximate.

**Screenshots of [Mqtt Dashboard](https://play.google.com/store/apps/details?id=com.app.vetru.mqttdashboard) Android application**

[screenshots/01.jpg](screenshots/01.jpg) - the carbon monoxide measured value (MQ-7).

[screenshots/02.jpg](screenshots/02.jpg) - broker connection settings.

**Photos**

[photos/01.jpg](photos/01.jpg) - devices connected with jumper wires (Dupont) on a breadboard.

[photos/02.jpg](photos/02.jpg) - MQ gas sensor PCB.

**Logs**

[logs/AQM.log](logs/AQM.log) - extract from [PlatformIO](https://platformio.org) monitor. Lines 79-87 show the MQ-7 and MQ-135 measured values and a text about the published MQTT message.

> [!IMPORTANT]
> You may need to create your own server (MQTT message broker) and set the connection details (username, password, and server address) into `MQTT_SERVER_URL` pre-processor macro.
> 
> Assign your Wi-Fi details to `WIFI_SSID` and `WIFI_PASSWORD`.

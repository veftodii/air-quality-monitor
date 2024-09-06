#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "mqtt_client.h"

// Pre-processor macros definition
#define ADC_DEF_VREF 1100 // mV
#define ADC_AVG_FLT_SAMPLES 64  // Number of samples used for ADC multisampling (Average software filter)
#define WIFI_SSID "Eftodii House-1   2.4GHz"
#define WIFI_PASSWORD ""
#define WIFI_AUTH_MODE WIFI_AUTH_OPEN
#define WIFI_MAX_RETRY 3  // Avoid unlimited reconnection to the access point when it's not accessible
#define WIFI_STB_CONNECTED BIT0  // Status bit 0 - connected to the access point with an IP
#define WIFI_STB_CONN_FAILED BIT1  // Status bit 1 - failed to connect after the maximum amount of retries
#define MQTT_SERVER_URL "mqtt://user:<Place the password here!>@mqtt.h2881725.stratoserver.net"  // Address format: <protocol_id>://<username>:<password>@<URL>[:<port>]

// Global variables declaration
char * const appTag = "AQM";  // Air quality monitor
EventGroupHandle_t wifiEventGroupHandle = NULL;

// Global functions prototype
void WiFiInitStation();
void WiFiEventHandler(void*, esp_event_base_t, int32_t, void*);
void MQTTEventHandler(void*, esp_event_base_t, int32_t, void*);
esp_mqtt_client_handle_t MQTTClientStart();

void app_main() {
    const adc_channel_t mq7ChannelNumber = ADC1_CHANNEL_6, mq135ChannelNumber = ADC1_CHANNEL_7;
    const adc_atten_t adcChannelAttenuation = ADC_ATTEN_DB_11;
    const adc_bits_width_t adcChannelPrecision = ADC_WIDTH_BIT_12;
    esp_adc_cal_characteristics_t *adcCharacteristics = NULL;
    esp_mqtt_client_handle_t mqttClientHandle = NULL;
    int mq7ChannelValue = 0, mq7ChannelVoltage = 0, mq135ChannelValue = 0, mq135ChannelVoltage = 0;
    char mq7ChannelVoltageText[30] = "";

    ESP_LOGI(appTag, "[APP] Startup ...");
    ESP_LOGI(appTag, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(appTag, "[APP] IDF version: %s", esp_get_idf_version());

    // MQTT
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Configure ADC
    adc1_config_width(adcChannelPrecision);
    adc1_config_channel_atten(mq7ChannelNumber, adcChannelAttenuation);
    adc1_config_channel_atten(mq135ChannelNumber, adcChannelAttenuation);

    // Characterize ADC
    adcCharacteristics = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(ADC_UNIT_1, adcChannelAttenuation, adcChannelPrecision, ADC_DEF_VREF, adcCharacteristics);

    // Initialize and connect to WiFi network
    WiFiInitStation();

    // Connect to MQTT server
    mqttClientHandle = MQTTClientStart();
    
    while (1) {
        // Continuously sample ADC1
        // USe multisampling to mitigate the effects of noise
        mq7ChannelValue = 0;
        mq135ChannelValue = 0;
        for (int i = 0; i < ADC_AVG_FLT_SAMPLES; i++) {
            mq7ChannelValue += adc1_get_raw(mq7ChannelNumber);
            mq135ChannelValue += adc1_get_raw(mq135ChannelNumber);
        }
        mq7ChannelValue /= ADC_AVG_FLT_SAMPLES;
        mq135ChannelValue /= ADC_AVG_FLT_SAMPLES;

        // Convert to voltage (mV)
        mq7ChannelVoltage = esp_adc_cal_raw_to_voltage(mq7ChannelValue, adcCharacteristics);
        mq135ChannelVoltage = esp_adc_cal_raw_to_voltage(mq135ChannelValue, adcCharacteristics);
        printf("MQ7: %d (%d mV)\tMQ135: %d (%d mV)\n", mq7ChannelValue, mq7ChannelVoltage, mq135ChannelValue, mq135ChannelVoltage);

        // Publish data on the MQTT server
        snprintf(mq7ChannelVoltageText, sizeof(mq7ChannelVoltageText), "%d", mq7ChannelVoltage);
        esp_mqtt_client_publish(mqttClientHandle, "/user/out/adc",  mq7ChannelVoltageText, 0, 1, 0);

        /* EN50291 - The European standard for carbon monoxide alarm definitions
           Main alarm requirements:
           - at 30ppm CO, the alarm must not activate for at least 120 minutes;
           - at 50ppm CO, the alarm must not activate before 60 minutes but must activate before 90 minutes;
           - at 100ppm CO, the alarm must not activate before 10 minutes but must activate before 40 minutes;
           - at 300ppm CO, the alarm must activate within 3 minutes. */

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// Global functions definition
void WiFiEventHandler(void *eventHandlerArgs, esp_event_base_t eventBase, int32_t eventID, void *eventData) {
    static int retryCounter = 0;

    if (eventBase == WIFI_EVENT && eventID == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (eventBase == WIFI_EVENT && eventID == WIFI_EVENT_STA_DISCONNECTED) {
        if (retryCounter < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            retryCounter++;
            ESP_LOGI(appTag, "Retry connecting to \"%s\"", WIFI_SSID);
        }
        else {
            xEventGroupSetBits(wifiEventGroupHandle, WIFI_STB_CONN_FAILED);
            ESP_LOGI(appTag, "Failed to connect to \"%s\"", WIFI_SSID);
        }
    }
    else if (eventBase == IP_EVENT && eventID == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *eventHandle = (ip_event_got_ip_t*) eventData;
        retryCounter = 0;
        xEventGroupSetBits(wifiEventGroupHandle, WIFI_STB_CONNECTED);
        ESP_LOGI(appTag, "Connected to \"%s\"", WIFI_SSID);
        ESP_LOGI(appTag, "Allocated IP address: " IPSTR, IP2STR(&eventHandle -> ip_info.ip));
    }
}

// Manual connection to network
void WiFiInitStation() {
    wifiEventGroupHandle = xEventGroupCreate();

    ESP_LOGI(appTag, "ESP_WIFI_MODE_STA");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifiInitConfig = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifiInitConfig));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFiEventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WiFiEventHandler, NULL));

    wifi_config_t wifiConfig = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
	        .threshold.authmode = WIFI_AUTH_MODE,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifiConfig));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(appTag, "WiFiInitStation() finished");

    // Wait until either the connection is established or failed to connect for the maximum number of retries
    xEventGroupWaitBits(wifiEventGroupHandle, WIFI_STB_CONNECTED | WIFI_STB_CONN_FAILED, pdFALSE, pdFALSE, portMAX_DELAY);

    // Unregister events processing to not reconnect automatically to network
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &WiFiEventHandler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFiEventHandler));
    vEventGroupDelete(wifiEventGroupHandle);
}

void MQTTEventHandler(void *eventHandlerArgs, esp_event_base_t eventBase, int32_t eventID, void *eventData) {
    esp_mqtt_event_handle_t eventHandle = eventData;

    ESP_LOGD(appTag, "Event dispatched from event loop: base = %s, id = %d", eventBase, eventID);

    switch (eventHandle -> event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(appTag, "MQTT_EVENT_CONNECTED");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(appTag, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(appTag, "MQTT_EVENT_PUBLISHED: msg_id = %d", eventHandle -> msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(appTag, "MQTT_EVENT_DATA");
            printf("TOPIC = %.*s\r\n", eventHandle -> topic_len, eventHandle -> topic);
            printf("DATA = %.*s\r\n", eventHandle -> data_len, eventHandle -> data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(appTag, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(appTag, "Other event: id = %d", eventHandle -> event_id);
            break;
    }
}

esp_mqtt_client_handle_t MQTTClientStart() {
    esp_mqtt_client_handle_t mqttClientHandle = NULL;
    esp_mqtt_client_config_t mqttClientConfig = {
        .uri = MQTT_SERVER_URL,
    };

    mqttClientHandle = esp_mqtt_client_init(&mqttClientConfig);
    esp_mqtt_client_register_event(mqttClientHandle, ESP_EVENT_ANY_ID, &MQTTEventHandler, mqttClientHandle);
    esp_mqtt_client_start(mqttClientHandle);

    return mqttClientHandle;
}
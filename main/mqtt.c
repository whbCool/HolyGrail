/**
 * @file mqtt.c
 * @brief MQTT client telemetry publisher implementation.
 */

#include "mqtt.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "HolyGrail_MQTT";
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_connected = false;

/**
 * @brief Event handler for MQTT events.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to MQTT broker successfully!");
            s_connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from MQTT broker!");
            s_connected = false;
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGD(TAG, "MQTT Topic Subscribed, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGD(TAG, "MQTT Topic Unsubscribed, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "MQTT Message Published, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGD(TAG, "MQTT Data Received");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Client encountered an error!");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "Last error code reported: 0x%x", event->error_handle->esp_transport_sock_errno);
            }
            break;
        default:
            break;
    }
}

esp_err_t mqtt_init(void) {
    ESP_LOGI(TAG, "Initializing MQTT Client connection...");

    // Configure the MQTT client structure
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = HOLYGRAIL_MQTT_BROKER_URI,
                .port = HOLYGRAIL_MQTT_PORT
            }
        }
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to instantiate MQTT client handle!");
        return ESP_FAIL;
    }

    // Register event handler with the MQTT event loop
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));

    // Start client worker thread
    esp_err_t ret = esp_mqtt_client_start(s_mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client started connecting to %s:%d", HOLYGRAIL_MQTT_BROKER_URI, HOLYGRAIL_MQTT_PORT);
    return ESP_OK;
}

bool mqtt_is_connected(void) {
    return s_connected;
}

esp_err_t mqtt_publish_telemetry(const char *payload) {
    if (s_mqtt_client == NULL || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    // Publish to the specified topic
    // QOS = 1 (at least once delivery), Retain = 0 (no retention)
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, HOLYGRAIL_MQTT_TOPIC, payload, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish MQTT message!");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Published message to topic '%s', msg_id=%d", HOLYGRAIL_MQTT_TOPIC, msg_id);
    return ESP_OK;
}

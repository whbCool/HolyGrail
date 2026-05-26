/**
 * @file mqtt.h
 * @brief MQTT client telemetry publisher for Holy Grail.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

// Suggesting default MQTT telemetry parameters
#define HOLYGRAIL_MQTT_BROKER_URI   "mqtt://192.168.1.100"
#define HOLYGRAIL_MQTT_PORT         1883
#define HOLYGRAIL_MQTT_TOPIC        "holygrail/radar/telemetry"

/**
 * @brief Initialize and start the MQTT client service.
 * 
 * Registers event handlers and starts the asynchronous MQTT connection worker.
 * 
 * @return ESP_OK on success.
 */
esp_err_t mqtt_init(void);

/**
 * @brief Check if the MQTT client is actively connected to the broker.
 * 
 * @return true if connected.
 */
bool mqtt_is_connected(void);

/**
 * @brief Publish a serialized JSON payload string to the MQTT broker.
 * 
 * @param payload The null-terminated JSON string to publish.
 * @return ESP_OK on success.
 */
esp_err_t mqtt_publish_telemetry(const char *payload);

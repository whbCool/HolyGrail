#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "ld2410c.h"
#include <stdio.h>

static const char *TAG = "HolyGrail_Radar";

// Pin configuration for LD2410C communication
#define LD2410C_TX_PIN 17 // ESP32 TX pin -> Sensor RXD pin
#define LD2410C_RX_PIN 16 // ESP32 RX pin -> Sensor TXD pin
#define LD2410C_UART_PORT UART_NUM_1

void app_main(void) {
  ESP_LOGI(TAG, "Initializing HolyGrail Radar application...");

  // 1. Configure the UART interface for HLK-LD2410C
  // The sensor defaults to 256000 baud, 8N1
  uart_config_t uart_config = {
      .baud_rate = 256000,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  ESP_ERROR_CHECK(uart_driver_install(LD2410C_UART_PORT, 256, 0, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(LD2410C_UART_PORT, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(LD2410C_UART_PORT, LD2410C_TX_PIN,
                               LD2410C_RX_PIN, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));

  ESP_LOGI(TAG, "UART initialized on port %d (TX: %d, RX: %d) at 256000 baud",
           LD2410C_UART_PORT, LD2410C_TX_PIN, LD2410C_RX_PIN);

  // 2. Initialize the LD2410C handler
  // Timeout set to 1000ms
  ld2410c_handle_t *ld = ld2410c_init(LD2410C_UART_PORT, 1000);
  if (ld == NULL) {
    ESP_LOGE(TAG, "Failed to initialize LD2410C driver context!");
    return;
  }

  ESP_LOGI(TAG, "LD2410C driver context successfully initialized.");

  uint8_t buffer[64];
  size_t length = 0;

  // 3. Continuously read and parse incoming sensor data
  while (1) {
    if (ld2410c_read_data_frame(ld, buffer, sizeof(buffer), &length) ==
        ESP_OK) {
      ld2410c_target_data_t target;
      if (ld2410c_parse_target_data(buffer, length, &target) == ESP_OK) {
        const char *state_str = "Unknown";
        switch (target.state) {
        case LD2410C_TARGET_NONE:
          state_str = "No Presence";
          break;
        case LD2410C_TARGET_MOVING:
          state_str = "Moving Target Detected";
          break;
        case LD2410C_TARGET_STATIONARY:
          state_str = "Stationary Target Detected";
          break;
        case LD2410C_TARGET_BOTH:
          state_str = "Combined Target Detected";
          break;
        case LD2410C_TARGET_NOISE_DET:
          state_str = "Background Noise Detection";
          break;
        case LD2410C_TARGET_NOISE_OK:
          state_str = "Noise Detection Succeeded";
          break;
        case LD2410C_TARGET_NOISE_FAIL:
          state_str = "Noise Detection Failed";
          break;
        default:
          break;
        }

        ESP_LOGI(TAG, "[Radar State]: %s", state_str);

        if (target.state == LD2410C_TARGET_MOVING ||
            target.state == LD2410C_TARGET_BOTH) {
          ESP_LOGI(TAG, "  -> Moving Target Distance: %d cm (Energy: %d)",
                   target.moving_distance_cm, target.moving_energy);
        }
        if (target.state == LD2410C_TARGET_STATIONARY ||
            target.state == LD2410C_TARGET_BOTH) {
          ESP_LOGI(TAG, "  -> Stationary Target Distance: %d cm (Energy: %d)",
                   target.stationary_distance_cm, target.stationary_energy);
        }
      } else {
        ESP_LOGE(TAG, "Received frame, but failed to parse target data.");
      }
    }

    // Sleep to yield CPU to other tasks (100ms)
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

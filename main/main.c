/**
 * @file main.c
 * @brief Main entry point for the Holy Grail custom aggregator board.
 *
 * Aggregates presence and distance data from 12 HLK-LD2410C radar sensors
 * using SPI-multiplexed SC16IS752 bridges, and publishes telemetry over
 * Ethernet (W5500) to an MQTT broker in JSON format.
 */

#include "esp_err.h"
#include "esp_log.h"
#include "ethernet.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ld2410c.h"
#include "mqtt.h"
#include "sc16is752.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "HolyGrail_Main";

/* ESP32 Hardware Pin Assignments based on Schematics */
#define ESP_SPI_HOST SPI2_HOST /**< Standard HSPI/SPI2 on ESP32 */
#define PIN_SPI_SCLK 18        /**< Shared SPI Clock */
#define PIN_SPI_MISO 19        /**< Shared SPI MISO */
#define PIN_SPI_MOSI 23        /**< Shared SPI MOSI */

/* CD74HC138 Line Decoder Pins */
#define PIN_DECODER_A0 25
#define PIN_DECODER_A1 26
#define PIN_DECODER_A2 32
#define PIN_DECODER_E3 33 /**< Active-High Enable */

/* W5500 Ethernet Pins */
#define PIN_W5500_CS 5   /**< Active-Low Chip Select */
#define PIN_W5500_RST 2 /**< W5500 Hardware Reset (IO2) */

/* Dynamic MISO Buffer Isolation Pin (Tied to IC12 OE) */
#define PIN_MISO_OE 27 /**< Active-Low OE control */

/* Status LED */
#define PIN_STATUS_LED 21

/* Sensor Target Baud Rate */
#define SENSOR_BAUDRATE                                                        \
  230400 /**< Configured for 0.00% clock error with 14.7456 MHz crystal */

/* Global Mutex for thread-safe access to sensor state table */
static SemaphoreHandle_t s_sensor_table_mutex = NULL;

/* Aggregated sensor state array */
typedef struct {
  char name[32];
  ld2410c_target_state_t state;
  uint16_t moving_distance_cm;
  uint8_t moving_energy;
  uint16_t stationary_distance_cm;
  uint8_t stationary_energy;
  bool is_online;
} aggregated_sensor_data_t;

static aggregated_sensor_data_t s_sensor_telemetry[12];

/* Physical layout logical mapping */
typedef struct {
  uint8_t chip_idx;            /**< CD74HC138 address output index (0..5) */
  sc16is752_channel_t channel; /**< SC16IS752 UART channel (A or B) */
  const char *name;            /**< User-facing label */
} sensor_map_t;

const sensor_map_t SENSOR_MAP[12] = {
    {0, SC16IS752_CHAN_A, "BankA_Sensor1"}, // U1 Chan A
    {2, SC16IS752_CHAN_A, "BankA_Sensor2"}, // U4 Chan A
    {4, SC16IS752_CHAN_A, "BankA_Sensor3"}, // U6 Chan A
    {1, SC16IS752_CHAN_A, "BankA_Sensor4"}, // U2 Chan A
    {3, SC16IS752_CHAN_A, "BankA_Sensor5"}, // U5 Chan A
    {5, SC16IS752_CHAN_A, "BankA_Sensor6"}, // U7 Chan A
    {0, SC16IS752_CHAN_B, "BankB_Sensor1"}, // U1 Chan B
    {2, SC16IS752_CHAN_B, "BankB_Sensor2"}, // U4 Chan B
    {4, SC16IS752_CHAN_B, "BankB_Sensor3"}, // U6 Chan B
    {1, SC16IS752_CHAN_B, "BankB_Sensor4"}, // U2 Chan B
    {3, SC16IS752_CHAN_B, "BankB_Sensor5"}, // U5 Chan B
    {5, SC16IS752_CHAN_B, "BankB_Sensor6"}  // U7 Chan B
};

/**
 * @brief Initialize all board hardware peripherals.
 */
static void init_board_hardware(void) {
  ESP_LOGI(TAG, "Initializing Holy Grail hardware interfaces...");

  // 1. Initialize Status LED
  gpio_config_t led_conf = {.intr_type = GPIO_INTR_DISABLE,
                            .mode = GPIO_MODE_OUTPUT,
                            .pin_bit_mask = (1ULL << PIN_STATUS_LED),
                            .pull_down_en = GPIO_PULLDOWN_DISABLE,
                            .pull_up_en = GPIO_PULLUP_DISABLE};
  gpio_config(&led_conf);
  gpio_set_level(PIN_STATUS_LED, 1); // Turn ON status LED on boot

  // 2. Initialize Shared SPI Bus
  spi_bus_config_t buscfg = {.miso_io_num = PIN_SPI_MISO,
                             .mosi_io_num = PIN_SPI_MOSI,
                             .sclk_io_num = PIN_SPI_SCLK,
                             .quadwp_io_num = -1,
                             .quadhd_io_num = -1,
                             .max_transfer_sz = 4096};
  ESP_ERROR_CHECK(spi_bus_initialize(ESP_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

  // 3. Initialize Multiplexed SC16IS752 Driver
  sc16is752_hw_config_t sc_hw_cfg = {
      .spi_host = ESP_SPI_HOST,
      .pin_a0 = PIN_DECODER_A0,
      .pin_a1 = PIN_DECODER_A1,
      .pin_a2 = PIN_DECODER_A2,
      .pin_e3 = PIN_DECODER_E3,
      .pin_miso_oe = PIN_MISO_OE,
      .pin_irq = -1 // Polling mode is used for high-speed sensor aggregation
  };
  ESP_ERROR_CHECK(sc16is752_init(&sc_hw_cfg));

  // Configure all 12 physical UART channels for standard 230400 bps baud rate
  for (int i = 0; i < 12; i++) {
    const sensor_map_t *sensor = &SENSOR_MAP[i];
    esp_err_t ret = sc16is752_configure_uart(sensor->chip_idx, sensor->channel,
                                             SENSOR_BAUDRATE);
    if (ret == ESP_OK) {
      s_sensor_telemetry[i].is_online = true;
      strcpy(s_sensor_telemetry[i].name, sensor->name);
      s_sensor_telemetry[i].state = LD2410C_TARGET_NONE;
      ESP_LOGI(TAG, "Port successfully initialized: %s (Chip: %d, Chan: %d)",
               sensor->name, sensor->chip_idx, sensor->channel);
    } else {
      s_sensor_telemetry[i].is_online = false;
      ESP_LOGE(TAG, "Failed to initialize port: %s (Chip: %d, Chan: %d)",
               sensor->name, sensor->chip_idx, sensor->channel);
    }
  }

  // 4. Initialize W5500 Ethernet Controller
  ethernet_config_t eth_cfg = {.spi_host = ESP_SPI_HOST,
                               .pin_cs = PIN_W5500_CS,
                               .pin_rst = PIN_W5500_RST,
                               .pin_intr = -1};
  ESP_ERROR_CHECK(ethernet_init(&eth_cfg));

  // Create thread safety mutex
  s_sensor_table_mutex = xSemaphoreCreateMutex();
  if (s_sensor_table_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create telemetry table mutex!");
  }
}

/**
 * @brief High-speed radar sensor aggregation loop.
 */
static void radar_aggregator_task(void *pvParameters) {
  ESP_LOGI(TAG, "Radar Aggregator task started.");
  uint8_t rx_buffer[64];

  while (1) {
    // Sequentially poll all 12 physical radar channels
    for (int i = 0; i < 12; i++) {
      if (!s_sensor_telemetry[i].is_online)
        continue;

      const sensor_map_t *sensor = &SENSOR_MAP[i];

      // Read pending serial bytes from the UART bridge
      int rx_len = sc16is752_read_bytes(sensor->chip_idx, sensor->channel,
                                        rx_buffer, sizeof(rx_buffer));

      if (rx_len > 0) {
        ld2410c_target_data_t target;

        // Parse raw serial frame using LD2410C driver utility
        if (ld2410c_parse_target_data(rx_buffer, rx_len, &target) == ESP_OK) {
          xSemaphoreTake(s_sensor_table_mutex, portMAX_DELAY);
          s_sensor_telemetry[i].state = target.state;
          s_sensor_telemetry[i].moving_distance_cm = target.moving_distance_cm;
          s_sensor_telemetry[i].moving_energy = target.moving_energy;
          s_sensor_telemetry[i].stationary_distance_cm =
              target.stationary_distance_cm;
          s_sensor_telemetry[i].stationary_energy = target.stationary_energy;
          xSemaphoreGive(s_sensor_table_mutex);

          ESP_LOGD(TAG, "[%s] State: %d, MovDist: %d cm, StatDist: %d cm",
                   sensor->name, target.state, target.moving_distance_cm,
                   target.stationary_distance_cm);
        }
      }
    }

    // yield CPU to prevent starving lower priority threads (10 ms resolution)
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/**
 * @brief Periodic publisher task sending telemetry JSON payload over Ethernet.
 */
static void telemetry_publish_task(void *pvParameters) {
  ESP_LOGI(TAG, "Telemetry publisher task started.");
  char json_payload[2048];
  bool mqtt_ready = false;

  while (1) {
    // Toggle Status LED as heart-beat indicator
    static int led_state = 0;
    gpio_set_level(PIN_STATUS_LED, led_state);
    led_state = !led_state;

    // Try to establish MQTT connection once Ethernet Link gets IP
    if (!mqtt_ready && ethernet_is_connected()) {
      ESP_LOGI(TAG,
               "Ethernet connection verified. Initializing MQTT client...");
      if (mqtt_init() == ESP_OK) {
        mqtt_ready = true;
      }
    }

    // If MQTT broker is active, publish latest data in JSON format
    if (mqtt_ready && mqtt_is_connected()) {
      size_t offset = 0;
      offset += snprintf(json_payload + offset, sizeof(json_payload) - offset,
                         "{\n  \"sensors\": [\n");

      xSemaphoreTake(s_sensor_table_mutex, portMAX_DELAY);
      for (int i = 0; i < 12; i++) {
        offset += snprintf(
            json_payload + offset, sizeof(json_payload) - offset,
            "    {\n"
            "      \"name\": \"%s\",\n"
            "      \"state\": %d,\n"
            "      \"moving_distance_cm\": %d,\n"
            "      \"moving_energy\": %d,\n"
            "      \"stationary_distance_cm\": %d,\n"
            "      \"stationary_energy\": %d\n"
            "    }%s\n",
            s_sensor_telemetry[i].name, s_sensor_telemetry[i].state,
            s_sensor_telemetry[i].moving_distance_cm,
            s_sensor_telemetry[i].moving_energy,
            s_sensor_telemetry[i].stationary_distance_cm,
            s_sensor_telemetry[i].stationary_energy, (i == 11) ? "" : ",");
      }
      xSemaphoreGive(s_sensor_table_mutex);

      offset += snprintf(json_payload + offset, sizeof(json_payload) - offset,
                         "  ]\n}");

      // Publish message
      mqtt_publish_telemetry(json_payload);
    }

    // Delay for 100ms (10Hz reporting speed for high-performance escape room
    // triggers)
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "================================================");
  ESP_LOGI(TAG, "TEA@CPP Escape Room 2026: The Holy Grail");
  ESP_LOGI(TAG, "Custom Aggregator Board Firmware Booting...");
  ESP_LOGI(TAG, "================================================");

  // Initialize all custom PCB components
  init_board_hardware();

  // Spawn high-speed radar polling thread
  xTaskCreatePinnedToCore(radar_aggregator_task, "radar_aggregator", 4096, NULL,
                          5, NULL, 0);

  // Spawn periodic network publisher thread
  xTaskCreatePinnedToCore(telemetry_publish_task, "telemetry_publish", 4096,
                          NULL, 4, NULL, 1);
}

/**
 * @file main.c
 * @brief Main entry point for the Holy Grail custom aggregator board.
 *
 * Aggregates presence and distance data from 12 HLK-LD2410C radar sensors
 * using SPI-multiplexed SC16IS752 bridges, and publishes telemetry over
 * Ethernet (W5500) to an MQTT broker in JSON format.
 */

#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "ethernet.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ld2410c.h"
#include "mqtt.h"
#include "rom/ets_sys.h"
#include "sc16is752.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "HolyGrail_Main";

/* Set to 1 to bypass Ethernet and print radar sensor telemetry to serial
 * monitor for testing */
#define DISABLE_ETHERNET_FOR_TESTING 1

/* Set to 1 to run SC16IS752 register validation diagnostic on boot */
#define RUN_CHIP_VALIDATION_DIAGNOSTIC 0

/* ESP32 Hardware Pin Assignments based on Schematics */
#define ESP_SPI_HOST SPI3_HOST /**< Standard VSPI/SPI3 on ESP32 */
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
#define PIN_W5500_RST 13 /**< W5500 Hardware Reset (IO13, bodged) */

/* SC16IS752 UART Bridge Reset Pin */
#define PIN_SC16IS752_RST 2 /**< SC16IS752 Hardware Reset (IO2, Active-Low) */

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

  // 1. Install GPIO ISR service to prevent warning/error
  gpio_install_isr_service(0);

  // 2. Pre-configure CS, RST, and Decoder Enable pins as outputs and set to
  // inactive state. This prevents bus contention on the shared MISO line at
  // boot.
  gpio_config_t rst_cs_conf = {
      .intr_type = GPIO_INTR_DISABLE,
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = ((1ULL << PIN_W5500_CS) | (1ULL << PIN_W5500_RST) |
                       (1ULL << PIN_SC16IS752_RST) | (1ULL << PIN_MISO_OE) |
                       (1ULL << PIN_DECODER_E3)),
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .pull_up_en = GPIO_PULLUP_DISABLE};
  gpio_config(&rst_cs_conf);

  // Set initial safe states: CS high, W5500 Reset high (running), SC16IS752
  // Reset high (running), MISO OE low (enabled), Decoder E3 low (disabled)
  gpio_set_level(PIN_W5500_CS, 1);
  gpio_set_level(PIN_W5500_RST, 1);
  gpio_set_level(PIN_SC16IS752_RST, 1); // Active-Low reset: HIGH to let it run
  gpio_set_level(PIN_MISO_OE, 0);
  gpio_set_level(PIN_DECODER_E3, 0);

  // 3. Pulse the Hardware Reset lines to reset W5500 and all SC16IS752 chips
  ESP_LOGI(TAG, "Executing hardware reset for W5500 and SC16IS752 array...");
  gpio_set_level(PIN_W5500_RST, 0);     // Reset W5500 (Active-Low)
  gpio_set_level(PIN_SC16IS752_RST, 0); // Reset SC16IS752 array (Active-Low)
  vTaskDelay(pdMS_TO_TICKS(10));        // Pulse for 10ms
  gpio_set_level(PIN_W5500_RST, 1);     // Release W5500 from reset
  gpio_set_level(PIN_SC16IS752_RST, 1); // Release SC16IS752 array from reset
  vTaskDelay(pdMS_TO_TICKS(
      150)); // Wait 150ms for chip PLLs and internal registers to stabilize

  // 4. Initialize Status LED
  gpio_config_t led_conf = {.intr_type = GPIO_INTR_DISABLE,
                            .mode = GPIO_MODE_OUTPUT,
                            .pin_bit_mask = (1ULL << PIN_STATUS_LED),
                            .pull_down_en = GPIO_PULLDOWN_DISABLE,
                            .pull_up_en = GPIO_PULLUP_DISABLE};
  gpio_config(&led_conf);
  gpio_set_level(PIN_STATUS_LED, 1); // Turn ON status LED on boot

  // 5. Initialize Shared SPI Bus
  spi_bus_config_t buscfg = {.miso_io_num = PIN_SPI_MISO,
                             .mosi_io_num = PIN_SPI_MOSI,
                             .sclk_io_num = PIN_SPI_SCLK,
                             .quadwp_io_num = -1,
                             .quadhd_io_num = -1,
                             .max_transfer_sz = 4096};
  ESP_ERROR_CHECK(spi_bus_initialize(ESP_SPI_HOST, &buscfg, SPI_DMA_DISABLED));

  // 6. Initialize Multiplexed SC16IS752 Driver
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

  // 7. Initialize W5500 Ethernet Controller (pass pin_rst = -1 since manual
  // reset is done)
#if !DISABLE_ETHERNET_FOR_TESTING
  ethernet_config_t eth_cfg = {.spi_host = ESP_SPI_HOST,
                               .pin_cs = PIN_W5500_CS,
                               .pin_rst = -1,
                               .pin_intr = -1};
  ESP_ERROR_CHECK(ethernet_init(&eth_cfg));
#else
  ESP_LOGW(TAG, "Ethernet initialization BYPASSED for testing.");
#endif

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
        // Rate-limited debug log of raw RX data to see if we receive anything and what it is
        static TickType_t last_rx_log_ticks[12] = {0};
        TickType_t now = xTaskGetTickCount();
        if (now - last_rx_log_ticks[i] >= pdMS_TO_TICKS(3000)) {
          last_rx_log_ticks[i] = now;
          char hex_str[128] = {0};
          size_t hex_len = 0;
          for (int b = 0; b < (rx_len > 16 ? 16 : rx_len); b++) {
            hex_len += snprintf(hex_str + hex_len, sizeof(hex_str) - hex_len, "%02X ", rx_buffer[b]);
          }
          ESP_LOGI(TAG, "Raw RX from [%s]: %d bytes: %s%s", 
                   sensor->name, rx_len, hex_str, rx_len > 16 ? "..." : "");
        }

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

#if DISABLE_ETHERNET_FOR_TESTING
          // Rate-limited print every 500ms per sensor to avoid spamming the
          // console
          static TickType_t last_print_ticks[12] = {0};
          if (now - last_print_ticks[i] >= pdMS_TO_TICKS(500)) {
            last_print_ticks[i] = now;
            ESP_LOGI(TAG,
                     "Sensor [%s] -> State: %d, MovDist: %d cm, StatDist: %d "
                     "cm, MovEnergy: %d, StatEnergy: %d",
                     sensor->name, target.state, target.moving_distance_cm,
                     target.stationary_distance_cm, target.moving_energy,
                     target.stationary_energy);
          }
#else
          ESP_LOGD(TAG, "[%s] State: %d, MovDist: %d cm, StatDist: %d cm",
                   sensor->name, target.state, target.moving_distance_cm,
                   target.stationary_distance_cm);
#endif
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

#if DISABLE_ETHERNET_FOR_TESTING
    // Print the JSON telemetry payload directly to the serial console every 2
    // seconds
    static int print_counter = 0;
    print_counter++;
    if (print_counter >= 20) { // 20 * 100ms = 2 seconds
      print_counter = 0;

      size_t offset = 0;
      offset += snprintf(json_payload + offset, sizeof(json_payload) - offset,
                         "{\n  \"sensors\": [\n");

      xSemaphoreTake(s_sensor_table_mutex, portMAX_DELAY);
      for (int i = 0; i < 12; i++) {
        if (!s_sensor_telemetry[i].is_online)
          continue;

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

      printf("\n--- Local Telemetry JSON Output "
             "---\n%s\n-----------------------------------\n",
             json_payload);
    }
#else
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
#endif

    // Delay for 100ms (10Hz reporting speed for high-performance escape room
    // triggers)
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static void run_sc16is752_diagnostic(void) {
  ESP_LOGI(TAG, "=================================================");
  ESP_LOGI(TAG, "RUNNING SC16IS752 CHIP VALIDATION DIAGNOSTIC...");
  ESP_LOGI(TAG, "=================================================");

  const char *chip_names[6] = {"U1", "U4", "U6", "U2", "U5", "U7"};
  uint8_t test_patterns[] = {0x55, 0xAA, 0x00};
  int total_failed = 0;

  for (int chip_idx = 0; chip_idx < 6; chip_idx++) {
    bool chip_ok = true;
    ESP_LOGI(TAG, "Testing Chip %d (%s)...", chip_idx, chip_names[chip_idx]);

    for (sc16is752_channel_t chan = SC16IS752_CHAN_A; chan < SC16IS752_CHAN_MAX;
         chan++) {
      const char *chan_name = (chan == SC16IS752_CHAN_A) ? "A" : "B";

      for (int p = 0; p < sizeof(test_patterns); p++) {
        uint8_t write_val = test_patterns[p];
        uint8_t read_val = 0xFF; // Start with dummy value

        esp_err_t ret_write =
            sc16is752_write_reg(chip_idx, chan, SC16IS752_REG_SPR, write_val);
        if (ret_write != ESP_OK) {
          ESP_LOGE(TAG, "  Chan %s SPR Write 0x%02X failed with error: %s",
                   chan_name, write_val, esp_err_to_name(ret_write));
          chip_ok = false;
          continue;
        }

        esp_err_t ret_read =
            sc16is752_read_reg(chip_idx, chan, SC16IS752_REG_SPR, &read_val);
        if (ret_read != ESP_OK) {
          ESP_LOGE(TAG, "  Chan %s SPR Read failed with error: %s", chan_name,
                   esp_err_to_name(ret_read));
          chip_ok = false;
          continue;
        }

        if (read_val != write_val) {
          ESP_LOGE(TAG, "  Chan %s SPR Mismatch! Wrote: 0x%02X, Read: 0x%02X",
                   chan_name, write_val, read_val);
          chip_ok = false;
        }
      }
    }

    if (chip_ok) {
      ESP_LOGI(TAG, "Chip %d (%s): [PASS] - Active & Communicating", chip_idx,
               chip_names[chip_idx]);
    } else {
      ESP_LOGE(TAG, "Chip %d (%s): [FAIL] - SPI Communication Error", chip_idx,
               chip_names[chip_idx]);
      total_failed++;
    }
  }

  ESP_LOGI(TAG, "=================================================");
  if (total_failed == 0) {
    ESP_LOGI(TAG, "DIAGNOSTIC COMPLETE: ALL CHIPS PASSED!");
  } else {
    ESP_LOGE(TAG, "DIAGNOSTIC COMPLETE: %d / 6 CHIPS FAILED!", total_failed);
  }
  ESP_LOGI(TAG, "=================================================");
}

static void bitbang_spi_read_spr(void) {
  ESP_LOGI(TAG, "=================================================");
  ESP_LOGI(TAG, "RUNNING BIT-BANGED WRITE-READ DIAGNOSTIC (MISO PULL-DOWN)...");
  ESP_LOGI(TAG, "=================================================");

  // Reset all pins to standard digital GPIO function, freeing them from SPI
  // peripheral block
  gpio_reset_pin(PIN_SPI_SCLK);
  gpio_reset_pin(PIN_SPI_MOSI);
  gpio_reset_pin(PIN_SPI_MISO);
  gpio_reset_pin(PIN_DECODER_A0);
  gpio_reset_pin(PIN_DECODER_A1);
  gpio_reset_pin(PIN_DECODER_A2);
  gpio_reset_pin(PIN_DECODER_E3);
  gpio_reset_pin(PIN_MISO_OE);
  gpio_reset_pin(PIN_SC16IS752_RST);
  gpio_reset_pin(PIN_W5500_CS);

  // Deinitialize RTC function for analog-capable pins to ensure digital GPIO
  // mode
  rtc_gpio_deinit(PIN_SC16IS752_RST);
  rtc_gpio_deinit(PIN_DECODER_A0);
  rtc_gpio_deinit(PIN_DECODER_A1);
  rtc_gpio_deinit(PIN_DECODER_A2);
  rtc_gpio_deinit(PIN_DECODER_E3);
  rtc_gpio_deinit(PIN_MISO_OE);

  // Configure all SPI/Control output pins as GPIOs for manual control
  gpio_config_t cfg = {
      .intr_type = GPIO_INTR_DISABLE,
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = ((1ULL << PIN_SPI_SCLK) | (1ULL << PIN_SPI_MOSI) |
                       (1ULL << PIN_DECODER_A0) | (1ULL << PIN_DECODER_A1) |
                       (1ULL << PIN_DECODER_A2) | (1ULL << PIN_DECODER_E3) |
                       (1ULL << PIN_MISO_OE) | (1ULL << PIN_SC16IS752_RST) |
                       (1ULL << PIN_W5500_CS) | (1ULL << PIN_W5500_RST)),
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .pull_up_en = GPIO_PULLUP_DISABLE};
  gpio_config(&cfg);

  // Configure MISO with pull-down (so floating registers read 0x00, not 0xFF)
  gpio_config_t miso_cfg = {.intr_type = GPIO_INTR_DISABLE,
                            .mode = GPIO_MODE_INPUT,
                            .pin_bit_mask = (1ULL << PIN_SPI_MISO),
                            .pull_down_en = GPIO_PULLDOWN_ENABLE,
                            .pull_up_en = GPIO_PULLUP_DISABLE};
  gpio_config(&miso_cfg);

  // Keep W5500 completely disabled and in reset during this test to avoid bus
  // noise
  gpio_set_level(PIN_W5500_CS, 1);
  gpio_set_level(PIN_W5500_RST,
                 0); // Active-Low reset: LOW holds W5500 in reset

  // 1. MISO Buffer Isolation & PCB Short Test
  ESP_LOGI(TAG, "--- Starting MISO Buffer & PCB Short Test ---");

  // Hold SC16IS752 array in RESET
  gpio_set_level(PIN_SC16IS752_RST, 0);
  gpio_set_level(PIN_DECODER_E3, 0); // Decoder disabled
  gpio_set_level(PIN_MISO_OE, 1);    // MISO Buffer Disabled (tri-state)
  ets_delay_us(5000);
  int miso_disabled = gpio_get_level(PIN_SPI_MISO);
  ESP_LOGI(
      TAG,
      "MISO Buffer DISABLED (OE=1, SC16 in RESET): MISO Pin = %d (expected 0)",
      miso_disabled);

  // Enable MISO Buffer with SC16 in RESET
  gpio_set_level(PIN_MISO_OE, 0); // MISO Buffer Enabled
  ets_delay_us(5000);
  int miso_in_reset = gpio_get_level(PIN_SPI_MISO);
  ESP_LOGI(TAG,
           "MISO Buffer ENABLED  (OE=0, SC16 in RESET): MISO Pin = %d "
           "(expected 0 if no PCB short to VCC)",
           miso_in_reset);

  // Release SC16 reset
  gpio_set_level(PIN_SC16IS752_RST, 1); // Release reset (HIGH)
  ets_delay_us(10000);
  int miso_out_reset = gpio_get_level(PIN_SPI_MISO);
  ESP_LOGI(TAG,
           "MISO Buffer ENABLED  (OE=0, SC16 ACTIVE):   MISO Pin = %d "
           "(expected 0 if chips tri-stated)",
           miso_out_reset);
  ESP_LOGI(TAG, "---------------------------------------------");

  // 4. Run write-then-read test for Chip 0 to 7 under SPI Mode 0
  ESP_LOGI(TAG, "--- Testing all 8 Decoder Address Configurations (0..7) ---");

  // Re-configure decoder, control, and SPI pins as INPUT/OUTPUT to allow
  // loopback readback
  gpio_config_t loop_cfg = {
      .intr_type = GPIO_INTR_DISABLE,
      .mode = GPIO_MODE_INPUT_OUTPUT,
      .pin_bit_mask = ((1ULL << PIN_DECODER_A0) | (1ULL << PIN_DECODER_A1) |
                       (1ULL << PIN_DECODER_A2) | (1ULL << PIN_DECODER_E3) |
                       (1ULL << PIN_MISO_OE) | (1ULL << PIN_SC16IS752_RST) |
                       (1ULL << PIN_SPI_SCLK) | (1ULL << PIN_SPI_MOSI) |
                       (1ULL << PIN_W5500_CS)),
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .pull_up_en = GPIO_PULLUP_DISABLE};
  gpio_config(&loop_cfg);

  // Quick SCLK/MOSI Pin Integrity Check
  gpio_set_level(PIN_SPI_SCLK, 1);
  gpio_set_level(PIN_SPI_MOSI, 1);
  ets_delay_us(10);
  int sclk_high = gpio_get_level(PIN_SPI_SCLK);
  int mosi_high = gpio_get_level(PIN_SPI_MOSI);

  gpio_set_level(PIN_SPI_SCLK, 0);
  gpio_set_level(PIN_SPI_MOSI, 0);
  ets_delay_us(10);
  int sclk_low = gpio_get_level(PIN_SPI_SCLK);
  int mosi_low = gpio_get_level(PIN_SPI_MOSI);

  ESP_LOGI(TAG, "--- SPI Pin Driving Verification ---");
  ESP_LOGI(
      TAG,
      "SCLK Pin: Set HIGH -> Read %d | Set LOW -> Read %d (expected 1 and 0)",
      sclk_high, sclk_low);
  ESP_LOGI(
      TAG,
      "MOSI Pin: Set HIGH -> Read %d | Set LOW -> Read %d (expected 1 and 0)",
      mosi_high, mosi_low);
  if (sclk_high != 1 || sclk_low != 0 || mosi_high != 1 || mosi_low != 0) {
    ESP_LOGE(TAG, "!!! HARDWARE FAILURE: SCLK or MOSI pins are stuck/shorted "
                  "on the ESP32 side !!!");
  } else {
    ESP_LOGI(TAG, "ESP32 SCLK and MOSI pin drivers are electrically healthy.");
  }
  ESP_LOGI(TAG, "------------------------------------");

  for (int chip = 0; chip < 8; chip++) {
    // Select chip via A0, A1, A2
    int target_a0 = (chip >> 0) & 1;
    int target_a1 = (chip >> 1) & 1;
    int target_a2 = (chip >> 2) & 1;

    gpio_set_level(PIN_DECODER_A0, target_a0);
    gpio_set_level(PIN_DECODER_A1, target_a1);
    gpio_set_level(PIN_DECODER_A2, target_a2);
    ets_delay_us(10);

    // Read back address pins
    int rb_a0 = gpio_get_level(PIN_DECODER_A0);
    int rb_a1 = gpio_get_level(PIN_DECODER_A1);
    int rb_a2 = gpio_get_level(PIN_DECODER_A2);

    // Idle state: SCLK low, MOSI low
    gpio_set_level(PIN_SPI_SCLK, 0);
    gpio_set_level(PIN_SPI_MOSI, 0);
    ets_delay_us(10);

    // Enable decoder (CS active-low)
    gpio_set_level(PIN_DECODER_E3, 1);
    ets_delay_us(10);
    int rb_e3 = gpio_get_level(PIN_DECODER_E3);

    // Send Write Command for SPR (register 7) channel A: 0x38 (0011 1000)
    uint8_t write_cmd = 0x38;
    for (int i = 7; i >= 0; i--) {
      gpio_set_level(PIN_SPI_MOSI, (write_cmd >> i) & 1);
      ets_delay_us(20);
      gpio_set_level(PIN_SPI_SCLK, 1); // Rising edge
      ets_delay_us(20);
      gpio_set_level(PIN_SPI_SCLK, 0); // Falling edge
    }

    // Send Data byte: 0x55 (0101 0101)
    uint8_t write_val = 0x55;
    for (int i = 7; i >= 0; i--) {
      gpio_set_level(PIN_SPI_MOSI, (write_val >> i) & 1);
      ets_delay_us(20);
      gpio_set_level(PIN_SPI_SCLK, 1); // Rising edge
      ets_delay_us(20);
      gpio_set_level(PIN_SPI_SCLK, 0); // Falling edge
    }

    // Disable decoder (CS goes high, latching the write)
    gpio_set_level(PIN_DECODER_E3, 0);
    ets_delay_us(100);

    // ---- STEP B: READ BACK FROM SPR REGISTER ----
    // Enable decoder (CS goes low)
    gpio_set_level(PIN_DECODER_E3, 1);
    ets_delay_us(10);

    // Send Read Command for SPR (register 7) channel A: 0xB8 (1011 1000)
    uint8_t read_cmd = 0xB8;
    for (int i = 7; i >= 0; i--) {
      gpio_set_level(PIN_SPI_MOSI, (read_cmd >> i) & 1);
      ets_delay_us(20);
      gpio_set_level(PIN_SPI_SCLK, 1); // Rising edge
      ets_delay_us(20);
      gpio_set_level(PIN_SPI_SCLK, 0); // Falling edge
    }

    // Read back register data while sending dummy bits (MOSI = 0)
    uint8_t read_val = 0;
    for (int i = 7; i >= 0; i--) {
      gpio_set_level(PIN_SPI_MOSI, 0);
      ets_delay_us(20);
      gpio_set_level(PIN_SPI_SCLK, 1); // Rising edge
      ets_delay_us(10);
      int bit = gpio_get_level(PIN_SPI_MISO);
      read_val |= (bit << i);
      ets_delay_us(10);
      gpio_set_level(PIN_SPI_SCLK, 0); // Falling edge
    }

    // Disable decoder (CS goes high)
    gpio_set_level(PIN_DECODER_E3, 0);
    ets_delay_us(10);

    ESP_LOGI(TAG,
             "Address %d [Set A2-A0: %d%d%d, Readback: %d%d%d | E3 Readback: "
             "%d] -> Read SPR: 0x%02X",
             chip, target_a2, target_a1, target_a0, rb_a2, rb_a1, rb_a0, rb_e3,
             read_val);
  }
  ESP_LOGI(TAG, "-------------------------------------------");

  // 5. Test W5500 Version Register (address 0x0039, control 0x00)
  ESP_LOGI(TAG, "--- Testing W5500 SPI version register read ---");

  // Release W5500 from reset (active-low reset)
  gpio_set_level(PIN_W5500_RST, 1);
  ets_delay_us(50000); // Wait 50ms for W5500 to boot

  // Ensure SC16 array is deselected
  gpio_set_level(PIN_DECODER_E3, 0);
  gpio_set_level(PIN_MISO_OE, 0); // MISO buffer enabled
  ets_delay_us(10);

  // Select W5500 (active-low CS)
  gpio_set_level(PIN_W5500_CS, 0);
  ets_delay_us(10);

  // W5500 Read Command: Address High (0x00), Address Low (0x39), Control (0x00
  // for Read Common Register)
  uint8_t w5500_cmd[] = {0x00, 0x39, 0x00};
  for (int b = 0; b < 3; b++) {
    uint8_t val = w5500_cmd[b];
    for (int i = 7; i >= 0; i--) {
      gpio_set_level(PIN_SPI_MOSI, (val >> i) & 1);
      ets_delay_us(20);
      gpio_set_level(PIN_SPI_SCLK, 1);
      ets_delay_us(20);
      gpio_set_level(PIN_SPI_SCLK, 0);
    }
  }

  // Read 1 byte of version data
  uint8_t version_val = 0;
  for (int i = 7; i >= 0; i--) {
    gpio_set_level(PIN_SPI_MOSI, 0);
    ets_delay_us(20);
    gpio_set_level(PIN_SPI_SCLK, 1);
    ets_delay_us(10);
    int bit = gpio_get_level(PIN_SPI_MISO);
    version_val |= (bit << i);
    ets_delay_us(10);
    gpio_set_level(PIN_SPI_SCLK, 0);
  }

  // Deselect W5500
  gpio_set_level(PIN_W5500_CS, 1);
  ets_delay_us(10);

  ESP_LOGI(TAG, "W5500 VERSIONR (0x0039) Readback: 0x%02X (expected 0x04)",
           version_val);
  ESP_LOGI(TAG, "------------------------------------------------");
  ESP_LOGI(TAG, "-------------------------------------------");
}

void app_main(void) {
  ESP_LOGI(TAG, "================================================");
  ESP_LOGI(TAG, "TEA@CPP Escape Room 2026: The Holy Grail");
  ESP_LOGI(TAG, "Custom Aggregator Board Firmware Booting...");
  ESP_LOGI(TAG, "================================================");

  // 1. Run boot-time bit-bang SPI self-test diagnostic
  bitbang_spi_read_spr();

  // 2. Initialize all custom PCB components for normal operation
  init_board_hardware();

  // 3. Spawn high-speed radar polling thread
  xTaskCreatePinnedToCore(radar_aggregator_task, "radar_aggregator", 4096, NULL,
                          5, NULL, 0);

  // 4. Spawn periodic network publisher thread
  xTaskCreatePinnedToCore(telemetry_publish_task, "telemetry_publish", 4096,
                          NULL, 4, NULL, 1);
}

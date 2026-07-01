/**
 * @file sc16is752.c
 * @brief SPI driver implementation for the SC16IS752 Dual UART-to-SPI bridge
 * controller.
 */

#include "sc16is752.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "SC16IS752";

/* SPI device handle */
static spi_device_handle_t s_spi_handle = NULL;
static sc16is752_hw_config_t s_hw_config;
static SemaphoreHandle_t s_spi_mutex = NULL;
static bool s_initialized = false;

esp_err_t sc16is752_init(const sc16is752_hw_config_t *config) {
  if (s_initialized) {
    ESP_LOGW(TAG, "SC16IS752 driver already initialized.");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Initializing SC16IS752 driver array...");
  memcpy(&s_hw_config, config, sizeof(sc16is752_hw_config_t));

  // Create the mutex for thread-safe shared SPI bus access
  s_spi_mutex = xSemaphoreCreateMutex();
  if (s_spi_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create SPI mutex!");
    return ESP_ERR_NO_MEM;
  }

  // 1. Configure the GPIOs for CD74HC138 addressing and enables
  gpio_config_t io_conf = {
      .intr_type = GPIO_INTR_DISABLE,
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask =
          ((1ULL << s_hw_config.pin_a0) | (1ULL << s_hw_config.pin_a1) |
           (1ULL << s_hw_config.pin_a2) | (1ULL << s_hw_config.pin_miso_oe)),
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .pull_up_en = GPIO_PULLUP_DISABLE};
  ESP_ERROR_CHECK(gpio_config(&io_conf));

  // Keep CD74HC138 disabled (E3 LOW) and MISO buffer enabled (OE LOW) by
  // default (allows W5500 MISO communication)
  // Note: pin_e3 (GPIO33) is managed automatically by the SPI master peripheral below.
  gpio_set_level(s_hw_config.pin_miso_oe, 0);
  gpio_set_level(s_hw_config.pin_a0, 0);
  gpio_set_level(s_hw_config.pin_a1, 0);
  gpio_set_level(s_hw_config.pin_a2, 0);

  // 2. Add the SPI device to the shared SPI bus (CS managed by hardware SPI)
  spi_device_interface_config_t devcfg = {
      .clock_speed_hz =
          1 * 1000 *
          1000,           // 1 MHz SPI clock for signal integrity margin
      .mode = 0,          // SPI Mode 0
      .spics_io_num = s_hw_config.pin_e3, // Hardware SPI master controls decoder enable E3!
      .queue_size = 1,
      .flags = SPI_DEVICE_POSITIVE_CS | SPI_DEVICE_NO_DUMMY // E3 is Active-High Enable
  };

  esp_err_t ret =
      spi_bus_add_device(s_hw_config.spi_host, &devcfg, &s_spi_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add SC16IS752 to SPI bus: %s",
             esp_err_to_name(ret));
    vQueueDelete(s_spi_mutex);
    return ret;
  }

  s_initialized = true;
  ESP_LOGI(TAG, "SC16IS752 multiplexer network configured successfully.");
  return ESP_OK;
}

esp_err_t sc16is752_select_chip(uint8_t chip_idx) {
  if (chip_idx >= 6) {
    return ESP_ERR_INVALID_ARG;
  }

  // Set A0, A1, A2 address bits
  gpio_set_level(s_hw_config.pin_a0, (chip_idx >> 0) & 0x01);
  gpio_set_level(s_hw_config.pin_a1, (chip_idx >> 1) & 0x01);
  gpio_set_level(s_hw_config.pin_a2, (chip_idx >> 2) & 0x01);

  // Enable MISO buffer (OE LOW)
  gpio_set_level(s_hw_config.pin_miso_oe, 0);

  return ESP_OK;
}

esp_err_t sc16is752_deselect_all(void) {
  // Keep MISO buffer enabled (OE LOW) for W5500 compatibility
  gpio_set_level(s_hw_config.pin_miso_oe, 0);

  return ESP_OK;
}

esp_err_t sc16is752_read_reg(uint8_t chip_idx, sc16is752_channel_t channel,
                             uint8_t reg_offset, uint8_t *out_val) {
  if (!s_initialized || s_spi_handle == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  // SPI address byte construction
  // Read flag (bit 7) = 1, Register offset (bits 6..3), Channel offset
  // (bits 2..1)
  uint8_t cmd_byte =
      0x80 | ((reg_offset & 0x0F) << 3) | ((channel & 0x03) << 1);

  uint8_t tx_data[2] = {cmd_byte, 0x00};
  uint8_t rx_data[2] = {0x00, 0x00};

  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  t.length = 16; // 16 bits (2 bytes)
  t.tx_buffer = tx_data;
  t.rx_buffer = rx_data;

  // Mutex locking for shared bus safety
  xSemaphoreTake(s_spi_mutex, portMAX_DELAY);

  // Hardware selection & bus setup
  sc16is752_select_chip(chip_idx);

  esp_err_t ret = spi_device_polling_transmit(s_spi_handle, &t);

  // Hardware deselect & isolation
  sc16is752_deselect_all();

  xSemaphoreGive(s_spi_mutex);

  if (ret == ESP_OK) {
    *out_val =
        rx_data[1]; // The second byte shifted out contains the register data
  }
  return ret;
}

esp_err_t sc16is752_write_reg(uint8_t chip_idx, sc16is752_channel_t channel,
                              uint8_t reg_offset, uint8_t val) {
  if (!s_initialized || s_spi_handle == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  // Write flag (bit 7) = 0, Register offset (bits 6..3), Channel offset
  // (bits 2..1)
  uint8_t cmd_byte = ((reg_offset & 0x0F) << 3) | ((channel & 0x03) << 1);

  uint8_t tx_data[2] = {cmd_byte, val};

  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  t.length = 16; // 16 bits
  t.tx_buffer = tx_data;

  xSemaphoreTake(s_spi_mutex, portMAX_DELAY);

  sc16is752_select_chip(chip_idx);

  esp_err_t ret = spi_device_polling_transmit(s_spi_handle, &t);

  sc16is752_deselect_all();

  xSemaphoreGive(s_spi_mutex);

  return ret;
}

esp_err_t sc16is752_configure_uart(uint8_t chip_idx,
                                   sc16is752_channel_t channel,
                                   uint32_t baudrate) {
  ESP_LOGI(TAG, "Configuring UART on Chip %d, Channel %s at %lu baud...",
           chip_idx, channel == SC16IS752_CHAN_A ? "A" : "B", baudrate);

  // 1. Enable register latch (DLAB) by writing to LCR[7]
  uint8_t lcr_val = 0;
  ESP_ERROR_CHECK(
      sc16is752_read_reg(chip_idx, channel, SC16IS752_REG_LCR, &lcr_val));
  lcr_val |= SC16IS752_LCR_DLAB;
  ESP_ERROR_CHECK(
      sc16is752_write_reg(chip_idx, channel, SC16IS752_REG_LCR, lcr_val));

  // 2. Calculate integer divisor
  // Baud Rate = XTAL_FREQ / (16 * Divisor) -> Divisor = XTAL_FREQ / (16 * Baud
  // Rate) Prescaler defaults to 1 (divisor divided by 1)
  uint32_t divisor = SC16IS752_XTAL_FREQ / (16 * baudrate);
  uint8_t dll_val = divisor & 0xFF;
  uint8_t dlh_val = (divisor >> 8) & 0xFF;

  ESP_ERROR_CHECK(
      sc16is752_write_reg(chip_idx, channel, SC16IS752_REG_DLL, dll_val));
  ESP_ERROR_CHECK(
      sc16is752_write_reg(chip_idx, channel, SC16IS752_REG_DLH, dlh_val));

  // 3. Disable DLAB and set UART framing (8N1: 8 data bits, no parity, 1 stop
  // bit) 8 data bits: LCR[1..0] = 11 (0x03) 1 stop bit: LCR[2] = 0 No parity:
  // LCR[5..3] = 000
  lcr_val = 0x03; // DLAB = 0, 8N1
  ESP_ERROR_CHECK(
      sc16is752_write_reg(chip_idx, channel, SC16IS752_REG_LCR, lcr_val));

  // 4. Enable and reset FIFOs, set trigger level to 8 bytes (so we get fewer
  // interrupts) FCR[0]: Enable FIFO = 1 FCR[1]: Reset RX FIFO = 1 FCR[2]: Reset
  // TX FIFO = 1 FCR[7..6]: RX trigger level = 01 (8 bytes)
  uint8_t fcr_val = 0x01 | 0x02 | 0x04 | (0x01 << 6);
  ESP_ERROR_CHECK(
      sc16is752_write_reg(chip_idx, channel, SC16IS752_REG_FCR, fcr_val));

  // 5. Enable RX interrupts (trigger when data is available or timeout)
  // IER[0]: Enable RX data available interrupt
  uint8_t ier_val = 0x01;
  ESP_ERROR_CHECK(
      sc16is752_write_reg(chip_idx, channel, SC16IS752_REG_IER, ier_val));

  ESP_LOGD(TAG,
           "UART successfully configured on Chip %d Channel %d (Divisor: %ld)",
           chip_idx, channel, divisor);
  return ESP_OK;
}

int sc16is752_write_bytes(uint8_t chip_idx, sc16is752_channel_t channel,
                          const uint8_t *data, size_t len) {
  size_t bytes_written = 0;
  uint8_t tx_level = 0;

  while (bytes_written < len) {
    // Read how much space is left in TX FIFO
    if (sc16is752_read_reg(chip_idx, channel, SC16IS752_REG_TXLVL, &tx_level) !=
        ESP_OK) {
      break;
    }

    if (tx_level == 0) {
      // TX FIFO is full, yield for a tiny bit
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    size_t write_chunk = len - bytes_written;
    if (write_chunk > tx_level) {
      write_chunk = tx_level;
    }

    for (size_t i = 0; i < write_chunk; i++) {
      if (sc16is752_write_reg(chip_idx, channel, SC16IS752_REG_THR,
                              data[bytes_written + i]) != ESP_OK) {
        break;
      }
    }
    bytes_written += write_chunk;
  }
  return bytes_written;
}

uint8_t sc16is752_get_rx_level(uint8_t chip_idx, sc16is752_channel_t channel) {
  uint8_t rx_level = 0;
  if (sc16is752_read_reg(chip_idx, channel, SC16IS752_REG_RXLVL, &rx_level) !=
      ESP_OK) {
    return 0;
  }
  return rx_level;
}

int sc16is752_read_bytes(uint8_t chip_idx, sc16is752_channel_t channel,
                         uint8_t *buffer, size_t max_len) {
  size_t bytes_read = 0;
  uint8_t rx_level = sc16is752_get_rx_level(chip_idx, channel);

  if (rx_level == 0) {
    return 0;
  }

  size_t read_chunk = max_len;
  if (read_chunk > rx_level) {
    read_chunk = rx_level;
  }

  for (size_t i = 0; i < read_chunk; i++) {
    uint8_t byte = 0;
    if (sc16is752_read_reg(chip_idx, channel, SC16IS752_REG_RHR, &byte) ==
        ESP_OK) {
      buffer[i] = byte;
      bytes_read++;
    } else {
      break;
    }
  }
  return bytes_read;
}

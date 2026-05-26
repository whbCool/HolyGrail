/**
 * @file sc16is752.h
 * @brief SPI driver for the SC16IS752 Dual UART-to-SPI bridge controller on Holy Grail.
 */

#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief SC16IS752 registers and offsets.
 * 
 * Note: Under SPI mode, the command byte sent to the SC16IS752 is:
 * Bit 7: Read (1) or Write (0)
 * Bit 6..3: Register offset (0..15)
 * Bit 2..1: Channel selection (00 for A, 01 for B)
 * Bit 0: Reserved (0)
 */
#define SC16IS752_REG_RHR         0x00 /**< Receiver Holding Register (R, LCR[7]=0) */
#define SC16IS752_REG_THR         0x00 /**< Transmitter Holding Register (W, LCR[7]=0) */
#define SC16IS752_REG_DLL         0x00 /**< Divisor Latch LSB (RW, LCR[7]=1) */
#define SC16IS752_REG_IER         0x01 /**< Interrupt Enable Register (RW, LCR[7]=0) */
#define SC16IS752_REG_DLH         0x01 /**< Divisor Latch MSB (RW, LCR[7]=1) */
#define SC16IS752_REG_IIR         0x02 /**< Interrupt Identification Register (R) */
#define SC16IS752_REG_FCR         0x02 /**< FIFO Control Register (W) */
#define SC16IS752_REG_LCR         0x03 /**< Line Control Register (RW) */
#define SC16IS752_REG_MCR         0x04 /**< Modem Control Register (RW) */
#define SC16IS752_REG_LSR         0x05 /**< Line Status Register (R) */
#define SC16IS752_REG_MSR         0x06 /**< Modem Status Register (R) */
#define SC16IS752_REG_SPR         0x07 /**< Scratchpad Register (RW) */
#define SC16IS752_REG_TXLVL       0x08 /**< TX FIFO Level Register (R) */
#define SC16IS752_REG_RXLVL       0x09 /**< RX FIFO Level Register (R) */
#define SC16IS752_REG_IODIR       0x0A /**< I/O Direction Register (RW) */
#define SC16IS752_REG_IOSTATE     0x0B /**< I/O State Register (R) */
#define SC16IS752_REG_IOINTENA    0x0C /**< I/O Interrupt Enable Register (RW) */
#define SC16IS752_REG_IOCONTROL   0x0E /**< I/O Control Register (RW) */
#define SC16IS752_REG_EFCR        0x0F /**< Extra Features Control Register (RW) */

/* Channel selection bits (in the command byte) */
#define SC16IS752_CHANNEL_A       0x00
#define SC16IS752_CHANNEL_B       0x01

/* LCR DLAB Bit */
#define SC16IS752_LCR_DLAB        0x80

/* Reference crystal clock on Holy Grail PCB */
#define SC16IS752_XTAL_FREQ       14745600 /**< 14.7456 MHz */

/**
 * @brief SC16IS752 channel specifier.
 */
typedef enum {
    SC16IS752_CHAN_A = 0,
    SC16IS752_CHAN_B = 1,
    SC16IS752_CHAN_MAX
} sc16is752_channel_t;

/**
 * @brief Hardware configuration for the SC16IS752 SPI-multiplexed array.
 */
typedef struct {
    spi_host_device_t spi_host;      /**< SPI Host (e.g. SPI2_HOST / VSPI) */
    gpio_num_t pin_a0;               /**< CD74HC138 address line A0 */
    gpio_num_t pin_a1;               /**< CD74HC138 address line A1 */
    gpio_num_t pin_a2;               /**< CD74HC138 address line A2 */
    gpio_num_t pin_e3;               /**< CD74HC138 active-high enable line E3 */
    gpio_num_t pin_miso_oe;          /**< IC12 active-low MISO OE buffer enable (GPIO27) */
    gpio_num_t pin_irq;              /**< Shared falling-edge interrupt pin ESP_IRQ (GPIO4) */
} sc16is752_hw_config_t;

/**
 * @brief Initialize the SC16IS752 driver array.
 * 
 * Configures the GPIOs for CD74HC138 control, the MISO buffer isolation control,
 * and sets up the SPI bus parameters.
 * 
 * @param config Pointer to hardware configurations.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t sc16is752_init(const sc16is752_hw_config_t *config);

/**
 * @brief Set the address on CD74HC138 to select a specific bridge chip.
 * 
 * @param chip_idx The bridge chip index (0 to 5 for U1, U2, U4, U5, U6, U7).
 * @return ESP_OK on success, or an error code.
 */
esp_err_t sc16is752_select_chip(uint8_t chip_idx);

/**
 * @brief Deselect the current bridge chip, tri-stating the MISO line.
 * 
 * @return ESP_OK on success, or an error code.
 */
esp_err_t sc16is752_deselect_all(void);

/**
 * @brief Read a register from a specific chip and UART channel.
 * 
 * @param chip_idx The bridge chip index (0..5).
 * @param channel The UART channel (SC16IS752_CHAN_A or B).
 * @param reg_offset The register offset (SC16IS752_REG_*).
 * @param[out] out_val Pointer to store the read value.
 * @return ESP_OK on success.
 */
esp_err_t sc16is752_read_reg(uint8_t chip_idx, sc16is752_channel_t channel, uint8_t reg_offset, uint8_t *out_val);

/**
 * @brief Write a value to a register on a specific chip and UART channel.
 * 
 * @param chip_idx The bridge chip index (0..5).
 * @param channel The UART channel (SC16IS752_CHAN_A or B).
 * @param reg_offset The register offset (SC16IS752_REG_*).
 * @param val The value to write.
 * @return ESP_OK on success.
 */
esp_err_t sc16is752_write_reg(uint8_t chip_idx, sc16is752_channel_t channel, uint8_t reg_offset, uint8_t val);

/**
 * @brief Configure UART line format and baud rate for a specific channel.
 * 
 * Divides the reference crystal frequency (14.7456 MHz) to configure DLL/DLH.
 * 
 * @param chip_idx The bridge chip index (0..5).
 * @param channel The UART channel (SC16IS752_CHAN_A or B).
 * @param baudrate Target baud rate (e.g. 230400 or 115200).
 * @return ESP_OK on success.
 */
esp_err_t sc16is752_configure_uart(uint8_t chip_idx, sc16is752_channel_t channel, uint32_t baudrate);

/**
 * @brief Write bytes to the UART transmitter.
 * 
 * @param chip_idx The bridge chip index (0..5).
 * @param channel The UART channel (SC16IS752_CHAN_A or B).
 * @param data Buffer containing data to transmit.
 * @param len Length of the data buffer.
 * @return Number of bytes successfully written.
 */
int sc16is752_write_bytes(uint8_t chip_idx, sc16is752_channel_t channel, const uint8_t *data, size_t len);

/**
 * @brief Read bytes from the UART receiver.
 * 
 * @param chip_idx The bridge chip index (0..5).
 * @param channel The UART channel (SC16IS752_CHAN_A or B).
 * @param[out] buffer Buffer to store read data.
 * @param max_len Maximum bytes to read.
 * @return Number of bytes successfully read.
 */
int sc16is752_read_bytes(uint8_t chip_idx, sc16is752_channel_t channel, uint8_t *buffer, size_t max_len);

/**
 * @brief Get the number of bytes available in the RX FIFO.
 * 
 * @param chip_idx The bridge chip index (0..5).
 * @param channel The UART channel (SC16IS752_CHAN_A or B).
 * @return Number of bytes available.
 */
uint8_t sc16is752_get_rx_level(uint8_t chip_idx, sc16is752_channel_t channel);

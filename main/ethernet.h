/**
 * @file ethernet.h
 * @brief W5500 Ethernet controller driver for Holy Grail.
 */

#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

// Suggested Default Network Configurations
#define HOLYGRAIL_STATIC_IP        "192.168.1.150"
#define HOLYGRAIL_GATEWAY          "192.168.1.1"
#define HOLYGRAIL_NETMASK          "255.255.255.0"
#define HOLYGRAIL_DNS              "192.168.1.1"

/**
 * @brief Configuration structure for the Ethernet subsystem.
 */
typedef struct {
    spi_host_device_t spi_host;      /**< SPI host associated with W5500 */
    gpio_num_t pin_cs;               /**< Chip Select pin for W5500 (IO5 / ESP_SCSNT) */
    gpio_num_t pin_rst;              /**< Hardware reset pin for W5500 (IO24 / ESP_USRSTT) */
    gpio_num_t pin_intr;             /**< Interrupt pin for W5500 (not used in polling, set if needed) */
} ethernet_config_t;

/**
 * @brief Initialize W5500 SPI Ethernet and join the network.
 * 
 * Configures the W5500 MAC, PHY, registers with the ESP-NETIF TCPIP stack,
 * and sets up the static IP settings.
 * 
 * @param config Pointer to the ethernet configuration.
 * @return ESP_OK on success.
 */
esp_err_t ethernet_init(const ethernet_config_t *config);

/**
 * @brief Check if the Ethernet link is up and has an active IP address.
 * 
 * @return true if connected with an IP.
 */
bool ethernet_is_connected(void);

/**
 * @file ethernet.c
 * @brief W5500 Ethernet controller driver implementation.
 */

#include "ethernet.h"
#include "esp_eth.h" // IWYU pragma: keep
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/ip_addr.h"
#include <string.h>

static const char *TAG = "HolyGrail_Eth";
static bool s_connected = false;
static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth_handle = NULL;

/**
 * @brief Event handler for Ethernet events.
 */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
  switch (event_id) {
  case ETHERNET_EVENT_CONNECTED:
    ESP_LOGI(TAG, "Ethernet Link Up");
    break;
  case ETHERNET_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "Ethernet Link Down");
    s_connected = false;
    break;
  case ETHERNET_EVENT_START:
    ESP_LOGI(TAG, "Ethernet Driver Started");
    break;
  case ETHERNET_EVENT_STOP:
    ESP_LOGI(TAG, "Ethernet Driver Stopped");
    s_connected = false;
    break;
  default:
    break;
  }
}

/**
 * @brief Event handler for IP events.
 */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data) {
  ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
  const esp_netif_ip_info_t *ip_info = &event->ip_info;

  ESP_LOGI(TAG, "Ethernet Got IP Address");
  ESP_LOGI(TAG, "~~~~~~~~~~~ Network Parameters ~~~~~~~~~~~");
  ESP_LOGI(TAG, "  IP:      " IPSTR, IP2STR(&ip_info->ip));
  ESP_LOGI(TAG, "  Gateway: " IPSTR, IP2STR(&ip_info->gw));
  ESP_LOGI(TAG, "  Netmask: " IPSTR, IP2STR(&ip_info->netmask));
  ESP_LOGI(TAG, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");

  s_connected = true;
}

esp_err_t ethernet_init(const ethernet_config_t *config) {
  ESP_LOGI(TAG, "Initializing W5500 Ethernet Controller...");

  // 1. Initialize TCP/IP netif stack and event loop
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // 2. Create netif instance for Ethernet
  esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
  s_eth_netif = esp_netif_new(&cfg);
  if (s_eth_netif == NULL) {
    ESP_LOGE(TAG, "Failed to create esp_netif instance!");
    return ESP_FAIL;
  }

  // 3. Configure static IP address parameters
  // Stop DHCP client first to allow static IP assignment
  ESP_ERROR_CHECK(esp_netif_dhcpc_stop(s_eth_netif));

  esp_netif_ip_info_t ip_info;
  memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));
  ip_info.ip.addr = ipaddr_addr(HOLYGRAIL_STATIC_IP);
  ip_info.gw.addr = ipaddr_addr(HOLYGRAIL_GATEWAY);
  ip_info.netmask.addr = ipaddr_addr(HOLYGRAIL_NETMASK);

  ESP_ERROR_CHECK(esp_netif_set_ip_info(s_eth_netif, &ip_info));
  ESP_LOGI(TAG, "Static IP configured: %s", HOLYGRAIL_STATIC_IP);

  // Set static DNS configuration
  esp_netif_dns_info_t dns_info;
  dns_info.ip.u_addr.ip4.addr = ipaddr_addr(HOLYGRAIL_DNS);
  dns_info.ip.type = IPADDR_TYPE_V4;
  ESP_ERROR_CHECK(
      esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns_info));

  // 4. Register event handlers
  ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                             &eth_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                             &got_ip_event_handler, NULL));

  // 5. Initialize W5500 Mac/Phy over SPI
  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.phy_addr = 1; // Default PHY address for W5500 on most modules
  phy_config.reset_gpio_num = config->pin_rst;

  // Define W5500 SPI device parameters
  spi_device_interface_config_t spi_devcfg = {
      .command_bits = 16,
      .address_bits = 8,
      .mode = 0,
      .clock_speed_hz =
          16 * 1000 * 1000, // 16 MHz as recommended in block diagram
      .spics_io_num = config->pin_cs,
      .queue_size = 20};

  // Instantiate MAC and PHY handles
  eth_w5500_config_t w5500_config =
      ETH_W5500_DEFAULT_CONFIG(config->spi_host, &spi_devcfg);
  esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
  if (mac == NULL) {
    ESP_LOGE(TAG, "Failed to create W5500 MAC!");
    return ESP_FAIL;
  }

  esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
  if (phy == NULL) {
    ESP_LOGE(TAG, "Failed to create W5500 PHY!");
    mac->del(mac);
    return ESP_FAIL;
  }

  // 6. Install the driver
  esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
  esp_err_t ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to install Ethernet driver: %s",
             esp_err_to_name(ret));
    phy->del(phy);
    mac->del(mac);
    return ret;
  }

  // 7. Attach Ethernet driver to TCPIP netif instance
  ESP_ERROR_CHECK(
      esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle)));

  // 8. Start Ethernet driver
  ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

  ESP_LOGI(TAG, "Ethernet interface successfully initialized.");
  return ESP_OK;
}

bool ethernet_is_connected(void) { return s_connected; }

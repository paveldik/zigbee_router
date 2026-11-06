#include <Arduino.h>
#include <esp_eth.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <freertos/semphr.h>

static esp_eth_mac_t *mac = nullptr;
static esp_eth_handle_t handle = nullptr;
static int selectedAddress = -1;
static bool macReady = false;
static bool driverReady = false;
static bool verboseIo = true;
static uint32_t lastDump = 0;
static SemaphoreHandle_t mdioMutex = nullptr;

static esp_err_t readRegister(uint32_t address, uint32_t reg, uint32_t *value)
{
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(mdioMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        result = mac->read_phy_reg(mac, address, reg, value);
        xSemaphoreGive(mdioMutex);
    }
    if (verboseIo || result != ESP_OK) {
        if (result == ESP_OK) {
            Serial.printf("[PHY-R] addr=%u reg=%u value=0x%04lx\n",
                          address, reg, static_cast<unsigned long>(*value));
        } else {
            Serial.printf("[PHY-R] addr=%u reg=%u ERROR=%s\n",
                          address, reg, esp_err_to_name(result));
        }
    }
    return result;
}

static esp_err_t traceRead(esp_eth_handle_t, uint32_t address, uint32_t reg, uint32_t *value)
{
    return readRegister(address, reg, value);
}

static esp_err_t traceWrite(esp_eth_handle_t, uint32_t address, uint32_t reg, uint32_t value)
{
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(mdioMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        result = mac->write_phy_reg(mac, address, reg, value);
        xSemaphoreGive(mdioMutex);
    }
    Serial.printf("[PHY-W] addr=%u reg=%u value=0x%04lx result=%s\n",
                  address, reg, static_cast<unsigned long>(value), esp_err_to_name(result));
    return result;
}

static esp_err_t scanPhy(esp_eth_handle_t)
{
    macReady = true;
    delay(20);
    Serial.println("[DIAG] MAC initialized. Reading IDs at PHY addresses 0 and 1.");
    for (uint32_t address = 0; address <= 1; ++address) {
        uint32_t id1 = 0, id2 = 0;
        esp_err_t r1 = readRegister(address, 2, &id1);
        esp_err_t r2 = readRegister(address, 3, &id2);
        if (r1 == ESP_OK && r2 == ESP_OK && id1 == 0x0007 && (id2 & 0xfff0) == 0xc0f0) {
            if (selectedAddress == -1) {
                selectedAddress = static_cast<int>(address);
            }
            Serial.printf("[DIAG] LAN8720-family PHY responds at address %u\n", address);
        } else {
            Serial.printf("[DIAG] No matching LAN8720 ID at address %u\n", address);
        }
    }
    return ESP_OK;
}

static void dumpRegisters()
{
    if (!macReady) {
        Serial.println("[DIAG] MAC initialization failed; register access unavailable.");
        return;
    }
    const uint32_t address = selectedAddress >= 0 ? selectedAddress : 0;
    Serial.printf("\n[DIAG] t=%lu ms addr=%u GPIO5=%d driver=%s\n",
                  static_cast<unsigned long>(millis()), address, digitalRead(5),
                  driverReady ? "started" : "FAILED");
    const uint32_t registers[] = {0, 1, 2, 3, 4, 5, 6, 17, 18, 27, 29, 30, 31};
    for (uint32_t reg : registers) {
        uint32_t value = 0;
        // BMSR link status is latched low; read twice for current status.
        if (reg == 1) {
            uint32_t latched = 0;
            if (readRegister(address, reg, &latched) != ESP_OK) {
                continue;
            }
            Serial.printf("[REG] BMSR first=0x%04lx\n", static_cast<unsigned long>(latched));
        }
        if (readRegister(address, reg, &value) != ESP_OK) {
            continue;
        }
        Serial.printf("[REG] %02u=0x%04lx", reg, static_cast<unsigned long>(value));
        if (reg == 0) {
            Serial.printf(" RESET=%u LOOPBACK=%u AN_ENABLE=%u POWER_DOWN=%u ISOLATE=%u",
                          (value >> 15) & 1, (value >> 14) & 1, (value >> 12) & 1,
                          (value >> 11) & 1, (value >> 10) & 1);
        } else if (reg == 1) {
            Serial.printf(" LINK=%u AN_COMPLETE=%u REMOTE_FAULT=%u",
                          (value >> 2) & 1, (value >> 5) & 1, (value >> 4) & 1);
        } else if (reg == 18) {
            Serial.printf(" PHYAD=%u MODE=%u", value & 0x1f, (value >> 5) & 7);
        }
        Serial.println();
    }
}

static void ethernetEvent(void *, esp_event_base_t, int32_t eventId, void *)
{
    const char *name = "OTHER";
    switch (eventId) {
        case ETHERNET_EVENT_START: name = "START"; break;
        case ETHERNET_EVENT_STOP: name = "STOP"; break;
        case ETHERNET_EVENT_CONNECTED: name = "LINK_UP"; break;
        case ETHERNET_EVENT_DISCONNECTED: name = "LINK_DOWN"; break;
    }
    Serial.printf("[ETH-EVENT] t=%lu %s\n", static_cast<unsigned long>(millis()), name);
}

void setup()
{
    // Route the board's USB UART to ESP32, leaving Zigbee untouched.
    pinMode(33, OUTPUT);
    digitalWrite(33, LOW);
    Serial.begin(115200);
    delay(1500);
    Serial.println("\nHMG LAN8720 DIAGNOSTIC v1");
    Serial.printf("[DIAG] reset_reason=%d SDK=%s\n", esp_reset_reason(), esp_get_idf_version());
    Serial.println("[DIAG] MDC=23 MDIO=18 CLK=GPIO17_OUT RESET_GPIO=5");
    Serial.println("[DIAG] No web UI/DHCP. No NVS writes, no Zigbee flashing.");
    Serial.println("[DIAG] Testing PHY link only. Register dumps every 5 seconds.");
    esp_log_level_set("*", ESP_LOG_DEBUG);
    mdioMutex = xSemaphoreCreateMutex();
    if (!mdioMutex) {
        Serial.println("[DIAG] ERROR: MDIO mutex allocation failed");
        return;
    }

    esp_err_t result = esp_netif_init();
    Serial.printf("[DIAG] esp_netif_init: %s\n", esp_err_to_name(result));
    if (result != ESP_OK) {
        return;
    }
    result = esp_event_loop_create_default();
    Serial.printf("[DIAG] event loop: %s\n", esp_err_to_name(result));
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return;
    }
    result = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, ethernetEvent, nullptr);
    Serial.printf("[DIAG] event handler: %s\n", esp_err_to_name(result));
    if (result != ESP_OK) {
        return;
    }
    eth_mac_config_t macConfig = ETH_MAC_DEFAULT_CONFIG();
    macConfig.smi_mdc_gpio_num = 23;
    macConfig.smi_mdio_gpio_num = 18;
    macConfig.sw_reset_timeout_ms = 1000;
    macConfig.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    macConfig.clock_config.rmii.clock_gpio = EMAC_CLK_OUT_180_GPIO;
    mac = esp_eth_mac_new_esp32(&macConfig);
    if (!mac) {
        Serial.println("[DIAG] ERROR: MAC allocation failed");
        return;
    }
    eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
    phyConfig.phy_addr = ESP_ETH_PHY_ADDR_AUTO;
    phyConfig.reset_gpio_num = 5;
    esp_eth_phy_t *phy = esp_eth_phy_new_lan8720(&phyConfig);
    if (!phy) {
        Serial.println("[DIAG] ERROR: PHY allocation failed");
        return;
    }
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    config.on_lowlevel_init_done = scanPhy;
    config.read_phy_reg = traceRead;
    config.write_phy_reg = traceWrite;
    result = esp_eth_driver_install(&config, &handle);
    Serial.printf("[DIAG] esp_eth_driver_install: %s\n", esp_err_to_name(result));
    if (result != ESP_OK) {
        // On failure the mediator is freed by IDF. Only direct MAC reads are used below.
        verboseIo = false;
        dumpRegisters();
        return;
    }
    uint32_t address = 0;
    result = esp_eth_ioctl(handle, ETH_CMD_G_PHY_ADDR, &address);
    Serial.printf("[DIAG] detected address: %lu result=%s\n",
                  static_cast<unsigned long>(address), esp_err_to_name(result));
    if (result == ESP_OK) {
        selectedAddress = static_cast<int>(address);
    }
    result = esp_eth_start(handle);
    Serial.printf("[DIAG] esp_eth_start: %s\n", esp_err_to_name(result));
    driverReady = result == ESP_OK;
    verboseIo = false;
    dumpRegisters();
}

void loop()
{
    if (millis() - lastDump >= 5000) {
        lastDump = millis();
        dumpRegisters();
    }
    delay(20);
}

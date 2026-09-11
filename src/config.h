#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <ctype.h>

// Version
#define FIRMWARE_VERSION "1.0.0"

// EEPROM/NVS Settings
#define CONFIG_NAMESPACE "meshcore_mqtt"
#define CONFIG_MAGIC 0x4D435147  // "MCQG" - MeshCore MQTT Gateway

// Default LoRa Settings
#define DEFAULT_LORA_FREQ 470.0f
#define DEFAULT_LORA_BW 250.0f
#define DEFAULT_LORA_SF 11
#define DEFAULT_LORA_CR 5
#define DEFAULT_LORA_TX_POWER 30

// Default MQTT Settings
#define DEFAULT_MQTT_SERVER "mqtt.example.com"
#define DEFAULT_MQTT_PORT 8883
#define DEFAULT_MQTT_USER ""
#define DEFAULT_MQTT_PASSWORD ""
#define DEFAULT_MQTT_CLIENT_ID "meshcore_gateway"
#define DEFAULT_MQTT_TOPIC_PREFIX "MESHCORE"

// WiFi Settings
#define DEFAULT_WIFI_SSID "your_wifi_ssid"
#define DEFAULT_WIFI_PASSWORD "your_wifi_password"

// Pin definitions for ESP32-S3 with E22-400M30S (SX1268)
#define LORA_SCK 10
#define LORA_MISO 12
#define LORA_MOSI 11
#define LORA_CS 9
#define LORA_RST 13
#define LORA_DIO0 8
#define LORA_DIO1 8
#define LORA_BUSY 14

// LED Pin definitions
#define LED_LINK_PIN  4   // Green  - WiFi/MQTT link status
#define LED_DATA_PIN  5   // Yellow - Data activity
#define LED_PWR_PIN   6   // Red    - Power / system status

// RS485 Pin definitions (SP3485EEN)
#define RS485_TX_PIN  43   // DI  - MCU TX → RS485 DI
#define RS485_RX_PIN  44  // RO  - RS485 RO → MCU RX
#define RS485_DE_PIN  2   // DE  - Driver Enable  (active HIGH)
#define RS485_RE_PIN  1   // RE  - Receiver Enable (active LOW)

// Default RS485 Settings
#define DEFAULT_RS485_BAUD 9600

// Configuration structures
struct WiFiConfig {
    char ssid[64];
    char password[64];
    bool enabled;
};

struct MQTTConfig {
    char server[128];
    uint16_t port;
    char username[64];
    char password[64];
    char clientId[64];
    char topicPrefix[64];     // effective prefix used for publish/subscribe
    char basePrefix[64];      // e.g. "meshcore"
    char country[32];         // e.g. "Australia"
    char region[32];          // e.g. "NSW" (optional)
    bool useTLS;
    bool insecureTLS;       // Allow TLS without certificate validation (setInsecure)
    bool enabled;
    bool publishRaw;         // Publish raw hex data
    bool publishDecoded;     // Publish decoded messages
    bool subscribeCommands;  // Subscribe to command topics
    bool bridgeAll;          // Subscribe to raw/messages for RF rebroadcast
    bool useCustomCA;        // Use a user-provided CA certificate
    char caCert[2048];       // PEM-encoded CA certificate (optional)
};

struct LoRaConfig {
    float frequency;
    float bandwidth;
    uint8_t spreadingFactor;
    uint8_t codingRate;
    uint8_t txPower;
    uint8_t syncWord;
    bool enableCRC;
};

struct RepeaterConfig {
    char nodeName[32];
    uint32_t nodeId;
    uint8_t maxHops;
    bool autoAck;
    bool broadcastEnabled;
    uint16_t routeTimeout;  // seconds
};

struct SecurityConfig {
    char guestPassword[32];
    char adminPassword[32];
};

// Access control for MeshCore nodes
struct AccessControlConfig {
    bool denyEnabled;            // Enable denylist enforcement
    uint8_t denyCount;           // Number of entries in denylist
    uint32_t denylist[16];       // List of blocked node IDs (hex IDs)
};

struct DiscoveryConfig {
    bool advertEnabled;
    uint16_t advertIntervalSec; // seconds
};

struct LocationConfig {
    float latitude;   // degrees
    float longitude;  // degrees
};

struct ClockConfig {
    char ntpServer[64];
    int16_t timezoneMinutes; // minutes offset from UTC
    bool autoSync;           // sync at boot when WiFi is up
};

struct RS485Config {
    uint32_t baudRate;
    bool enabled;
    bool bridgeToLora;
    bool bridgeToMqtt;
    bool bridgeFromLora;
    bool modbusEnabled;
    uint16_t modbusPollIntervalMs;
};

struct ModbusPollEntry {
    uint8_t slaveId;
    uint8_t functionCode;
    uint16_t startReg;
    uint16_t regCount;
    char label[32];
};

static const size_t MODBUS_MAX_POLLS = 8;

// Neighbor tracking for discovery
struct NeighborInfo {
    uint32_t nodeId;
    char nodeName[32];
    int lastRssi;
    float lastSnr;
    float latitude;
    float longitude;
    unsigned long lastSeenMs;
};

struct GatewayConfig {
    uint32_t magic;
    WiFiConfig wifi;
    MQTTConfig mqtt;
    LoRaConfig lora;
    RepeaterConfig repeater;
    SecurityConfig security;
    AccessControlConfig access;
    DiscoveryConfig discovery;
    LocationConfig location;
    ClockConfig clock;
    RS485Config rs485;
    ModbusPollEntry modbusPolls[MODBUS_MAX_POLLS];
    uint8_t modbusPollCount;
};

// Derive a safe MQTT Client ID from the repeater node name
inline void deriveClientIdFromNodeName(const char* nodeName, char* out, size_t outSize) {
    if (outSize == 0) return;
    if (!nodeName) nodeName = "";
    size_t j = 0;
    for (size_t i = 0; nodeName[i] != '\0' && j < outSize - 1; ++i) {
        char c = nodeName[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_') {
            out[j++] = c;
        } else if (c == ' ' || c == '.' || c == ':' || c == '/' || c == '\\') {
            out[j++] = '_';
        } else {
            // skip any other characters
        }
    }
    if (j == 0) {
        const char* fallback = "MQTT-Gateway";
        for (size_t i = 0; fallback[i] != '\0' && j < outSize - 1; ++i) {
            out[j++] = fallback[i];
        }
    }
    out[j] = '\0';
}

// Build hierarchical topic prefix from base/country/region into out
// Normalizes all segments to uppercase and strips spaces for global consistency
inline void deriveTopicPrefix(const MQTTConfig& mqtt, char* out, size_t outSize) {
    if (outSize == 0) return;
    out[0] = '\0';

    auto appendUpperSegment = [&](const char* seg) {
        if (!seg || seg[0] == '\0') return;
        // Prepare uppercase sanitized copy of the segment
        char upper[64];
        size_t j = 0;
        for (size_t i = 0; seg[i] != '\0' && j < sizeof(upper) - 1; ++i) {
            char c = seg[i];
            if (c == ' ') continue; // drop spaces
            // Allow A-Z, a-z, 0-9, '-', '_'
            if ((c >= 'a' && c <= 'z')) c = (char)toupper((unsigned char)c);
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
                upper[j++] = c;
            }
        }
        upper[j] = '\0';
        if (upper[0] == '\0') return;
        if (out[0] != '\0') strncat(out, "/", outSize - strlen(out) - 1);
        strncat(out, upper, outSize - strlen(out) - 1);
    };

    const char* base = (mqtt.basePrefix[0] != '\0') ? mqtt.basePrefix : DEFAULT_MQTT_TOPIC_PREFIX;
    appendUpperSegment(base);
    appendUpperSegment(mqtt.country);   // Expect ISO2 (e.g., AU, NZ)
    appendUpperSegment(mqtt.region);    // Expect subdivision code part (e.g., NSW, AUK)
}

// Default configuration
inline GatewayConfig getDefaultConfig() {
    GatewayConfig config;
    config.magic = CONFIG_MAGIC;
    
    // WiFi defaults
    strncpy(config.wifi.ssid, DEFAULT_WIFI_SSID, sizeof(config.wifi.ssid));
    strncpy(config.wifi.password, DEFAULT_WIFI_PASSWORD, sizeof(config.wifi.password));
    config.wifi.enabled = true;
    
    // MQTT defaults
    strncpy(config.mqtt.server, DEFAULT_MQTT_SERVER, sizeof(config.mqtt.server));
    config.mqtt.port = DEFAULT_MQTT_PORT;
    strncpy(config.mqtt.username, DEFAULT_MQTT_USER, sizeof(config.mqtt.username));
    strncpy(config.mqtt.password, DEFAULT_MQTT_PASSWORD, sizeof(config.mqtt.password));
    strncpy(config.mqtt.clientId, DEFAULT_MQTT_CLIENT_ID, sizeof(config.mqtt.clientId));
    strncpy(config.mqtt.basePrefix, DEFAULT_MQTT_TOPIC_PREFIX, sizeof(config.mqtt.basePrefix));
    config.mqtt.country[0] = '\0';
    config.mqtt.region[0] = '\0';
    deriveTopicPrefix(config.mqtt, config.mqtt.topicPrefix, sizeof(config.mqtt.topicPrefix));
    config.mqtt.useTLS = true;
    config.mqtt.insecureTLS = false;
    config.mqtt.enabled = true;
    config.mqtt.publishRaw = true;
    config.mqtt.publishDecoded = true;
    config.mqtt.subscribeCommands = true;
    config.mqtt.bridgeAll = true;
    config.mqtt.useCustomCA = false;
    config.mqtt.caCert[0] = '\0';
    
    // LoRa defaults
    config.lora.frequency = DEFAULT_LORA_FREQ;
    config.lora.bandwidth = DEFAULT_LORA_BW;
    config.lora.spreadingFactor = DEFAULT_LORA_SF;
    config.lora.codingRate = DEFAULT_LORA_CR;
    config.lora.txPower = DEFAULT_LORA_TX_POWER;
    config.lora.syncWord = 0x12;  // MeshCore default (RADIOLIB_SX126X_SYNC_WORD_PRIVATE)
    config.lora.enableCRC = true;
    
    // Repeater defaults
    strncpy(config.repeater.nodeName, "MQTT-Gateway", sizeof(config.repeater.nodeName));
    config.repeater.nodeId = 0;  // Will be auto-generated
    config.repeater.maxHops = 3;
    config.repeater.autoAck = true;
    config.repeater.broadcastEnabled = true;
    config.repeater.routeTimeout = 300;

    // Security defaults
    config.security.guestPassword[0] = '\0';
    config.security.adminPassword[0] = '\0';

    // Access control defaults
    config.access.denyEnabled = false;
    config.access.denyCount = 0;
    for (size_t i = 0; i < (sizeof(config.access.denylist)/sizeof(config.access.denylist[0])); ++i) {
        config.access.denylist[i] = 0;
    }

    // Discovery defaults
    config.discovery.advertEnabled = false;
    config.discovery.advertIntervalSec = 300; // 5 minutes

    // Location defaults
    config.location.latitude = 0.0f;
    config.location.longitude = 0.0f;

    // Clock defaults
    strncpy(config.clock.ntpServer, "pool.ntp.org", sizeof(config.clock.ntpServer));
    config.clock.timezoneMinutes = 0; // UTC
    config.clock.autoSync = true;

    // RS485 defaults
    config.rs485.baudRate = DEFAULT_RS485_BAUD;
    config.rs485.enabled = true;
    config.rs485.bridgeToLora = false;
    config.rs485.bridgeToMqtt = true;
    config.rs485.bridgeFromLora = false;
    config.rs485.modbusEnabled = false;
    config.rs485.modbusPollIntervalMs = 5000;

    // Modbus poll entries defaults
    config.modbusPollCount = 0;
    for (size_t i = 0; i < MODBUS_MAX_POLLS; ++i)
    {
        config.modbusPolls[i].slaveId = 0;
        config.modbusPolls[i].functionCode = 0x03;
        config.modbusPolls[i].startReg = 0;
        config.modbusPolls[i].regCount = 1;
        config.modbusPolls[i].label[0] = '\0';
    }
    
    return config;
}

#endif // CONFIG_H
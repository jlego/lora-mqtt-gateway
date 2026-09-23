/**
 * MeshCore MQTT Gateway
 *
 * This firmware is a LoRa MQTT gateway with serial configuration.
 * It bridges LoRa mesh messages to MQTT brokers.
 *
 * Features:
 * - LoRa packet repeater functionality
 * - MQTT message bridging (LoRa <-> MQTT)
 * - Serial configuration interface
 * - WiFi connectivity
 * - Persistent configuration storage
 *
 * Version: 1.0.0
 */

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <string.h>

// Configuration and handlers
#include "config.h"
#include "settings_manager.h"
#include "mqtt_handler.h"
#include "serial_config.h"
#include "rs485_handler.h"
#include "modbus_handler.h"

// LoRa radio object - SX1268 for E22-400M30S
// Use FSPI (SPI2_HOST) dedicated for LoRa
static SPIClass spi(FSPI);
SX1268 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, spi, SPISettings());

// UI helpers for consistent boxed output
static const int BOX_CONTENT_WIDTH = 54; // width between bars, excluding leading/trailing single spaces

static void printBoxLine(const String &content)
{
    int pad = BOX_CONTENT_WIDTH - (int)content.length();
    if (pad < 0)
        pad = 0; // truncate visually if too long
    Serial.print("│ ");
    Serial.print(content);
    for (int i = 0; i < pad; ++i)
        Serial.print(' ');
    Serial.println(" │");
}

static void printBoxKeyValue(const char *key, const String &value, int keyWidth = 14)
{
    String line = String(key);
    while ((int)line.length() < keyWidth)
        line += ' ';
    line += value;
    printBoxLine(line);
}

// Global objects
GatewayConfig config;
SettingsManager settingsManager;
MQTTHandler *mqttHandler = nullptr;
ConfigMenu *serialConfig = nullptr;
RS485Handler *rs485Handler = nullptr;
ModbusHandler *modbusHandler = nullptr;

// Statistics
uint32_t packetsReceived = 0;
uint32_t packetsSent = 0;
uint32_t packetsForwarded = 0;
uint32_t packetsFailed = 0;
uint32_t rs485PacketsRx = 0;
uint32_t rs485PacketsTx = 0;
uint32_t modbusPollCount = 0;
uint32_t modbusErrorCount = 0;

// Timing
unsigned long lastStatsPublish = 0;
unsigned long lastStatusBlink = 0;
unsigned long lastPacketCheck = 0;
bool configMode = false;

// Radio state
bool radioInitialized = false;
volatile bool packetReceived = false;

// LED state
static unsigned long dataLedOffAt = 0;
static const unsigned long DATA_LED_FLASH_MS = 80;
static bool linkLedState = false;
static unsigned long linkLedToggleAt = 0;

// Discovery / Neighbour tracking
static NeighborInfo neighbors[16];
static size_t neighborCount = 0;
static unsigned long lastAdvertSent = 0;

// Simple recent-packet deduplication to prevent rapid re-repeat loops
static const size_t RECENT_PACKET_SLOTS = 8;
struct RecentPacketEntry
{
    uint32_t hash;
    unsigned long timestampMs;
};
static RecentPacketEntry recentPackets[RECENT_PACKET_SLOTS] = {};

static uint32_t fnv1aHash32(const uint8_t *data, size_t length)
{
    uint32_t hash = 2166136261u; // FNV offset basis
    for (size_t i = 0; i < length; ++i)
    {
        hash ^= data[i];
        hash *= 16777619u; // FNV prime
    }
    return hash;
}

static bool wasPacketSeenRecently(uint32_t hash, unsigned long nowMs, unsigned long windowMs)
{
    for (size_t i = 0; i < RECENT_PACKET_SLOTS; ++i)
    {
        if (recentPackets[i].hash == hash)
        {
            if (nowMs - recentPackets[i].timestampMs <= windowMs)
            {
                return true;
            }
        }
    }
    return false;
}

static void rememberPacket(uint32_t hash, unsigned long nowMs)
{
    // Insert/overwrite the oldest slot
    size_t oldest = 0;
    unsigned long oldestTs = recentPackets[0].timestampMs;
    for (size_t i = 1; i < RECENT_PACKET_SLOTS; ++i)
    {
        if (recentPackets[i].timestampMs < oldestTs)
        {
            oldestTs = recentPackets[i].timestampMs;
            oldest = i;
        }
    }
    recentPackets[oldest].hash = hash;
    recentPackets[oldest].timestampMs = nowMs;
}

// Function declarations
void setupLoRa();
void handleLoRaReceive();
void handleLoRaPacket(uint8_t *data, size_t length, int rssi, float snr);
bool sendLoRaPacket(const uint8_t *data, size_t length);
void checkSerialInput();
void publishStats();
void publishNeighbours();
void blinkLED();
void flashDataLED();
void setRadioFlag();
void handleRS485Receive();
void exitConfigMode();
void sendAdvert();
void printTelemetryToSerial();
void printNeighboursToSerial();

// Radio interrupt flag
volatile uint32_t interruptCount = 0;
void IRAM_ATTR setRadioFlag()
{
    packetReceived = true;
    interruptCount++;
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    // Initialize LED pins
    pinMode(LED_LINK_PIN, OUTPUT);
    pinMode(LED_DATA_PIN, OUTPUT);
    pinMode(LED_PWR_PIN, OUTPUT);
    digitalWrite(LED_LINK_PIN, LOW);
    digitalWrite(LED_DATA_PIN, LOW);
    digitalWrite(LED_PWR_PIN, HIGH);

    // Initialize settings manager
    if (!settingsManager.begin())
    {
        Serial.println(F("✗ Failed to initialize settings manager"));
    }

    // Load configuration or use defaults
    if (!settingsManager.loadConfig(config))
    {
        Serial.println(F("⚠ No saved configuration found, using defaults"));
        config = getDefaultConfig();
        settingsManager.saveConfig(config);
    }
    else
    {
        Serial.println(F("✓ Configuration loaded"));
    }

    // Generate node ID if not set
    if (config.repeater.nodeId == 0)
    {
        uint64_t chipid = ESP.getEfuseMac();
        config.repeater.nodeId = (uint32_t)(chipid & 0xFFFFFFFF);
        settingsManager.saveConfig(config);
        Serial.printf("✓ Generated Node ID: 0x%08X\n", config.repeater.nodeId);
    }

    Serial.println();

    // Sync MQTT Client ID with repeater node name
    {
        char prevId[sizeof(config.mqtt.clientId)];
        strncpy(prevId, config.mqtt.clientId, sizeof(prevId));
        prevId[sizeof(prevId) - 1] = '\0';
        deriveClientIdFromNodeName(config.repeater.nodeName, config.mqtt.clientId, sizeof(config.mqtt.clientId));
        if (strcmp(prevId, config.mqtt.clientId) != 0)
        {
            settingsManager.saveConfig(config);
            Serial.printf("✓ MQTT Client ID set to: %s\n", config.mqtt.clientId);
        }
    }

    Serial.println(F("┌────────────────────────────────────────────────────────┐"));
    printBoxLine(String("Node Name: ") + config.repeater.nodeName);
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%08X", config.repeater.nodeId);
        printBoxLine(String("Node ID:   ") + buf);
    }
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.3f MHz", config.lora.frequency);
        printBoxLine(String("LoRa Freq: ") + buf);
    }
    printBoxLine(String("WiFi:      ") + (config.wifi.enabled ? "Enabled" : "Disabled"));
    printBoxLine(String("MQTT:      ") + (config.mqtt.enabled ? "Enabled" : "Disabled"));
    printBoxLine(String("RS485:     ") + (config.rs485.enabled ? "Enabled" : "Disabled"));
    printBoxLine(String("Modbus:    ") + (config.rs485.modbusEnabled ? "Enabled" : "Disabled"));
    Serial.println(F("└────────────────────────────────────────────────────────┘"));
    Serial.println();

    // Setup LoRa
    Serial.println(F("Initializing LoRa..."));
    setupLoRa();
    Serial.println(F("✓ LoRa initialized"));

    // Setup MQTT if enabled
    if (config.wifi.enabled && config.mqtt.enabled)
    {
        Serial.println(F("\nInitializing MQTT..."));
        mqttHandler = new MQTTHandler(config);

        // Set callback for MQTT -> LoRa messages
        mqttHandler->setMessageCallback([](const uint8_t *payload, size_t length)
                                        {
            Serial.printf("Forwarding MQTT message to LoRa (%d bytes)\n", length);
            flashDataLED();
            sendLoRaPacket(payload, length); });

        if (mqttHandler->begin())
        {
            Serial.println(F("✓ MQTT initialized"));
            mqttHandler->publishGatewayStatus(true);
        }
        else
        {
            Serial.println(F("✗ MQTT initialization failed"));
        }
    }
    else
    {
        Serial.println(F("⚠ MQTT disabled (WiFi or MQTT not enabled in config)"));
    }

    // Setup RS485 if enabled
    if (config.rs485.enabled)
    {
        if (config.rs485.modbusEnabled)
        {
            Serial.println(F("\nInitializing RS485 (Modbus RTU Master)..."));
            modbusHandler = new ModbusHandler(config);
            if (modbusHandler->begin())
            {
                Serial.println(F("✓ Modbus RTU initialized"));
            }
            else
            {
                Serial.println(F("✗ Modbus RTU initialization failed"));
                delete modbusHandler;
                modbusHandler = nullptr;
            }
        }
        else
        {
            Serial.println(F("\nInitializing RS485 (raw bridge)..."));
            rs485Handler = new RS485Handler(config);
            if (rs485Handler->begin())
            {
                Serial.println(F("✓ RS485 initialized"));
            }
            else
            {
                Serial.println(F("✗ RS485 initialization failed"));
                delete rs485Handler;
                rs485Handler = nullptr;
            }
        }
    }
    else
    {
        Serial.println(F("⚠ RS485 disabled"));
    }

    // Setup serial configuration interface
    serialConfig = new ConfigMenu(config, settingsManager);
    serialConfig->setOnExitCallback(exitConfigMode);
    serialConfig->begin();

    Serial.println();
    Serial.println(F("════════════════════════════════════════════════════════"));
    Serial.println(F("✓ Gateway started successfully!"));
    Serial.println(F("════════════════════════════════════════════════════════"));
    Serial.println();
    Serial.println(F("Commands:"));
    Serial.println(F("  'c' - Enter configuration menu"));
    Serial.println(F("  's' - Show statistics"));
    Serial.println(F("  'n' - Show neighbours"));
    Serial.println(F("  'd' - Debug info (interrupt count)"));
    Serial.println(F("  't' - Send test packet (TX test)"));
    Serial.println(F("  'r' - Restart device"));
    Serial.println();
    Serial.println(F("(Hint) Press 'c' at any time to open the configuration menu"));
}

void loop()
{
    // Handle LoRa messages
    if (!configMode)
    {
        handleLoRaReceive();

        // Handle MQTT
        if (mqttHandler)
        {
            mqttHandler->loop();
        }

        // Handle RS485 / Modbus
        if (modbusHandler)
        {
            modbusHandler->loop();
            if (modbusHandler->hasResult() && mqttHandler && mqttHandler->isConnected())
            {
                String json = modbusHandler->consumeResult();
                char topic[128];
                snprintf(topic, sizeof(topic), "%s/modbus", config.mqtt.topicPrefix);
                mqttHandler->getMqttClient().publish(topic, json.c_str(), false);
                modbusPollCount++;
                flashDataLED();
            }
        }
        else if (rs485Handler)
        {
            rs485Handler->loop();
            handleRS485Receive();
        }
    }

    // Check for serial commands
    if (!configMode)
    {
        checkSerialInput();
    }
    else
    {
        serialConfig->handleMenu();
    }

    // Publish statistics periodically
    unsigned long now = millis();
    if (!configMode && mqttHandler && mqttHandler->isConnected() && now - lastStatsPublish > 60000)
    {
        publishStats();
        publishNeighbours(); // Also publish neighbor list with stats
        lastStatsPublish = now;
    }

    // Periodic advert broadcast
    if (!configMode && config.discovery.advertEnabled && now - lastAdvertSent > (unsigned long)config.discovery.advertIntervalSec * 1000UL)
    {
        sendAdvert();
        lastAdvertSent = now;
    }

    // Blink status LED
    if (now - lastStatusBlink > 1000)
    {
        blinkLED();
        lastStatusBlink = now;
    }

    yield();
}

void setupLoRa()
{
    // Hardware reset of LoRa module
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);
    delay(10);
    digitalWrite(LORA_RST, HIGH);
    delay(150);

    // Initialize SPI on FSPI bus
    spi.begin(LORA_SCK, LORA_MISO, LORA_MOSI);
    delay(100);

    Serial.print(F("Initializing radio... "));

    // Initialize radio with configuration
    // SX1268.begin(freq, bw, sf, cr, syncWord, power, preambleLength, tcxoVoltage, useRegulatorLDO)
    // E22-400M30S uses TCXO at 1.8V on DIO3
    // Start with 22 dBm (SX1268 max), then configure PA for 30 dBm after begin
    int state;
    int8_t initPower = (config.lora.txPower > 22) ? 22 : config.lora.txPower;
    state = radio.begin(
        config.lora.frequency,
        config.lora.bandwidth,
        config.lora.spreadingFactor,
        config.lora.codingRate,
        config.lora.syncWord,
        initPower,
        8,    // preamble length
        1.8,  // TCXO voltage (1.8V for E22-400M30S)
        false // use DC-DC regulator
    );

    if (state == RADIOLIB_ERR_NONE)
    {
        Serial.println(F("success!"));

        // Configure SX1268 for E22-400M30S module
        // DIO2 as RF switch (required for E22 modules)
        radio.setDio2AsRfSwitch(true);

        // Set current limit for external PA (240mA for E22-400M30S)
        radio.setCurrentLimit(240);

        // For E22-400M30S external PA: configure PA for high power output
        if (config.lora.txPower > 22)
        {
            Serial.printf("Configuring external PA for %d dBm... ", config.lora.txPower);
            state = radio.setPaConfig(0x04, RADIOLIB_SX126X_PA_CONFIG_SX1268, 0x07);
            if (state == RADIOLIB_ERR_NONE)
            {
                state = radio.setTxParams(config.lora.txPower, RADIOLIB_SX126X_PA_RAMP_200U);
            }
            if (state == RADIOLIB_ERR_NONE)
            {
                Serial.println(F("OK"));
            }
            else
            {
                Serial.printf("FAILED (%d)\n", state);
            }
        }

        // Enable CRC if configured
        if (config.lora.enableCRC)
        {
            radio.setCRC(true);
        }

// CRITICAL: For LilyGo boards, explicitly set output power and PA config
// This ensures the PA (Power Amplifier) is actually enabled
        // CRITICAL: Set packet received action (RadioLib's proper method)
        Serial.print(F("Setting packet received action... "));
        radio.setPacketReceivedAction(setRadioFlag);
        Serial.println(F("OK"));

        // Start continuous listening
        state = radio.startReceive();
        if (state == RADIOLIB_ERR_NONE)
        {
            Serial.println(F("✓ Radio listening for packets"));
            radioInitialized = true;
        }
        else
        {
            Serial.print(F("✗ Failed to start receive, code: "));
            Serial.println(state);
        }
    }
    else
    {
        Serial.print(F("failed, code: "));
        Serial.println(state);
        Serial.println(F("✗ Check wiring and antenna!"));
    }

    // Print radio configuration
    Serial.println(F("\n┌── LoRa Configuration ──────────────────────────────────┐"));
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.3f MHz", config.lora.frequency);
        printBoxKeyValue("Frequency:", buf, 16);
    }
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f kHz", config.lora.bandwidth);
        printBoxKeyValue("Bandwidth:", buf, 16);
    }
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", config.lora.spreadingFactor);
        printBoxKeyValue("Spreading Factor:", buf, 18);
    }
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "4/%d", config.lora.codingRate);
        printBoxKeyValue("Coding Rate:", buf, 16);
    }
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d dBm", config.lora.txPower);
        printBoxKeyValue("TX Power:", buf, 16);
    }
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%02X", config.lora.syncWord);
        printBoxKeyValue("Sync Word:", buf, 16);
    }
    printBoxKeyValue("CRC:", String(config.lora.enableCRC ? "Enabled" : "Disabled"), 16);
    Serial.println(F("└────────────────────────────────────────────────────────┘\n"));
}

void handleLoRaReceive()
{
    if (!radioInitialized)
        return;

    // Check if packet was received
    if (packetReceived)
    {
        packetReceived = false;
        Serial.println(F("🔔 Interrupt fired! Reading packet..."));

        // Buffer for received data
        uint8_t buffer[256];

        // Read received data
        int state = radio.readData(buffer, sizeof(buffer));

        if (state == RADIOLIB_ERR_NONE)
        {
            // Get packet info
            size_t length = radio.getPacketLength();
            int rssi = radio.getRSSI();
            float snr = radio.getSNR();

            Serial.printf("📥 RX SUCCESS: %d bytes, RSSI=%d dBm, SNR=%.1f dB\n", length, rssi, snr);
            packetsReceived++;
            flashDataLED();

            // Handle the packet
            handleLoRaPacket(buffer, length, rssi, snr);
        }
        else if (state == RADIOLIB_ERR_CRC_MISMATCH)
        {
            Serial.println(F("⚠ CRC error!"));
        }
        else
        {
            Serial.printf("⚠ Read error, code: %d\n", state);
        }

        // ✅ CRITICAL FIX: Put radio back into receive mode
        state = radio.startReceive();
        if (state != RADIOLIB_ERR_NONE)
        {
            Serial.print(F("✗ Failed to restart receive, code: "));
            Serial.println(state);
            radioInitialized = false;
        }
    }
}

void handleLoRaPacket(uint8_t *data, size_t length, int rssi, float snr)
{
    // Log to serial
    Serial.printf("\n📡 LoRa RX: %d bytes | RSSI: %d dBm | SNR: %.1f dB\n", length, rssi, snr);

    // Print hex dump (first 32 bytes)
    Serial.print("   Data: ");
    for (size_t i = 0; i < min(length, (size_t)32); i++)
    {
        Serial.printf("%02X ", data[i]);
    }
    if (length > 32)
        Serial.print("...");
    Serial.println();

    // Try to interpret as text if printable
    bool isPrintable = true;
    // Track parsed ADVERT details if present so we can publish origin and metadata
    bool parsedAdvert = false;
    uint32_t advertNodeId = 0;
    char advertName[32] = {0};
    float advertLat = 0.0f;
    float advertLon = 0.0f;
    for (size_t i = 0; i < length; i++)
    {
        if (data[i] < 32 || data[i] > 126)
        {
            isPrintable = false;
            break;
        }
    }

    if (isPrintable && length > 0)
    {
        Serial.print("   Text: \"");
        for (size_t i = 0; i < length; i++)
        {
            Serial.write(data[i]);
        }
        Serial.println("\"");

        // Simple neighbour discovery on ADVERT messages: "ADVERT <nodeIdHex> <nodeName> <lat> <lon>"
        if (length >= 6 && strncmp((const char *)data, "ADVERT", 6) == 0)
        {
            char buf[256];
            size_t copyLen = min(length, sizeof(buf) - 1);
            memcpy(buf, data, copyLen);
            buf[copyLen] = '\0';
            char *saveptr;
            char *tok = strtok_r(buf, " ", &saveptr); // ADVERT
            tok = strtok_r(nullptr, " ", &saveptr);   // nodeIdHex
            uint32_t nid = 0;
            if (tok)
            {
                nid = (uint32_t)strtoul(tok, nullptr, 16);
            }
            tok = strtok_r(nullptr, " ", &saveptr); // nodeName
            char nname[32] = {0};
            if (tok)
            {
                strncpy(nname, tok, sizeof(nname) - 1);
            }
            tok = strtok_r(nullptr, " ", &saveptr); // lat
            float lat = tok ? atof(tok) : 0.0f;
            tok = strtok_r(nullptr, " ", &saveptr); // lon
            float lon = tok ? atof(tok) : 0.0f;

            // Apply access control denylist: drop adverts from denied node IDs
            bool denied = false;
            if (config.access.denyEnabled)
            {
                for (uint8_t i = 0; i < config.access.denyCount && i < (sizeof(config.access.denylist) / sizeof(config.access.denylist[0])); ++i)
                {
                    if (config.access.denylist[i] == nid && nid != 0)
                    {
                        denied = true;
                        break;
                    }
                }
            }

            if (denied)
            {
                Serial.println(F("   ✗ Advert dropped (denied node)"));
                // Skip neighbor update and further processing for denied node
                return;
            }

            // Remember parsed advert for later MQTT publication/decoded origin
            parsedAdvert = true;
            advertNodeId = nid;
            strncpy(advertName, nname, sizeof(advertName) - 1);
            advertLat = lat;
            advertLon = lon;

            size_t idx = neighborCount;
            for (size_t i = 0; i < neighborCount; ++i)
            {
                if (neighbors[i].nodeId == nid)
                {
                    idx = i;
                    break;
                }
            }
            if (idx == neighborCount && neighborCount < (sizeof(neighbors) / sizeof(neighbors[0])))
            {
                neighborCount++;
            }
            if (idx < (sizeof(neighbors) / sizeof(neighbors[0])))
            {
                neighbors[idx].nodeId = nid;
                strncpy(neighbors[idx].nodeName, nname, sizeof(neighbors[idx].nodeName) - 1);
                neighbors[idx].nodeName[sizeof(neighbors[idx].nodeName) - 1] = '\0';
                neighbors[idx].lastRssi = rssi;
                neighbors[idx].lastSnr = snr;
                neighbors[idx].latitude = lat;
                neighbors[idx].longitude = lon;
                neighbors[idx].lastSeenMs = millis();
                Serial.println(F("   ✓ Neighbour updated from advert"));
            }
        }
    }

    // Forward to MQTT if connected
    if (mqttHandler && mqttHandler->isConnected())
    {
        // If this was an ADVERT received over RF, publish a structured advert event
        if (parsedAdvert)
        {
            mqttHandler->publishAdvert(advertNodeId, advertName, advertLat, advertLon);
        }
        // Publish raw packet
        if (config.mqtt.publishRaw)
        {
            mqttHandler->publishRawPacket(data, length, rssi, snr);
        }

        // Publish decoded message if it looks like text
        if (config.mqtt.publishDecoded && isPrintable)
        {
            char message[256] = {0};
            size_t msgLen = min(length, sizeof(message) - 1);
            memcpy(message, data, msgLen);

            // For ADVERT messages, set origin to the advertising node; otherwise use gateway id
            uint32_t fromId = parsedAdvert && advertNodeId != 0 ? advertNodeId : config.repeater.nodeId;
            mqttHandler->publishDecodedMessage(
                fromId,
                0xFFFFFFFF, // to (broadcast)
                message,
                0, // message type
                rssi,
                snr,
                0 // hop count
            );
        }

        packetsForwarded++;
    }

    // Forward to RS485 if enabled and bridging from LoRa
    if (rs485Handler && config.rs485.bridgeFromLora)
    {
        rs485Handler->send(data, length);
        rs485PacketsTx++;
        Serial.printf("   → Forwarded to RS485 (%d bytes)\n", length);
    }

    // Optional: Repeat packet if configured as repeater
    // This is a simple repeater - just retransmit what we receive
    // In a real mesh implementation, you'd check hop count, routing, etc.
    if (config.repeater.maxHops > 0 && length > 0)
    {
        unsigned long nowMs = millis();
        uint32_t h = fnv1aHash32(data, length);
        if (!wasPacketSeenRecently(h, nowMs, 2000UL))
        {
            // Simple delay to avoid collisions
            delay(random(100, 300));
            // Retransmit
            if (sendLoRaPacket(data, length))
            {
                Serial.println("   ↻ Packet repeated");
                rememberPacket(h, millis());
            }
        }
        else
        {
            Serial.println("   ↻ Skipped repeat (duplicate seen recently)");
        }
    }
}

bool sendLoRaPacket(const uint8_t *data, size_t length)
{
    if (!radioInitialized || length == 0 || length > 255)
    {
        packetsFailed++;
        return false;
    }

    Serial.printf("\n📤 LoRa TX: %d bytes\n", length);

    // Transmit the packet
    int state = radio.transmit((uint8_t *)data, length);

    if (state == RADIOLIB_ERR_NONE)
    {
        packetsSent++;
        flashDataLED();
        Serial.println("   ✓ Sent successfully");

        // Put radio back into receive mode
        radio.startReceive();
        return true;
    }
    else
    {
        packetsFailed++;
        Serial.print("   ✗ Failed, code: ");
        Serial.println(state);

        // Try to recover
        radio.startReceive();
        return false;
    }
}

void checkSerialInput()
{
    if (Serial.available())
    {
        char c = Serial.read();

        switch (c)
        {
        case 'c':
        case 'C':
            configMode = true;
            if (mqttHandler) mqttHandler->suspend();
            serialConfig->showMainMenu();
            break;

        case 's':
        case 'S':
            Serial.println(F("\n┌── Gateway Statistics ──────────────────────────────────┐"));
            printTelemetryToSerial();
            Serial.println(F("└────────────────────────────────────────────────────────┘\n"));
            break;

        case 'n':
        case 'N':
            Serial.println(F("\n┌── Neighbours ──────────────────────────────────────────┐"));
            printNeighboursToSerial();
            Serial.println(F("└────────────────────────────────────────────────────────┘\n"));
            break;

        case 'r':
        case 'R':
            Serial.println(F("\n⚠ Restarting device..."));
            delay(1000);
            ESP.restart();
            break;

        case 'd':
        case 'D':
            Serial.println(F("\n┌── DEBUG INFO ──────────────────────────────────────────┐"));
            Serial.printf("│ Radio Interrupts:    %u\n", interruptCount);
            Serial.printf("│ Packets Received:    %u\n", packetsReceived);
            Serial.printf("│ Packets Sent:        %u\n", packetsSent);
            Serial.printf("│ Packets Forwarded:   %u\n", packetsForwarded);
            Serial.printf("│ Packets Failed:      %u\n", packetsFailed);
            Serial.printf("│ Radio Initialized:   %s\n", radioInitialized ? "YES" : "NO");
            Serial.printf("│ Packet Flag:         %s\n", packetReceived ? "SET" : "CLEAR");
            // Check radio status
            if (radioInitialized)
            {
                Serial.println(F("│"));
                Serial.print(F("│ Radio status check...  "));
                int state = radio.startReceive();
                if (state == RADIOLIB_ERR_NONE)
                {
                    Serial.println(F("RX ACTIVE"));
                    // Try to read RSSI to verify radio is listening
                    int rssi = radio.getRSSI();
                    Serial.printf("│ Current RSSI:         %d dBm\n", rssi);
                }
                else
                {
                    Serial.printf("RX FAILED (code: %d)\n", state);
                }
            }
            Serial.println(F("└────────────────────────────────────────────────────────┘\n"));
            break;

        case 't':
        case 'T':
            Serial.println(F("\n📡 Sending test packet..."));
            {
                uint8_t testPacket[] = "TEST_GATEWAY_TX";
                if (sendLoRaPacket(testPacket, sizeof(testPacket)))
                {
                    Serial.println(F("✓ Test packet transmitted successfully!"));
                    Serial.println(F("  (Your other radio should receive this if in range)"));
                }
                else
                {
                    Serial.println(F("✗ Test packet transmission failed!"));
                }
            }
            break;
        }
    }
}

void publishStats()
{
    if (mqttHandler)
    {
        mqttHandler->publishStats(packetsReceived, packetsSent, packetsForwarded, packetsFailed);
    }
}

void publishNeighbours()
{
    if (mqttHandler)
    {
        mqttHandler->publishNeighbors(neighbors, neighborCount);
    }
}

void handleRS485Receive()
{
    if (!rs485Handler || !rs485Handler->hasData())
        return;

    uint8_t buf[256];
    size_t len = rs485Handler->readData(buf, sizeof(buf));
    if (len == 0)
        return;

    rs485PacketsRx++;
    flashDataLED();

    Serial.printf("\n🔌 RS485 RX: %d bytes\n", len);
    Serial.print("   Data: ");
    for (size_t i = 0; i < min(len, (size_t)32); i++)
    {
        Serial.printf("%02X ", buf[i]);
    }
    if (len > 32)
        Serial.print("...");
    Serial.println();

    // Bridge RS485 → LoRa
    if (config.rs485.bridgeToLora)
    {
        if (sendLoRaPacket(buf, len))
        {
            Serial.println("   → Forwarded to LoRa");
        }
    }

    // Bridge RS485 → MQTT
    if (config.rs485.bridgeToMqtt && mqttHandler && mqttHandler->isConnected())
    {
        if (config.mqtt.publishRaw)
        {
            mqttHandler->publishRawPacket(buf, len, 0, 0.0f);
        }
        Serial.println("   → Forwarded to MQTT");
    }
}

void flashDataLED()
{
    digitalWrite(LED_DATA_PIN, HIGH);
    dataLedOffAt = millis() + DATA_LED_FLASH_MS;
}

void blinkLED()
{
    unsigned long now = millis();

    // --- PWR LED (Red) ---
    // Solid ON = normal; fast blink = error (radio not initialized)
    if (radioInitialized)
    {
        digitalWrite(LED_PWR_PIN, HIGH);
    }
    else
    {
        static unsigned long pwrToggleAt = 0;
        if (now - pwrToggleAt > 200)
        {
            pwrToggleAt = now;
            digitalWrite(LED_PWR_PIN, !digitalRead(LED_PWR_PIN));
        }
    }

    // --- LINK LED (Green) ---
    if (!config.wifi.enabled)
    {
        digitalWrite(LED_LINK_PIN, LOW);
    }
    else if (WiFi.status() != WL_CONNECTED)
    {
        // WiFi connecting: fast blink 200ms
        if (now - linkLedToggleAt > 200)
        {
            linkLedToggleAt = now;
            linkLedState = !linkLedState;
            digitalWrite(LED_LINK_PIN, linkLedState ? HIGH : LOW);
        }
    }
    else if (mqttHandler && mqttHandler->isConnected())
    {
        // WiFi + MQTT connected: solid ON
        digitalWrite(LED_LINK_PIN, HIGH);
    }
    else
    {
        // WiFi connected but MQTT not: slow blink 1000ms
        if (now - linkLedToggleAt > 1000)
        {
            linkLedToggleAt = now;
            linkLedState = !linkLedState;
            digitalWrite(LED_LINK_PIN, linkLedState ? HIGH : LOW);
        }
    }

    // --- DATA LED (Yellow) ---
    // Auto-off after flash duration; flash is triggered by flashDataLED()
    if (dataLedOffAt > 0 && now >= dataLedOffAt)
    {
        dataLedOffAt = 0;
        digitalWrite(LED_DATA_PIN, LOW);
    }
}

void sendAdvert()
{
    // Compose a simple advert string: ADVERT <nodeIdHex> <nodeName> <lat> <lon>
    char payload[160];
    snprintf(payload, sizeof(payload), "ADVERT %08X %s %.6f %.6f",
             config.repeater.nodeId,
             config.repeater.nodeName,
             (double)config.location.latitude,
             (double)config.location.longitude);
    sendLoRaPacket((const uint8_t *)payload, strlen(payload));
    // Also publish an advert event on MQTT for visibility if connected
    if (mqttHandler && mqttHandler->isConnected())
    {
        mqttHandler->publishAdvert(
            config.repeater.nodeId,
            config.repeater.nodeName,
            config.location.latitude,
            config.location.longitude);
    }
}

void printTelemetryToSerial()
{
    Serial.printf("Uptime:           %-36lu \n", millis() / 1000);
    Serial.printf("Packets Received: %-36lu \n", packetsReceived);
    Serial.printf("Packets Sent:     %-36lu \n", packetsSent);
    Serial.printf("Packets Forwarded:%-36lu \n", packetsForwarded);
    Serial.printf("Packets Failed:   %-36lu \n", packetsFailed);
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.printf("WiFi RSSI:        %-36d \n", WiFi.RSSI());
        Serial.printf("IP Address:       %-36s \n", WiFi.localIP().toString().c_str());
    }
    if (rs485Handler)
    {
        Serial.printf("RS485 RX:         %-36lu \n", rs485PacketsRx);
        Serial.printf("RS485 TX:         %-36lu \n", rs485PacketsTx);
    }
    if (modbusHandler)
    {
        Serial.printf("Modbus Polls OK:  %-36lu \n", modbusPollCount);
        Serial.printf("Modbus Errors:    %-36lu \n", modbusErrorCount);
    }
}

void printNeighboursToSerial()
{
    if (neighborCount == 0)
    {
        Serial.println(F("(none)"));
        return;
    }
    unsigned long now = millis();
    for (size_t i = 0; i < neighborCount; ++i)
    {
        unsigned long age = (now - neighbors[i].lastSeenMs) / 1000UL;
        Serial.printf("ID: 0x%08X  Name: %-16s  RSSI: %4d  SNR: %5.1f  Age: %lus  Lat: %.5f  Lon: %.5f\n",
                      neighbors[i].nodeId,
                      neighbors[i].nodeName,
                      neighbors[i].lastRssi,
                      neighbors[i].lastSnr,
                      age,
                      (double)neighbors[i].latitude,
                      (double)neighbors[i].longitude);
    }
}

// Exit configuration mode helper
void exitConfigMode()
{
    configMode = false;
    if (mqttHandler) mqttHandler->resume();
    Serial.println(F("\n✓ Exited configuration mode"));

    // Restart if configuration changed significantly
    Serial.println(F("⚠ Some changes may require a restart"));
    Serial.println(F("(Hint) Live view resumed. Press 'c' to return to the menu"));
}
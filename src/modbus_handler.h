#ifndef MODBUS_HANDLER_H
#define MODBUS_HANDLER_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include "config.h"

class ModbusHandler {
public:
    ModbusHandler(GatewayConfig &cfg)
        : config(cfg), serialPort(Serial1), pollIndex(0), lastPollMs(0),
          requestLen(0), responseLen(0) {}

    bool begin()
    {
        if (!config.rs485.enabled || !config.rs485.modbusEnabled)
            return false;

        pinMode(RS485_DE_PIN, OUTPUT);
        pinMode(RS485_RE_PIN, OUTPUT);
        digitalWrite(RS485_DE_PIN, LOW);
        digitalWrite(RS485_RE_PIN, LOW);

        serialPort.begin(config.rs485.baudRate, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
        setReceiveMode();

        Serial.printf("Modbus RTU Master @ %u baud, %u poll(s), interval %u ms\n",
                      config.rs485.baudRate, config.modbusPollCount,
                      config.rs485.modbusPollIntervalMs);
        return true;
    }

    void loop()
    {
        if (config.modbusPollCount == 0)
            return;

        unsigned long now = millis();
        if (now - lastPollMs < config.rs485.modbusPollIntervalMs)
            return;

        lastPollMs = now;

        const ModbusPollEntry &entry = config.modbusPolls[pollIndex];
        if (entry.slaveId == 0)
        {
            advancePollIndex();
            return;
        }

        uint16_t values[MODBUS_MAX_REGS];
        int8_t rc = readRegisters(entry.slaveId, entry.functionCode,
                                  entry.startReg, entry.regCount, values);
        if (rc > 0)
        {
            Serial.printf("Modbus: Slave %d FC 0x%02X Reg %d Count %d → OK\n",
                          entry.slaveId, entry.functionCode,
                          entry.startReg, entry.regCount);
            lastResultJson = buildResultJson(entry, values, (uint8_t)rc);
        }
        else
        {
            Serial.printf("Modbus: Slave %d FC 0x%02X Reg %d → FAIL (%d)\n",
                          entry.slaveId, entry.functionCode,
                          entry.startReg, rc);
            lastResultJson = buildErrorJson(entry, rc);
        }

        advancePollIndex();
    }

    bool hasResult() const { return lastResultJson.length() > 0; }

    String consumeResult()
    {
        String r = lastResultJson;
        lastResultJson = "";
        return r;
    }

    const ModbusPollEntry &currentPollEntry() const
    {
        return config.modbusPolls[pollIndex];
    }

private:
    static const size_t MODBUS_MAX_REGS = 64;
    static const uint16_t RESPONSE_TIMEOUT_MS = 500;

    GatewayConfig &config;
    HardwareSerial &serialPort;

    uint8_t pollIndex;
    unsigned long lastPollMs;
    String lastResultJson;

    uint8_t requestBuf[8];
    size_t requestLen;
    uint8_t responseBuf[256];
    size_t responseLen;

    void advancePollIndex()
    {
        pollIndex++;
        if (pollIndex >= config.modbusPollCount)
            pollIndex = 0;
    }

    static uint16_t crc16(const uint8_t *data, size_t length)
    {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < length; ++i)
        {
            crc ^= data[i];
            for (uint8_t j = 0; j < 8; ++j)
            {
                if (crc & 0x0001)
                    crc = (crc >> 1) ^ 0xA001;
                else
                    crc >>= 1;
            }
        }
        return crc;
    }

    void setTransmitMode()
    {
        digitalWrite(RS485_RE_PIN, HIGH);
        digitalWrite(RS485_DE_PIN, HIGH);
    }

    void setReceiveMode()
    {
        digitalWrite(RS485_DE_PIN, LOW);
        digitalWrite(RS485_RE_PIN, LOW);
    }

    void sendRequest(const uint8_t *data, size_t length)
    {
        setTransmitMode();
        delayMicroseconds(200);
        serialPort.write(data, length);
        serialPort.flush();
        delayMicroseconds(200);
        setReceiveMode();
    }

    size_t receiveResponse(uint8_t *buf, size_t maxLen, uint16_t timeoutMs)
    {
        size_t len = 0;
        unsigned long start = millis();
        while (millis() - start < timeoutMs && len < maxLen)
        {
            while (serialPort.available() && len < maxLen)
            {
                buf[len++] = (uint8_t)serialPort.read();
                start = millis();
            }
        }
        return len;
    }

    int8_t readRegisters(uint8_t slaveId, uint8_t funcCode,
                         uint16_t startReg, uint16_t regCount,
                         uint16_t *outValues)
    {
        if (regCount == 0 || regCount > MODBUS_MAX_REGS)
            return -1;

        requestBuf[0] = slaveId;
        requestBuf[1] = funcCode;
        requestBuf[2] = (uint8_t)(startReg >> 8);
        requestBuf[3] = (uint8_t)(startReg & 0xFF);
        requestBuf[4] = (uint8_t)(regCount >> 8);
        requestBuf[5] = (uint8_t)(regCount & 0xFF);

        uint16_t crc = crc16(requestBuf, 6);
        requestBuf[6] = (uint8_t)(crc & 0xFF);
        requestBuf[7] = (uint8_t)(crc >> 8);
        requestLen = 8;

        while (serialPort.available())
            serialPort.read();

        sendRequest(requestBuf, requestLen);

        responseLen = receiveResponse(responseBuf, sizeof(responseBuf), RESPONSE_TIMEOUT_MS);
        if (responseLen < 5)
            return -2;

        uint16_t respCrc = crc16(responseBuf, responseLen - 2);
        uint16_t recvCrc = (uint16_t)responseBuf[responseLen - 1] << 8 |
                           (uint16_t)responseBuf[responseLen - 2];
        if (respCrc != recvCrc)
            return -3;

        if (responseBuf[0] != slaveId)
            return -4;

        if (responseBuf[1] & 0x80)
            return -5;

        uint8_t byteCount = responseBuf[2];
        if (byteCount != (uint8_t)(regCount * 2))
            return -6;

        for (uint16_t i = 0; i < regCount; ++i)
        {
            outValues[i] = ((uint16_t)responseBuf[3 + i * 2] << 8) |
                           (uint16_t)responseBuf[4 + i * 2];
        }

        return (int8_t)regCount;
    }

    String buildResultJson(const ModbusPollEntry &entry,
                           const uint16_t *values, uint8_t count)
    {
        StaticJsonDocument<1024> doc;
        doc["slaveId"] = entry.slaveId;
        doc["functionCode"] = entry.functionCode;
        doc["startReg"] = entry.startReg;
        doc["count"] = count;
        if (entry.label[0] != '\0')
            doc["label"] = entry.label;
        doc["timestamp"] = millis();

        JsonArray arr = doc.createNestedArray("values");
        for (uint8_t i = 0; i < count; ++i)
        {
            arr.add(values[i]);
        }

        String output;
        serializeJson(doc, output);
        return output;
    }

    String buildErrorJson(const ModbusPollEntry &entry, int8_t errorCode)
    {
        StaticJsonDocument<256> doc;
        doc["slaveId"] = entry.slaveId;
        doc["functionCode"] = entry.functionCode;
        doc["startReg"] = entry.startReg;
        if (entry.label[0] != '\0')
            doc["label"] = entry.label;
        doc["error"] = errorCode;
        doc["timestamp"] = millis();

        String output;
        serializeJson(doc, output);
        return output;
    }
};

#endif // MODBUS_HANDLER_H
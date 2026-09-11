#ifndef RS485_HANDLER_H
#define RS485_HANDLER_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include "config.h"

class RS485Handler {
public:
    RS485Handler(GatewayConfig &cfg)
        : config(cfg), serialPort(Serial1), rxBufLen(0) {}

    bool begin()
    {
        if (!config.rs485.enabled)
            return false;

        pinMode(RS485_DE_PIN, OUTPUT);
        pinMode(RS485_RE_PIN, OUTPUT);
        digitalWrite(RS485_DE_PIN, LOW);
        digitalWrite(RS485_RE_PIN, LOW);

        serialPort.begin(config.rs485.baudRate, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
        setReceiveMode();
        rxBufLen = 0;

        Serial.printf("RS485 initialized @ %u baud (TX=%d RX=%d DE=%d RE=%d)\n",
                      config.rs485.baudRate, RS485_TX_PIN, RS485_RX_PIN,
                      RS485_DE_PIN, RS485_RE_PIN);
        return true;
    }

    void loop()
    {
        while (serialPort.available() && rxBufLen < sizeof(rxBuffer))
        {
            rxBuffer[rxBufLen++] = (uint8_t)serialPort.read();
        }
    }

    bool hasData() const { return rxBufLen > 0; }

    size_t readData(uint8_t *out, size_t maxLen)
    {
        size_t n = (rxBufLen < maxLen) ? rxBufLen : maxLen;
        memcpy(out, rxBuffer, n);
        if (rxBufLen > n)
        {
            memmove(rxBuffer, rxBuffer + n, rxBufLen - n);
        }
        rxBufLen -= n;
        return n;
    }

    size_t send(const uint8_t *data, size_t length)
    {
        setTransmitMode();
        delayMicroseconds(200);

        size_t written = serialPort.write(data, length);
        serialPort.flush();

        delayMicroseconds(200);
        setReceiveMode();
        return written;
    }

    void sendString(const char *str)
    {
        send((const uint8_t *)str, strlen(str));
    }

private:
    GatewayConfig &config;
    HardwareSerial &serialPort;

    static const size_t RX_BUF_SIZE = 256;
    uint8_t rxBuffer[RX_BUF_SIZE];
    size_t rxBufLen;

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
};

#endif // RS485_HANDLER_H
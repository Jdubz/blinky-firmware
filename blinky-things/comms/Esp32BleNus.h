#pragma once

/**
 * Esp32BleNus.h - Nordic UART Service (NUS) for ESP32-S3 using NimBLE
 *
 * Same protocol as the nRF52840 BleNus, but using NimBLE-Arduino 2.x API.
 * Provides bidirectional serial-over-BLE for fleet server communication.
 *
 * NUS UUIDs:
 *   Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX:      6E400002 (central writes commands here)
 *   TX:      6E400003 (device notifies responses here)
 */

#include <Arduino.h>
#include <NimBLEDevice.h>

class Esp32BleNus : public Print {
public:
    using LineCallback = void(*)(const char* line);

    /// Initialize NUS server and start advertising under `name` (the nRF52840's
    /// "Blinky-<deviceId>-<sn2>" / "Blinky-<sn4>" scheme, built by the sketch).
    void begin(const char* name);

    /// Drain TX buffer (call from loop).
    void update();

    /// Register callback for received lines.
    void setLineCallback(LineCallback cb) { lineCallback_ = cb; }

    // Print interface (buffered, drained in update())
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* data, size_t size) override;

    bool isConnected() const { return connected_; }
    void printDiagnostics(Print& out) const;

private:
    friend class NusServerCallbacks;
    friend class NusRxCallbacks;

    void onConnect(uint16_t connId);
    void onDisconnect(uint16_t connId);
    void onRxData(const uint8_t* data, size_t len);
    void drainTxBuffer();
    void runQueuedCommands();

    NimBLEServer* server_ = nullptr;
    NimBLECharacteristic* txChar_ = nullptr;  // Notify (device → central)
    NimBLECharacteristic* rxChar_ = nullptr;  // Write (central → device)

    LineCallback lineCallback_ = nullptr;

    // RX line assembly
    static const size_t LINE_BUF_SIZE = 768;
    char lineBuf_[LINE_BUF_SIZE];
    size_t lineLen_ = 0;

    // Command queue: NimBLE host task -> loop task, as BleNus on the nRF52840 (#173).
    // Lines run in update() on the loop task: the console isn't thread-safe, and a long
    // reply on the host task's stack is what reset the nRF52840 hat. One producer, one
    // consumer; the indices are published with memory barriers.
    static const uint8_t CMD_QUEUE_DEPTH = 4;
    char cmdQueue_[CMD_QUEUE_DEPTH][LINE_BUF_SIZE];
    volatile uint8_t cmdHead_ = 0;
    volatile uint8_t cmdTail_ = 0;
    volatile uint32_t cmdDropped_ = 0;
    uint32_t cmdDroppedReported_ = 0;

    // TX ring buffer
    static const size_t TX_BUF_SIZE = 4096;
    uint8_t txBuf_[TX_BUF_SIZE];
    volatile size_t txHead_ = 0;
    volatile size_t txTail_ = 0;

    bool connected_ = false;
    uint16_t mtu_ = 20;
    uint32_t linesRx_ = 0;
};

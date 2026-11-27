#include "Esp32RmtLedStrip.h"

#ifdef BLINKY_PLATFORM_ESP32S3

#include "../../audio/LoopMetrics.h"
#include <driver/gpio.h>
#include <driver/rmt_encoder.h>
#include <esp_heap_caps.h>
#include <new>
#include <string.h>

Esp32RmtLedStrip* volatile Esp32RmtLedStrip::strips_[MAX_STRIPS] = {nullptr, nullptr, nullptr, nullptr};
TaskHandle_t Esp32RmtLedStrip::txTask_ = nullptr;

namespace {

// One frame on the wire: the pixel bytes (bytes encoder), then the latch (copy encoder),
// the structure of ESP-IDF's led_strip RMT encoder.
struct WsEncoder {
    rmt_encoder_t base;
    rmt_encoder_handle_t bytes;
    rmt_encoder_handle_t copy;
    int state;                       // 0: pixel bytes, 1: latch
    rmt_symbol_word_t latch;
};

rmt_encode_state_t orState(rmt_encode_state_t a, rmt_encode_state_t b) {
    return static_cast<rmt_encode_state_t>(static_cast<int>(a) | static_cast<int>(b));
}

size_t wsEncode(rmt_encoder_t* enc, rmt_channel_handle_t ch, const void* data, size_t size,
                rmt_encode_state_t* retState) {
    WsEncoder* e = __containerof(enc, WsEncoder, base);
    rmt_encode_state_t session = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t symbols = 0;
    if (e->state == 0) {
        symbols += e->bytes->encode(e->bytes, ch, data, size, &session);
        if (session & RMT_ENCODING_COMPLETE) e->state = 1;
        if (session & RMT_ENCODING_MEM_FULL) {
            *retState = orState(state, RMT_ENCODING_MEM_FULL);
            return symbols;
        }
    }
    if (e->state == 1) {
        symbols += e->copy->encode(e->copy, ch, &e->latch, sizeof(e->latch), &session);
        if (session & RMT_ENCODING_COMPLETE) {
            e->state = 0;
            state = orState(state, RMT_ENCODING_COMPLETE);
        }
        if (session & RMT_ENCODING_MEM_FULL) state = orState(state, RMT_ENCODING_MEM_FULL);
    }
    *retState = state;
    return symbols;
}

esp_err_t wsReset(rmt_encoder_t* enc) {
    WsEncoder* e = __containerof(enc, WsEncoder, base);
    rmt_encoder_reset(e->bytes);
    rmt_encoder_reset(e->copy);
    e->state = 0;
    return ESP_OK;
}

esp_err_t wsDel(rmt_encoder_t* enc) {
    WsEncoder* e = __containerof(enc, WsEncoder, base);
    if (e->bytes) rmt_del_encoder(e->bytes);
    if (e->copy) rmt_del_encoder(e->copy);
    delete e;
    return ESP_OK;
}

// Ticks at 10 MHz (0.1 us).
constexpr uint16_t T0H = 4, T0L = 8, T1H = 8, T1L = 4;

esp_err_t newWsEncoder(uint32_t resolutionHz, uint32_t resetUs, rmt_encoder_handle_t* out) {
    WsEncoder* e = new (std::nothrow) WsEncoder();
    if (!e) return ESP_ERR_NO_MEM;
    e->base.encode = wsEncode;
    e->base.reset = wsReset;
    e->base.del = wsDel;
    rmt_bytes_encoder_config_t bc = {};
    bc.bit0.level0 = 1; bc.bit0.duration0 = T0H; bc.bit0.level1 = 0; bc.bit0.duration1 = T0L;
    bc.bit1.level0 = 1; bc.bit1.duration0 = T1H; bc.bit1.level1 = 0; bc.bit1.duration1 = T1L;
    bc.flags.msb_first = 1;
    esp_err_t err = rmt_new_bytes_encoder(&bc, &e->bytes);
    if (err == ESP_OK) {
        rmt_copy_encoder_config_t cc = {};
        err = rmt_new_copy_encoder(&cc, &e->copy);
    }
    if (err != ESP_OK) {
        wsDel(&e->base);
        return err;
    }
    // The latch: low for resetUs, as two halves (a symbol half holds at most 32767 ticks).
    const uint32_t half = resolutionHz / 1000000UL * resetUs / 2;
    static_assert(T1H + T1L == 12 && T0H + T0L == 12, "1.2 us bit period at 10 MHz");
    e->latch.level0 = 0;
    e->latch.duration0 = half;
    e->latch.level1 = 0;
    e->latch.duration1 = half;
    *out = &e->base;
    return ESP_OK;
}

uint8_t* allocInternal(uint32_t bytes) {
    auto* p = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (p) memset(p, 0, bytes);
    return p;
}

}  // namespace

Esp32RmtLedStrip::Esp32RmtLedStrip(uint16_t numPixels, int16_t pin, uint32_t ledType)
    : numPixels_(numPixels), pin_(pin), brightness_(0),
      rOffset_(0), gOffset_(0), bOffset_(0),
      pixels_(nullptr), staged_(nullptr), wire_(nullptr),
      chan_(nullptr), encoder_(nullptr), dma_(false), begun_(false), metricsOwner_(false),
      txErrorLogged_(false), lock_(portMUX_INITIALIZER_UNLOCKED),
      onWire_(false), stagedFresh_(false),
      handedSeq_(0), stagedSeq_(0), startedSeq_(0), startedMs_(0) {
    // NEO_* color order (lower 8 bits), as Nrf52PwmLedStrip decodes it.
    const uint8_t order = (uint8_t)(ledType & 0xFF);
    bOffset_ = (order >> 0) & 0x3;
    gOffset_ = (order >> 2) & 0x3;
    rOffset_ = (order >> 4) & 0x3;
    const bool inRange = rOffset_ <= 2 && gOffset_ <= 2 && bOffset_ <= 2;
    const bool distinct = rOffset_ != gOffset_ && rOffset_ != bOffset_ && gOffset_ != bOffset_;
    if (!inRange || !distinct) {
        Serial.print(F("[ERROR] Esp32RmtLedStrip: invalid ledType 0x"));
        Serial.print(ledType, HEX);
        Serial.println(inRange ? F(" (offsets must be distinct; check device JSON)")
                               : F(" (offsets must be 0-2; RGBW types are not supported)"));
        return;
    }
    if (pin < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        Serial.print(F("[ERROR] Esp32RmtLedStrip: ledPin "));
        Serial.print(pin);
        Serial.println(F(" is not an output GPIO (the ESP32-S3 takes GPIO numbers: D0 = GPIO1)"));
        return;
    }
    const uint32_t bytes = (uint32_t)numPixels * 3;
    pixels_ = allocInternal(bytes);
    staged_ = allocInternal(bytes);
    wire_ = allocInternal(bytes);
    if (!isValid()) {
        Serial.print(F("[ERROR] Esp32RmtLedStrip: out of internal RAM for "));
        Serial.print(numPixels);
        Serial.print(F(" LEDs (needs "));
        Serial.print(bytes * 3);
        Serial.println(F(" bytes)"));
        heap_caps_free(pixels_); pixels_ = nullptr;
        heap_caps_free(staged_); staged_ = nullptr;
        heap_caps_free(wire_); wire_ = nullptr;
    }
}

Esp32RmtLedStrip::~Esp32RmtLedStrip() {
    if (chan_) {
        portENTER_CRITICAL(&lock_);
        stagedFresh_ = false;
        portEXIT_CRITICAL(&lock_);
        rmt_tx_wait_all_done(chan_, 100);
        for (int i = 0; i < MAX_STRIPS; i++) {
            if (strips_[i] == this) strips_[i] = nullptr;
        }
        rmt_disable(chan_);
        rmt_del_channel(chan_);
    }
    if (encoder_) rmt_del_encoder(encoder_);
    heap_caps_free(wire_);
    heap_caps_free(staged_);
    heap_caps_free(pixels_);
}

void Esp32RmtLedStrip::begin() {
    if (begun_) return;
    if (!isValid()) {
        Serial.println(F("[ERROR] Esp32RmtLedStrip: not initialized (see the error above); LEDs stay dark"));
        return;
    }
    int slot = -1;
    for (int i = 0; i < MAX_STRIPS; i++) {
        if (strips_[i] == nullptr) { slot = i; break; }
    }
    if (slot < 0) {
        Serial.println(F("[ERROR] Esp32RmtLedStrip: no free RMT TX channel"));
        return;
    }

    // One transmit task for every strip: above the loop task (priority 1), on the loop's
    // core, so a waiting frame starts as soon as the wire frees, not when the loop yields.
    if (!txTask_) {
        if (xTaskCreatePinnedToCore(txTaskMain, "ledtx", 3072, nullptr, 2, &txTask_,
                                    ARDUINO_RUNNING_CORE) != pdPASS) {
            txTask_ = nullptr;
            Serial.println(F("[ERROR] Esp32RmtLedStrip: transmit task creation failed; LEDs stay dark"));
            return;
        }
    }

    rmt_tx_channel_config_t cfg = {};
    cfg.gpio_num = static_cast<gpio_num_t>(pin_);
    cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz = RMT_RESOLUTION_HZ;
    cfg.trans_queue_depth = 1;   // one frame on the wire; the next waits in staged_
    cfg.mem_block_symbols = 1024;
    cfg.flags.with_dma = 1;
    esp_err_t err = rmt_new_tx_channel(&cfg, &chan_);
    dma_ = (err == ESP_OK);
    if (err != ESP_OK) {
        // The S3 has one DMA-capable RMT TX channel: a second strand takes RMT memory blocks.
        Serial.print(F("[INFO] Esp32RmtLedStrip: no RMT DMA channel (err="));
        Serial.print(static_cast<int>(err));
        Serial.println(F("), using RMT memory"));
        cfg.flags.with_dma = 0;
        cfg.mem_block_symbols = 96;   // two 48-symbol blocks
        err = rmt_new_tx_channel(&cfg, &chan_);
    }
    if (err != ESP_OK) {
        chan_ = nullptr;
        Serial.print(F("[ERROR] Esp32RmtLedStrip: RMT TX channel failed (err="));
        Serial.print(static_cast<int>(err));
        Serial.println(F("); LEDs stay dark"));
        return;
    }
    err = newWsEncoder(RMT_RESOLUTION_HZ, RESET_US, &encoder_);
    rmt_tx_event_callbacks_t cbs = {};
    cbs.on_trans_done = onTransDone;
    if (err == ESP_OK) err = rmt_tx_register_event_callbacks(chan_, &cbs, this);
    if (err == ESP_OK) err = rmt_enable(chan_);
    if (err != ESP_OK) {
        Serial.print(F("[ERROR] Esp32RmtLedStrip: RMT setup failed (err="));
        Serial.print(static_cast<int>(err));
        Serial.println(F("); LEDs stay dark"));
        if (encoder_) { rmt_del_encoder(encoder_); encoder_ = nullptr; }
        rmt_del_channel(chan_);
        chan_ = nullptr;
        return;
    }

    vTaskSuspendAll();
    metricsOwner_ = true;
    for (int i = 0; i < MAX_STRIPS; i++) {
        if (strips_[i]) metricsOwner_ = false;
    }
    strips_[slot] = this;
    xTaskResumeAll();
    begun_ = true;

    Serial.print(F("[INFO] Esp32RmtLedStrip: ready, GPIO"));
    Serial.print(pin_);
    Serial.print(F(", "));
    Serial.print(numPixels_);
    Serial.print(F(" LEDs, RMT "));
    Serial.println(dma_ ? F("DMA") : F("memory"));
}

void Esp32RmtLedStrip::show() {
    if (!begun_) return;
    const uint32_t bytes = (uint32_t)numPixels_ * 3;
    portENTER_CRITICAL(&lock_);
    const bool superseded = stagedFresh_;
    memcpy(staged_, pixels_, bytes);
    handedSeq_ = handedSeq_ + 1;
    stagedSeq_ = handedSeq_;
    stagedFresh_ = true;
    portEXIT_CRITICAL(&lock_);
    if (superseded && metricsOwner_) LoopMetrics::noteLedSuperseded();
    xTaskNotifyGive(txTask_);  // higher priority: with the wire idle it starts the frame now
}

void Esp32RmtLedStrip::lastStarted(uint32_t& seq, uint32_t& startMs) const {
    portMUX_TYPE* lock = const_cast<portMUX_TYPE*>(&lock_);
    portENTER_CRITICAL(lock);
    seq = startedSeq_;
    startMs = startedMs_;
    portEXIT_CRITICAL(lock);
}

void Esp32RmtLedStrip::setPixelColor(uint16_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (!pixels_ || index >= numPixels_) return;
    if (brightness_) {
        r = ((uint16_t)r * (uint16_t)brightness_) >> 8;
        g = ((uint16_t)g * (uint16_t)brightness_) >> 8;
        b = ((uint16_t)b * (uint16_t)brightness_) >> 8;
    }
    uint8_t* p = &pixels_[index * 3];
    p[rOffset_] = r;
    p[gOffset_] = g;
    p[bOffset_] = b;
}

void Esp32RmtLedStrip::setPixelColor(uint16_t index, uint32_t color) {
    setPixelColor(index, (uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color);
}

void Esp32RmtLedStrip::clear() {
    if (pixels_) memset(pixels_, 0, (uint32_t)numPixels_ * 3);
}

void Esp32RmtLedStrip::setBrightness(uint8_t brightness) {
    brightness_ = (uint8_t)(brightness + 1);  // Adafruit convention, as Nrf52PwmLedStrip
}

uint8_t Esp32RmtLedStrip::getBrightness() const {
    return brightness_ ? (brightness_ - 1) : 255;
}

uint16_t Esp32RmtLedStrip::numPixels() const {
    return numPixels_;
}

uint32_t Esp32RmtLedStrip::Color(uint8_t r, uint8_t g, uint8_t b) const {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

uint32_t Esp32RmtLedStrip::txStackFreeBytes() {
    // ESP-IDF FreeRTOS counts stack in bytes.
    return txTask_ ? (uint32_t)uxTaskGetStackHighWaterMark(txTask_) : 0;
}

bool Esp32RmtLedStrip::onTransDone(rmt_channel_handle_t, const rmt_tx_done_event_data_t*, void* ctx) {
    auto* s = static_cast<Esp32RmtLedStrip*>(ctx);
    portENTER_CRITICAL_ISR(&s->lock_);
    s->onWire_ = false;
    portEXIT_CRITICAL_ISR(&s->lock_);
    BaseType_t woken = pdFALSE;
    if (txTask_) vTaskNotifyGiveFromISR(txTask_, &woken);
    return woken == pdTRUE;
}

void Esp32RmtLedStrip::txTaskMain(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // show() or a finished frame
        for (int i = 0; i < MAX_STRIPS; i++) {
            Esp32RmtLedStrip* s = strips_[i];
            if (s) s->serviceFromTask();
        }
    }
}

void Esp32RmtLedStrip::serviceFromTask() {
    const uint32_t bytes = (uint32_t)numPixels_ * 3;
    portENTER_CRITICAL(&lock_);
    if (onWire_ || !stagedFresh_) {
        portEXIT_CRITICAL(&lock_);
        return;
    }
    memcpy(wire_, staged_, bytes);   // wire_ is free: the RMT finished reading it
    stagedFresh_ = false;
    startedSeq_ = stagedSeq_;
    startedMs_ = millis();
    onWire_ = true;                  // before the transmit: the done interrupt clears it
    portEXIT_CRITICAL(&lock_);

    rmt_transmit_config_t tc = {};
    tc.loop_count = 0;
    tc.flags.eot_level = 0;
    const esp_err_t err = rmt_transmit(chan_, encoder_, wire_, bytes, &tc);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&lock_);
        onWire_ = false;
        portEXIT_CRITICAL(&lock_);
        if (!txErrorLogged_) {
            txErrorLogged_ = true;
            Serial.print(F("[ERROR] Esp32RmtLedStrip: rmt_transmit failed (err="));
            Serial.print(static_cast<int>(err));
            Serial.println(F("); frame dropped (logged once)"));
        }
        return;
    }
    if (metricsOwner_) LoopMetrics::noteLedFrameStarted();
}

#endif // BLINKY_PLATFORM_ESP32S3

#include "Nrf52PwmLedStrip.h"

#ifdef BLINKY_PLATFORM_NRF52840

#include "../../audio/LoopMetrics.h"

Nrf52PwmLedStrip* volatile Nrf52PwmLedStrip::byPwm_[PWM_COUNT] = {nullptr, nullptr, nullptr, nullptr};
TaskHandle_t Nrf52PwmLedStrip::txTask_ = nullptr;

static_assert(PWM_COUNT == 4, "nRF52840 MDK: PWM0..PWM3");

namespace {

NRF_PWM_Type* const kPwm[4] = {NRF_PWM0, NRF_PWM1, NRF_PWM2, NRF_PWM3};
const IRQn_Type kPwmIrq[4] = {PWM0_IRQn, PWM1_IRQn, PWM2_IRQn, PWM3_IRQn};

// Encode table: one nibble (MSB first) -> four duty values, as two little-endian words
// (the lower halfword goes out first). 6|0x8000 = T0H, 13|0x8000 = T1H (see the header).
#define BLINKY_H(bit) ((bit) ? 0x800DUL : 0x8006UL)
#define BLINKY_NIB(v) BLINKY_H((v) & 8) | (BLINKY_H((v) & 4) << 16), BLINKY_H((v) & 2) | (BLINKY_H((v) & 1) << 16)
const uint32_t kNibble[32] = {
    BLINKY_NIB(0),  BLINKY_NIB(1),  BLINKY_NIB(2),  BLINKY_NIB(3),
    BLINKY_NIB(4),  BLINKY_NIB(5),  BLINKY_NIB(6),  BLINKY_NIB(7),
    BLINKY_NIB(8),  BLINKY_NIB(9),  BLINKY_NIB(10), BLINKY_NIB(11),
    BLINKY_NIB(12), BLINKY_NIB(13), BLINKY_NIB(14), BLINKY_NIB(15),
};
#undef BLINKY_NIB
#undef BLINKY_H
static_assert(sizeof(kNibble) / sizeof(kNibble[0]) == 32, "nibble table: 16 nibbles x 2 words");

}  // namespace

static_assert((6 | 0x8000) == 0x8006 && (13 | 0x8000) == 0x800D, "kNibble must match T0H/T1H");

extern "C" {
void PWM0_IRQHandler(void) { Nrf52PwmLedStrip::onPwmIrq(0); }
void PWM1_IRQHandler(void) { Nrf52PwmLedStrip::onPwmIrq(1); }
void PWM2_IRQHandler(void) { Nrf52PwmLedStrip::onPwmIrq(2); }
void PWM3_IRQHandler(void) { Nrf52PwmLedStrip::onPwmIrq(3); }
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

Nrf52PwmLedStrip::Nrf52PwmLedStrip(uint16_t numPixels, int16_t pin, uint32_t ledType)
    : numPixels_(numPixels), pin_(pin), brightness_(0),
      rOffset_(0), gOffset_(0), bOffset_(0),
      pixels_(nullptr), staged_(nullptr), pwmBuffer_(nullptr), pwmBufferLen_(0),
      pwm_(nullptr), pwmIndex_(-1), begun_(false), metricsOwner_(false),
      onWire_(false), stagedFresh_(false),
      handedSeq_(0), stagedSeq_(0), startedSeq_(0), startedMs_(0) {

    // Decode the NEO_* ledType byte layout (see header). Mask to lower 8
    // bits — the upper bits hold the 400/800 KHz speed selector which this
    // driver ignores (WS2812B 800 KHz is hardcoded in the PWM timing).
    uint8_t order = (uint8_t)(ledType & 0xFF);
    bOffset_ = (order >> 0) & 0x3;
    gOffset_ = (order >> 2) & 0x3;
    rOffset_ = (order >> 4) & 0x3;

    // Validate offsets. The NEO_* encoding uses 2-bit fields that can hold
    // values 0-3, but this driver allocates only 3 bytes per pixel (RGB).
    // An offset of 3 — produced by RGBW-style constants the hardware does
    // not support — would walk one byte past the slot and corrupt the
    // next pixel on every write. Fail loud at construction per
    // CLAUDE.md "No Silent Fallbacks": leave the buffers unallocated
    // so isValid() reports false; the caller already haltWithError's
    // on !isValid().
    bool offsets_in_range = rOffset_ <= 2 && gOffset_ <= 2 && bOffset_ <= 2;
    bool offsets_distinct = rOffset_ != gOffset_ && rOffset_ != bOffset_ && gOffset_ != bOffset_;
    if (!offsets_in_range || !offsets_distinct) {
        Serial.print(F("[ERROR] Nrf52PwmLedStrip: invalid ledType 0x"));
        Serial.print(ledType, HEX);
        Serial.print(F(" decoded r="));
        Serial.print(rOffset_);
        Serial.print(F(" g="));
        Serial.print(gOffset_);
        Serial.print(F(" b="));
        Serial.print(bOffset_);
        Serial.println(offsets_in_range
            ? F(" (offsets must be distinct; check device JSON)")
            : F(" (offsets must be 0-2; RGBW types are not supported)"));
        return;
    }

    const uint32_t numBytes = (uint32_t)numPixels * 3;
    pixels_ = new(std::nothrow) uint8_t[numBytes];
    staged_ = new(std::nothrow) uint8_t[numBytes];
    if (pixels_) memset(pixels_, 0, numBytes);
    if (staged_) memset(staged_, 0, numBytes);

    // 8 duty values per byte, then the reset tail. EasyDMA reads RAM only: heap is RAM.
    pwmBufferLen_ = numBytes * 8 + RESET_PERIODS;
    pwmBuffer_ = new(std::nothrow) uint16_t[pwmBufferLen_];
    if (pwmBuffer_) {
        for (uint32_t i = numBytes * 8; i < pwmBufferLen_; i++) pwmBuffer_[i] = DUTY_LOW;
    }
    if (!pixels_ || !staged_ || !pwmBuffer_) {
        Serial.print(F("[ERROR] Nrf52PwmLedStrip: out of heap for "));
        Serial.print(numPixels);
        Serial.print(F(" LEDs (needs "));
        Serial.print(numBytes * 2 + pwmBufferLen_ * 2);
        Serial.println(F(" bytes)"));
    }
}

Nrf52PwmLedStrip::~Nrf52PwmLedStrip() {
    if (pwm_) {
        stagedFresh_ = false;  // nothing new starts (the task runs only while the loop waits)
        while (onWire_) vTaskDelay(1);
        pwm_->INTENCLR = PWM_INTENCLR_SEQEND0_Msk;
        NVIC_DisableIRQ(kPwmIrq[pwmIndex_]);
        byPwm_[pwmIndex_] = nullptr;
        pwm_->ENABLE = 0;
        pwm_->PSEL.OUT[0] = 0xFFFFFFFFUL;
    }
    delete[] pwmBuffer_;
    delete[] staged_;
    delete[] pixels_;
}

// ---------------------------------------------------------------------------
// ILedStrip interface
// ---------------------------------------------------------------------------

void Nrf52PwmLedStrip::begin() {
    if (begun_) return;
    if (!isValid()) {
        Serial.println(F("[ERROR] Nrf52PwmLedStrip: buffer alloc failed"));
        return;
    }

    // PWM SEQ[n].CNT is 15 bits (max 32767): 24 duty values per LED plus the reset tail.
    // Check before claiming the PWM peripheral to avoid leaking the slot on error.
    if (pwmBufferLen_ > 32767) {
        Serial.print(F("[ERROR] Too many LEDs for PWM DMA ("));
        Serial.print(numPixels_);
        Serial.print(F(", max "));
        Serial.print((32767 - RESET_PERIODS) / 24);
        Serial.println(F(")"));
        return;
    }

    // One transmit task serves every strip. Above the loop (it must start a waiting frame
    // the moment the wire frees, not when the loop next yields), equal to the core's
    // callback task, below Bluefruit. It runs ~0.5 ms per frame at 1024 LEDs.
    if (!txTask_) {
        if (xTaskCreate(txTaskMain, "ledtx", 256, nullptr, TASK_PRIO_NORMAL, &txTask_) != pdPASS) {
            txTask_ = nullptr;
            Serial.println(F("[ERROR] Nrf52PwmLedStrip: transmit task creation failed (out of heap); LEDs stay dark"));
            return;
        }
    }

    pwmIndex_ = findFreePwm();
    if (pwmIndex_ < 0) {
        Serial.println(F("[ERROR] No free PWM peripheral for LED strip"));
        return;
    }
    pwm_ = kPwm[pwmIndex_];

    // Configure PWM — persistent, not re-done per frame
    pwm_->MODE = (PWM_MODE_UPDOWN_Up << PWM_MODE_UPDOWN_Pos);
    pwm_->PRESCALER = (PWM_PRESCALER_PRESCALER_DIV_1 << PWM_PRESCALER_PRESCALER_Pos);
    pwm_->COUNTERTOP = (CTOP << PWM_COUNTERTOP_COUNTERTOP_Pos);
    pwm_->LOOP = (PWM_LOOP_CNT_Disabled << PWM_LOOP_CNT_Pos);
    pwm_->DECODER = (PWM_DECODER_LOAD_Common << PWM_DECODER_LOAD_Pos) |
                    (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);
    pwm_->SEQ[0].REFRESH = 0;
    pwm_->SEQ[0].ENDDELAY = 0;

    // Map Arduino pin to nRF GPIO
#if defined(ARDUINO_ARCH_NRF52840)
    uint32_t gpio = g_APinDescription[pin_].name;
#else
    uint32_t gpio = g_ADigitalPinMap[pin_];
#endif
    pwm_->PSEL.OUT[0] = gpio;
    pwm_->ENABLE = 1;

    // SEQEND interrupt: the ISR only marks the wire free and wakes the task. Lowest app
    // priority (7, the PDM's level; 0, 1 and 4 belong to the SoftDevice), at or below
    // configMAX_SYSCALL_INTERRUPT_PRIORITY (2) as FreeRTOS FromISR calls require.
    vTaskSuspendAll();
    metricsOwner_ = true;
    for (uint8_t i = 0; i < PWM_COUNT; i++) {
        if (byPwm_[i]) metricsOwner_ = false;
    }
    byPwm_[pwmIndex_] = this;
    xTaskResumeAll();
    pwm_->EVENTS_SEQEND[0] = 0;
    pwm_->INTENSET = PWM_INTENSET_SEQEND0_Msk;
    NVIC_ClearPendingIRQ(kPwmIrq[pwmIndex_]);
    NVIC_SetPriority(kPwmIrq[pwmIndex_], PWM_IRQ_PRIORITY);
    NVIC_EnableIRQ(kPwmIrq[pwmIndex_]);
    begun_ = true;

    Serial.print(F("[INFO] Nrf52PwmLedStrip: ready, pin=D"));
    Serial.print(pin_);
    Serial.print(F(" (GPIO 0x"));
    Serial.print(gpio, HEX);
    Serial.print(F("), PWM"));
    Serial.print(pwmIndex_);
    Serial.print(F(", "));
    Serial.print(numPixels_);
    Serial.print(F(" LEDs, PWM buf="));
    Serial.print(pwmBufferLen_ * 2);
    Serial.println(F(" bytes"));
}

void Nrf52PwmLedStrip::show() {
    if (!begun_) return;
    const uint32_t bytes = (uint32_t)numPixels_ * 3;
    // Hold the scheduler (not interrupts) so the transmit task can't read a half-copied frame.
    vTaskSuspendAll();
    if (stagedFresh_ && metricsOwner_) LoopMetrics::noteLedSuperseded();
    memcpy(staged_, pixels_, bytes);
    handedSeq_ = handedSeq_ + 1;
    stagedSeq_ = handedSeq_;
    stagedFresh_ = true;
    xTaskResumeAll();
    xTaskNotifyGive(txTask_);  // higher priority: with the wire idle it starts the frame now
}

void Nrf52PwmLedStrip::lastStarted(uint32_t& seq, uint32_t& startMs) const {
    vTaskSuspendAll();  // the task writes the pair; read it whole
    seq = startedSeq_;
    startMs = startedMs_;
    xTaskResumeAll();
}

void Nrf52PwmLedStrip::setPixelColor(uint16_t index, uint8_t r, uint8_t g, uint8_t b) {
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

void Nrf52PwmLedStrip::setPixelColor(uint16_t index, uint32_t color) {
    setPixelColor(index,
                  (uint8_t)(color >> 16),
                  (uint8_t)(color >> 8),
                  (uint8_t)color);
}

void Nrf52PwmLedStrip::clear() {
    if (pixels_) {
        memset(pixels_, 0, (uint32_t)numPixels_ * 3);
    }
}

void Nrf52PwmLedStrip::setBrightness(uint8_t brightness) {
    // Adafruit convention: store b+1 so internal 0 = max (no scaling).
    // setBrightness(0) → stores 1 → near-off; setBrightness(255) → stores 0 → max.
    brightness_ = (uint8_t)(brightness + 1);
}

uint8_t Nrf52PwmLedStrip::getBrightness() const {
    // brightness_==0 means setBrightness() was never called → max brightness (255)
    return brightness_ ? (brightness_ - 1) : 255;
}

uint16_t Nrf52PwmLedStrip::numPixels() const {
    return numPixels_;
}

uint32_t Nrf52PwmLedStrip::Color(uint8_t r, uint8_t g, uint8_t b) const {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

uint32_t Nrf52PwmLedStrip::txStackFreeBytes() {
    return txTask_ ? (uint32_t)uxTaskGetStackHighWaterMark(txTask_) * sizeof(StackType_t) : 0;
}

// ---------------------------------------------------------------------------
// Transmit path
// ---------------------------------------------------------------------------

void Nrf52PwmLedStrip::onPwmIrq(uint8_t instance) {
    NRF_PWM_Type* p = kPwm[instance];
    if (!p->EVENTS_SEQEND[0]) return;
    p->EVENTS_SEQEND[0] = 0;
    (void)p->EVENTS_SEQEND[0];  // read back: the clear lands before the ISR returns (no re-entry)
    Nrf52PwmLedStrip* s = byPwm_[instance];
    if (s) s->onWire_ = false;
    BaseType_t woken = pdFALSE;
    if (txTask_) vTaskNotifyGiveFromISR(txTask_, &woken);
    portYIELD_FROM_ISR(woken);
}

void Nrf52PwmLedStrip::txTaskMain(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // show() or SEQEND
        for (uint8_t i = 0; i < PWM_COUNT; i++) {
            Nrf52PwmLedStrip* s = byPwm_[i];
            if (s) s->serviceFromTask();
        }
    }
}

void Nrf52PwmLedStrip::serviceFromTask() {
    // The loop (the only show() caller) runs below this task, and copies into staged_ with
    // the scheduler held, so staged_ is whole here and can't change during the encode.
    if (onWire_ || !stagedFresh_) return;
    encodeStaged();
    stagedFresh_ = false;
    startedSeq_ = stagedSeq_;
    startedMs_ = millis();
    onWire_ = true;  // before SEQSTART: the ISR clears it
    pwm_->SEQ[0].PTR = (uint32_t)pwmBuffer_;
    pwm_->SEQ[0].CNT = pwmBufferLen_;
    pwm_->TASKS_SEQSTART[0] = 1;
    if (metricsOwner_) LoopMetrics::noteLedFrameStarted();
}

void Nrf52PwmLedStrip::encodeStaged() {
    // Two table words per nibble: 4 stores per byte instead of 8 compare-and-stores.
    // pwmBuffer_ comes from new[] (8-byte aligned), and each byte fills 16 bytes.
    const uint32_t numBytes = (uint32_t)numPixels_ * 3;
    uint32_t* out = reinterpret_cast<uint32_t*>(pwmBuffer_);
    const uint8_t* in = staged_;
    for (uint32_t n = 0; n < numBytes; n++) {
        const uint8_t v = in[n];
        const uint32_t* hi = &kNibble[(v >> 4) * 2];
        const uint32_t* lo = &kNibble[(v & 0x0F) * 2];
        out[0] = hi[0];
        out[1] = hi[1];
        out[2] = lo[0];
        out[3] = lo[1];
        out += 4;
    }
    // The reset tail after the data was written once at construction and never changes.
}

int8_t Nrf52PwmLedStrip::findFreePwm() {
    for (uint8_t i = 0; i < PWM_COUNT; i++) {
        NRF_PWM_Type* p = kPwm[i];
        if (byPwm_[i] == nullptr &&
            (p->ENABLE == 0) &&
            (p->PSEL.OUT[0] & PWM_PSEL_OUT_CONNECT_Msk) &&
            (p->PSEL.OUT[1] & PWM_PSEL_OUT_CONNECT_Msk) &&
            (p->PSEL.OUT[2] & PWM_PSEL_OUT_CONNECT_Msk) &&
            (p->PSEL.OUT[3] & PWM_PSEL_OUT_CONNECT_Msk)) {
            return (int8_t)i;
        }
    }
    return -1;
}

#endif // BLINKY_PLATFORM_NRF52840

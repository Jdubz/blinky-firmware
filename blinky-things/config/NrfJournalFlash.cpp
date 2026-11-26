#include "NrfJournalFlash.h"

#if BLINKY_SETTINGS_JOURNAL
#include <Arduino.h>
#include <string.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>
#include "../inputs/SerialConsole.h"

namespace {
// The one journal operation in flight (the writer task's, or setup()'s).
volatile bool jf_inflight = false;
volatile uint32_t jf_event = 0;
SemaphoreHandle_t jf_done = nullptr;
// What the in-flight operation leaves on flash: jf_src words at jf_addr, or
// (jf_src null) jf_words erased words.
volatile uint32_t jf_addr = 0;
const uint32_t* volatile jf_src = nullptr;
volatile uint32_t jf_words = 0;
uint32_t jf_ops = 0;
uint32_t jf_retries = 0;

bool journalOpLanded() {
    const uint32_t* p = reinterpret_cast<const uint32_t*>(jf_addr);
    if (jf_src != nullptr) return memcmp(p, jf_src, jf_words * 4) == 0;
    for (uint32_t i = 0; i < jf_words; i++) {
        if (p[i] != 0xFFFFFFFFu) return false;
    }
    return true;
}

uint32_t journalOpIssue() {
    if (jf_src == nullptr) return sd_flash_page_erase(jf_addr / NrfJournalFlash::PAGE);
    return sd_flash_write(reinterpret_cast<uint32_t*>(jf_addr), jf_src, jf_words);
}

// One erase or write, completed and verified.
bool journalRunOp(uint32_t addr, const uint32_t* src, uint32_t words) {
    if (jf_done == nullptr) {
        jf_done = xSemaphoreCreateBinary();
        if (jf_done == nullptr) {
            SerialConsole::logError(F("settings journal: no semaphore for flash events"));
            return false;
        }
    }
    for (int attempt = 0; attempt < 5; attempt++) {
        jf_addr = addr;
        jf_src = src;
        jf_words = words;
        uint8_t sd = 0;
        (void)sd_softdevice_is_enabled(&sd);
        if (!sd) {
            // Before Bluefruit.begin(): the SoftDevice API runs it now.
            const uint32_t err = journalOpIssue();
            jf_ops++;
            if (err != NRF_SUCCESS) {
                SerialConsole::logError(F("settings journal: flash operation refused (SoftDevice off)"));
                return false;
            }
            return journalOpLanded();
        }
        xSemaphoreTake(jf_done, 0);  // drop a stale completion
        jf_inflight = false;
        const uint32_t t0 = millis();
        uint32_t err;
        for (;;) {
            // The SoC task must not see this operation's event before
            // jf_inflight says it's ours, or it goes to the core's waiter.
            vTaskSuspendAll();
            err = journalOpIssue();
            if (err == NRF_SUCCESS) jf_inflight = true;
            xTaskResumeAll();
            if (err != NRF_ERROR_BUSY) break;  // BUSY: the core's own flash operation is pending
            if (millis() - t0 > 10000) break;
            vTaskDelay(1);
        }
        if (err != NRF_SUCCESS) {
            SerialConsole::logError(F("settings journal: SoftDevice refused the flash operation"));
            return false;
        }
        jf_ops++;
        if (xSemaphoreTake(jf_done, pdMS_TO_TICKS(30000)) != pdTRUE) {
            // jf_inflight stays set: a late event is still claimed here.
            SerialConsole::logError(F("settings journal: no flash completion event in 30 s"));
            return false;
        }
        if (jf_event == NRF_EVT_FLASH_OPERATION_SUCCESS) {
            if (journalOpLanded()) return true;
            SerialConsole::logError(F("settings journal: flash readback mismatch"));
            return false;
        }
        // NRF_EVT_FLASH_OPERATION_ERROR: no room between radio events this
        // time (S140 SDS, Flash memory API: retry).
        jf_retries++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    SerialConsole::logError(F("settings journal: flash operation failed 5 times"));
    return false;
}
}  // namespace

// Bluefruit's SoC task calls this for every flash event (see the header).
extern "C" void __real_flash_nrf5x_event_cb(uint32_t event);
extern "C" void __wrap_flash_nrf5x_event_cb(uint32_t event) {
    // A SUCCESS counts as ours only once our data is on flash: a core
    // operation that completed just before ours was issued has its event
    // still queued, and it must reach the core.
    if (jf_inflight && (event != NRF_EVT_FLASH_OPERATION_SUCCESS || journalOpLanded())) {
        jf_event = event;
        jf_inflight = false;
        xSemaphoreGive(jf_done);
        return;
    }
    __real_flash_nrf5x_event_cb(event);
}

extern uint32_t __etext;
extern uint32_t __data_start__;
extern uint32_t __data_end__;

bool NrfJournalFlash::usable() {
    // Keep the event hook linked: it references __real_flash_nrf5x_event_cb,
    // which exists only when the build wraps the core's handler (build.sh).
    static void (*volatile hook)(uint32_t) = __wrap_flash_nrf5x_event_cb;
    if (hook == nullptr) return false;
    if (NRF_UICR->NRFFW[0] != BOOTLOADER_START) {
        SerialConsole::logError(F("[FALLBACK] settings journal off: bootloader not at 0xF4000 (saves use LittleFS and stall, #49)"));
        return false;
    }
    const uint32_t imageEnd = reinterpret_cast<uint32_t>(&__etext) +
        (reinterpret_cast<uint32_t>(&__data_end__) - reinterpret_cast<uint32_t>(&__data_start__));
    if (imageEnd > BASE) {
        SerialConsole::logError(F("[FALLBACK] settings journal off: firmware image reaches 0xEA000 (saves use LittleFS and stall, #49)"));
        return false;
    }
    return true;
}

bool NrfJournalFlash::erasePage(uint32_t page) {
    if (page >= PAGES) return false;
    return journalRunOp(BASE + page * PAGE, nullptr, PAGE / 4);
}

bool NrfJournalFlash::program(uint32_t off, const uint32_t* words, uint32_t nWords) {
    if ((off & 3u) != 0 || off + nWords * 4 > PAGES * PAGE) return false;
    for (uint32_t i = 0; i < nWords; i += CHUNK_WORDS) {
        const uint32_t n = (nWords - i < CHUNK_WORDS) ? nWords - i : CHUNK_WORDS;
        if (!journalRunOp(BASE + off + i * 4, words + i, n)) return false;
    }
    return true;
}

uint32_t NrfJournalFlash::ops() { return jf_ops; }
uint32_t NrfJournalFlash::retries() { return jf_retries; }
#endif

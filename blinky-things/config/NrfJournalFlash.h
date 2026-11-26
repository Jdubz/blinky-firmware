#pragma once

#include "SettingsJournal.h"

// The settings journal's flash on the native nRF52840 core (blinky-firmware#49).
#if !(defined(ARDUINO_ARCH_MBED) || defined(TARGET_NAME) || defined(MBED_CONF_TARGET_NAME)) && \
    (defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(NRF52840_XXAA))
#define BLINKY_SETTINGS_JOURNAL 1

/**
 * Three pages at 0xEA000-0xECFFF: the bottom of the 40 kB the bootloader
 * reserves for application data (Adafruit/our fork, nRF52840:
 * DFU_APP_DATA_RESERVED = 10 pages below the bootloader at 0xF4000). The
 * core's LittleFS uses only the top 7 of those pages (0xED000-0xF3FFF), so
 * these three are unused, and no update path writes them: DFU is single
 * bank and erases only the pages the new image covers, the bootloader
 * refuses an image that reaches into the reserved area, and UF2 writes only
 * the image's own blocks. usable() checks the bootloader start and that the
 * running image ends below the region before anything is written.
 *
 * Erases and writes go through the SoftDevice flash API (NVMC is restricted
 * while the SoftDevice runs; before Bluefruit.begin() the same calls run
 * synchronously). Writes are split into chunks of CHUNK_WORDS so the CPU is
 * halted at most CHUNK_WORDS * 41 us (tWRITE max) per operation, and the
 * caller (the low-priority writer task) waits for each completion event.
 * Completion events reach this backend through
 * __wrap_flash_nrf5x_event_cb (scripts/build.sh links with
 * -Wl,--wrap=flash_nrf5x_event_cb): Bluefruit's SoC task hands every flash
 * event to the core's InternalFS handler, whose semaphore must only count
 * the core's own operations.
 */
class NrfJournalFlash : public JournalFlash {
public:
    static const uint32_t BASE = 0xEA000;
    static const uint32_t PAGES = 3;
    static const uint32_t PAGE = 4096;
    static const uint32_t BOOTLOADER_START = 0xF4000;
    static const uint32_t LITTLEFS_START = 0xED000;
    static const uint32_t CHUNK_WORDS = 8;  // <= 328 us of halted CPU per write

    // Safe to use on this chip: the bootloader starts where the layout above
    // assumes and the running image ends below BASE. Logs why not.
    static bool usable();

    const uint8_t* data() const override { return reinterpret_cast<const uint8_t*>(BASE); }
    uint32_t pageSize() const override { return PAGE; }
    uint32_t pageCount() const override { return PAGES; }
    bool erasePage(uint32_t page) override;
    bool program(uint32_t off, const uint32_t* words, uint32_t nWords) override;

    // Diagnostics: operations issued, retried after an error event, events claimed.
    static uint32_t ops();
    static uint32_t retries();
};

static_assert(NrfJournalFlash::BASE + NrfJournalFlash::PAGES * NrfJournalFlash::PAGE == NrfJournalFlash::LITTLEFS_START,
              "settings journal must end where the core's LittleFS starts");
static_assert(NrfJournalFlash::BASE >= NrfJournalFlash::BOOTLOADER_START - 10 * NrfJournalFlash::PAGE,
              "settings journal must lie inside the bootloader's reserved app-data pages");
#endif

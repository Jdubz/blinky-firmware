#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * SettingsJournal - settings persistence that never erases flash while the
 * render loop runs (blinky-firmware#49).
 *
 * Why: on the nRF52840 a page erase halts the CPU for up to 85 ms (product
 * spec, NVMC: tERASEPAGE), and with the SoftDevice enabled the application
 * cannot split it: NVMC is a restricted peripheral, so partial erase
 * (ERASEPAGEPARTIAL) is unavailable, and sd_flash_page_erase() runs the whole
 * erase as one ~90 ms timing event (S140 SDS, Flash memory API). LittleFS
 * through the core's page cache erased and rewrote a 4 kB page on every save.
 * Writes are short (tWRITE <= 41 us per word), so the journal only ever
 * writes at run time, in small chunks, and does its erases at boot, before
 * the loop renders a frame.
 *
 * Layout: a ring of erase pages. Each page starts with a header (magic, seq,
 * ~seq) and a FULL record of the state, so pages are independent; DELTA
 * records (changed byte runs) and SYNC records follow. The newest page with
 * a valid header and a valid first FULL record is current. Each record is
 * [u32 head = len<<16 | type<<8 | 0xA5][u32 crc32(head, payload)][payload,
 * zero-padded to a word]. A blank (all-ones) head ends a page's records. A
 * torn or corrupt record closes the page: nothing after it is trusted and
 * nothing more is appended there.
 *
 * Every word is written once between erases (nWRITE limit), records are
 * written in address order, and a record counts only once its CRC checks,
 * so a power cut mid-write loses at most that one save.
 *
 * Platform-independent: the flash is a JournalFlash (the nRF52 backend uses
 * the SoftDevice flash API; host tests use a NOR-flash simulator).
 */
class JournalFlash {
public:
    virtual ~JournalFlash() {}
    virtual const uint8_t* data() const = 0;  // the region, memory-mapped for reads
    virtual uint32_t pageSize() const = 0;
    virtual uint32_t pageCount() const = 0;
    // Erase one page (all bytes 0xFF). Halts the CPU up to 85 ms on nRF52840.
    virtual bool erasePage(uint32_t page) = 0;
    // Program nWords words at byte offset off (word aligned) of the region.
    // The target words are erased. True only if the words read back.
    virtual bool program(uint32_t off, const uint32_t* words, uint32_t nWords) = 0;
};

class SettingsJournal {
public:
    static const uint16_t MAX_STATE = 768;   // bytes; ConfigData is <= 650
    static const uint32_t PAGE_MAGIC = 0x4A4B4C42u;  // "BLKJ"
    static const uint8_t TYPE_FULL = 1;
    static const uint8_t TYPE_DELTA = 2;
    static const uint8_t TYPE_SYNC = 3;

    enum class Result : uint8_t {
        WRITTEN,              // persisted without an erase
        WRITTEN_AFTER_ERASE,  // persisted, but every page was used: a page was erased (stall)
        UNCHANGED,            // identical to the journal's state: nothing written
        FAILED,               // not persisted
    };

    explicit SettingsJournal(JournalFlash& flash);

    // Scan the flash for the newest committed state. True if one was found.
    bool mount();

    bool hasState() const { return has_; }
    const uint8_t* state() const { return state_; }
    uint16_t stateLen() const { return len_; }

    // The last SYNC value recorded (ConfigStorage: CRC of the LittleFS copy).
    bool hasSync() const { return syncValid_; }
    uint32_t syncValue() const { return sync_; }

    // Boot only (erases allowed): make s the state, leave the current page
    // with at least a quarter page free and every other page erased, so the
    // run-time appends that follow need no erase.
    bool prepare(const uint8_t* s, uint16_t len);

    // Persist s as a DELTA (or FULL) record. Run time: allowErase=false
    // never erases (FAILED when no page is free); allowErase=true erases
    // the oldest page when all are used and says so (WRITTEN_AFTER_ERASE).
    Result append(const uint8_t* s, uint16_t len, bool allowErase);

    // Record a SYNC value (same erase rules as append).
    Result appendSync(uint32_t value, bool allowErase);

    // Bytes that can still be appended without an erase (current page's
    // free space plus whole blank pages, record overhead not subtracted).
    uint32_t freeBytes() const;
    uint32_t eraseCount() const { return erases_; }
    int32_t currentPage() const { return cur_; }

    static uint32_t crc32(uint32_t crc, const uint8_t* p, size_t n);

private:
    static const uint32_t HEADER_BYTES = 12;
    static const uint32_t RECORD_OVERHEAD = 8;
    static const uint32_t BUF_WORDS = (HEADER_BYTES + RECORD_OVERHEAD + MAX_STATE + 2 * RECORD_OVERHEAD + 8) / 4;

    JournalFlash& flash_;
    int32_t cur_ = -1;        // current page, -1 none
    uint32_t off_ = 0;        // next free byte in the current page
    uint32_t maxSeq_ = 0;     // highest valid page seq seen
    bool closed_ = false;     // current page takes no more appends
    bool has_ = false;
    uint16_t len_ = 0;
    uint8_t state_[MAX_STATE];
    bool syncValid_ = false;
    uint32_t sync_ = 0;
    uint32_t erases_ = 0;
    uint32_t buf_[BUF_WORDS];          // record encoding
    uint8_t payload_[MAX_STATE + 64];  // delta encoding scratch

    uint32_t read32(uint32_t off) const;
    bool pageBlank(uint32_t page, uint32_t from = 0) const;
    bool replayPage(uint32_t page, uint32_t* endOff, bool* closed);
    static uint32_t recordBytes(uint16_t plen) { return RECORD_OVERHEAD + ((plen + 3u) & ~3u); }
    // Encode one record into buf_ at word index w; returns words used.
    uint32_t encode(uint32_t w, uint8_t type, const uint8_t* payload, uint16_t plen);
    uint16_t encodeDelta(const uint8_t* s, uint16_t len);  // into payload_; 0xFFFF if FULL is smaller
    bool erase(uint32_t page);
    // Start the next page with header + FULL(s) (+ SYNC); s becomes the state.
    bool openPage(const uint8_t* s, uint16_t len, bool allowErase, bool* erased);
    Result commit(uint8_t type, const uint8_t* payload, uint16_t plen,
                  const uint8_t* s, uint16_t len, bool allowErase);
};

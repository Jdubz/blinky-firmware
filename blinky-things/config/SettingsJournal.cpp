#include "SettingsJournal.h"

#include <string.h>

SettingsJournal::SettingsJournal(JournalFlash& flash) : flash_(flash) {
    memset(state_, 0, sizeof(state_));
}

uint32_t SettingsJournal::crc32(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t SettingsJournal::read32(uint32_t off) const {
    uint32_t v;
    memcpy(&v, flash_.data() + off, 4);
    return v;
}

bool SettingsJournal::pageBlank(uint32_t page, uint32_t from) const {
    const uint32_t ps = flash_.pageSize();
    for (uint32_t o = from; o < ps; o += 4) {
        if (read32(page * ps + o) != 0xFFFFFFFFu) return false;
    }
    return true;
}

// Replay one page into state_. True if the page has a valid header and a
// valid first FULL record. *endOff is where the next record would go and
// *closed says the page can take no more (a torn/corrupt record, or a tail
// that isn't blank).
bool SettingsJournal::replayPage(uint32_t page, uint32_t* endOff, bool* closed) {
    const uint32_t ps = flash_.pageSize();
    const uint32_t base = page * ps;
    *closed = false;
    bool first = true;
    bool syncValid = false;
    uint32_t sync = 0;
    uint32_t off = HEADER_BYTES;
    for (;;) {
        if (off + RECORD_OVERHEAD > ps) break;  // page full
        const uint32_t head = read32(base + off);
        if (head == 0xFFFFFFFFu) {
            if (!pageBlank(page, off)) *closed = true;  // something was written past the end
            break;
        }
        const uint16_t len = static_cast<uint16_t>(head >> 16);
        const uint8_t type = static_cast<uint8_t>(head >> 8);
        const uint32_t rec = recordBytes(len);
        bool ok = (head & 0xFFu) == 0xA5u && off + rec <= ps &&
                  (type == TYPE_FULL || type == TYPE_DELTA || type == TYPE_SYNC);
        const uint8_t* payload = flash_.data() + base + off + RECORD_OVERHEAD;
        if (ok) {
            uint32_t crc = crc32(0, reinterpret_cast<const uint8_t*>(&head), 4);
            crc = crc32(crc, payload, len);
            ok = crc == read32(base + off + 4);
        }
        if (ok && first) ok = type == TYPE_FULL;
        if (ok && type == TYPE_FULL) ok = len <= MAX_STATE;
        if (ok && type == TYPE_SYNC) ok = len == 4;
        if (ok && type == TYPE_DELTA) {
            // Validate every run before applying any of them.
            uint32_t p = 0;
            while (ok && p < len) {
                if (p + 4 > len) { ok = false; break; }
                const uint16_t at = static_cast<uint16_t>(payload[p] | (payload[p + 1] << 8));
                const uint16_t n = static_cast<uint16_t>(payload[p + 2] | (payload[p + 3] << 8));
                if (n == 0 || static_cast<uint32_t>(at) + n > len_ || p + 4 + n > len) ok = false;
                p += 4u + n;
            }
        }
        if (!ok) {
            if (first) return false;
            *closed = true;
            break;
        }
        if (type == TYPE_FULL) {
            memcpy(state_, payload, len);
            len_ = len;
        } else if (type == TYPE_DELTA) {
            uint32_t p = 0;
            while (p < len) {
                const uint16_t at = static_cast<uint16_t>(payload[p] | (payload[p + 1] << 8));
                const uint16_t n = static_cast<uint16_t>(payload[p + 2] | (payload[p + 3] << 8));
                memcpy(state_ + at, payload + p + 4, n);
                p += 4u + n;
            }
        } else {
            memcpy(&sync, payload, 4);
            syncValid = true;
        }
        first = false;
        off += rec;
    }
    if (first) return false;  // header only (torn while opening)
    *endOff = off;
    syncValid_ = syncValid;
    sync_ = sync;
    return true;
}

bool SettingsJournal::mount() {
    const uint32_t ps = flash_.pageSize();
    const uint32_t pages = flash_.pageCount();
    has_ = false;
    cur_ = -1;
    off_ = 0;
    closed_ = false;
    len_ = 0;
    syncValid_ = false;
    maxSeq_ = 0;
    // Valid headers, newest first. Pages are few (a handful), so selection is fine.
    bool tried[32] = {};
    if (pages > 32) return false;
    for (uint32_t p = 0; p < pages; p++) {
        const uint32_t m = read32(p * ps), s = read32(p * ps + 4), ns = read32(p * ps + 8);
        if (m == PAGE_MAGIC && s == ~ns && s > maxSeq_) maxSeq_ = s;
    }
    for (;;) {
        int32_t best = -1;
        uint32_t bestSeq = 0;
        for (uint32_t p = 0; p < pages; p++) {
            if (tried[p]) continue;
            const uint32_t m = read32(p * ps), s = read32(p * ps + 4), ns = read32(p * ps + 8);
            if (m != PAGE_MAGIC || s != ~ns) continue;
            if (best < 0 || s > bestSeq) { best = static_cast<int32_t>(p); bestSeq = s; }
        }
        if (best < 0) return false;
        tried[best] = true;
        uint32_t end = 0;
        bool closed = false;
        if (replayPage(static_cast<uint32_t>(best), &end, &closed)) {
            cur_ = best;
            off_ = end;
            closed_ = closed;
            has_ = true;
            return true;
        }
    }
}

uint32_t SettingsJournal::encode(uint32_t w, uint8_t type, const uint8_t* payload, uint16_t plen) {
    const uint32_t head = (static_cast<uint32_t>(plen) << 16) | (static_cast<uint32_t>(type) << 8) | 0xA5u;
    uint32_t crc = crc32(0, reinterpret_cast<const uint8_t*>(&head), 4);
    crc = crc32(crc, payload, plen);
    buf_[w] = head;
    buf_[w + 1] = crc;
    const uint32_t pw = (plen + 3u) / 4u;
    if (pw > 0) buf_[w + 1 + pw] = 0;  // zero padding in the last word
    memcpy(&buf_[w + 2], payload, plen);
    return 2 + pw;
}

uint16_t SettingsJournal::encodeDelta(const uint8_t* s, uint16_t len) {
    // Runs of changed bytes; gaps shorter than a run header (4 bytes) are
    // merged into the run. Returns 0xFFFF when a FULL record is no larger.
    uint32_t out = 0;
    uint32_t i = 0;
    while (i < len) {
        if (s[i] == state_[i]) { i++; continue; }
        const uint32_t start = i;
        uint32_t last = i;
        uint32_t j = i + 1;
        while (j < len) {
            if (s[j] != state_[j]) { last = j; j++; continue; }
            uint32_t k = j;
            while (k < len && s[k] == state_[k] && k - j < 4) k++;
            if (k == len || k - j >= 4) break;
            j = k;
        }
        const uint32_t n = last - start + 1;
        if (out + 4 + n >= len || out + 4 + n > sizeof(payload_)) return 0xFFFF;
        payload_[out++] = static_cast<uint8_t>(start);
        payload_[out++] = static_cast<uint8_t>(start >> 8);
        payload_[out++] = static_cast<uint8_t>(n);
        payload_[out++] = static_cast<uint8_t>(n >> 8);
        memcpy(payload_ + out, s + start, n);
        out += n;
        i = last + 1;
    }
    return static_cast<uint16_t>(out);
}

bool SettingsJournal::erase(uint32_t page) {
    erases_++;
    return flash_.erasePage(page);
}

bool SettingsJournal::openPage(const uint8_t* s, uint16_t len, bool allowErase, bool* erased) {
    const uint32_t ps = flash_.pageSize();
    const uint32_t pages = flash_.pageCount();
    const uint32_t need = HEADER_BYTES + recordBytes(len) + (syncValid_ ? recordBytes(4) : 0);
    if (len > MAX_STATE || need > ps) return false;
    const uint32_t next = cur_ < 0 ? 0 : (static_cast<uint32_t>(cur_) + 1) % pages;
    if (static_cast<int32_t>(next) == cur_) return false;  // a one-page journal can't rotate
    if (!pageBlank(next)) {
        if (!allowErase) return false;
        *erased = true;
        if (!erase(next)) return false;
    }
    const uint32_t seq = maxSeq_ + 1;
    buf_[0] = PAGE_MAGIC;
    buf_[1] = seq;
    buf_[2] = ~seq;
    uint32_t w = 3;
    w += encode(w, TYPE_FULL, s, len);
    if (syncValid_) w += encode(w, TYPE_SYNC, reinterpret_cast<const uint8_t*>(&sync_), 4);
    const bool ok = flash_.program(next * ps, buf_, w);
    // Even a failed write may have consumed the seq; never reuse it.
    maxSeq_ = seq;
    if (!ok) return false;
    if (s != state_) memcpy(state_, s, len);
    len_ = len;
    has_ = true;
    cur_ = static_cast<int32_t>(next);
    off_ = w * 4;
    closed_ = false;
    return true;
}

SettingsJournal::Result SettingsJournal::commit(uint8_t type, const uint8_t* payload, uint16_t plen,
                                                const uint8_t* s, uint16_t len, bool allowErase) {
    const uint32_t ps = flash_.pageSize();
    if (has_ && cur_ >= 0 && !closed_ && off_ + recordBytes(plen) <= ps) {
        const uint32_t w = encode(0, type, payload, plen);
        if (flash_.program(static_cast<uint32_t>(cur_) * ps + off_, buf_, w)) {
            off_ += w * 4;
            if (s != state_) memcpy(state_, s, len);
            len_ = len;
            return Result::WRITTEN;
        }
        closed_ = true;  // a write that didn't land: append nothing more here
    }
    // A new page carries the whole state (and the SYNC value).
    bool erased = false;
    if (!openPage(s, len, allowErase, &erased)) return Result::FAILED;
    return erased ? Result::WRITTEN_AFTER_ERASE : Result::WRITTEN;
}

SettingsJournal::Result SettingsJournal::append(const uint8_t* s, uint16_t len, bool allowErase) {
    if (len == 0 || len > MAX_STATE) return Result::FAILED;
    if (has_ && len == len_ && memcmp(s, state_, len) == 0) return Result::UNCHANGED;
    if (has_ && len == len_) {
        const uint16_t dlen = encodeDelta(s, len);
        if (dlen != 0xFFFF) return commit(TYPE_DELTA, payload_, dlen, s, len, allowErase);
    }
    return commit(TYPE_FULL, s, len, s, len, allowErase);
}

SettingsJournal::Result SettingsJournal::appendSync(uint32_t value, bool allowErase) {
    if (!has_) return Result::FAILED;
    if (syncValid_ && sync_ == value) return Result::UNCHANGED;
    const bool prevValid = syncValid_;
    const uint32_t prev = sync_;
    syncValid_ = true;
    sync_ = value;  // set first: a new page carries it
    uint8_t p[4];
    memcpy(p, &value, 4);
    const Result r = commit(TYPE_SYNC, p, 4, state_, len_, allowErase);
    if (r == Result::FAILED) {
        syncValid_ = prevValid;
        sync_ = prev;
    }
    return r;
}

bool SettingsJournal::prepare(const uint8_t* s, uint16_t len) {
    const uint32_t ps = flash_.pageSize();
    const uint32_t pages = flash_.pageCount();
    if (len == 0 || len > MAX_STATE) return false;
    if (append(s, len, true) == Result::FAILED) return false;
    // Room for the session: at least a quarter page in the current page.
    if (closed_ || off_ + ps / 4 > ps) {
        bool erased = false;
        if (!openPage(state_, len_, true, &erased)) return false;
    }
    bool ok = true;
    for (uint32_t p = 0; p < pages; p++) {
        if (static_cast<int32_t>(p) == cur_ || pageBlank(p)) continue;
        if (!erase(p) || !pageBlank(p)) ok = false;
    }
    return ok;
}

uint32_t SettingsJournal::freeBytes() const {
    const uint32_t ps = flash_.pageSize();
    uint32_t free = 0;
    for (uint32_t p = 0; p < flash_.pageCount(); p++) {
        if (static_cast<int32_t>(p) == cur_) {
            if (!closed_) free += ps - off_;
        } else if (pageBlank(p)) {
            free += ps;
        }
    }
    return free;
}

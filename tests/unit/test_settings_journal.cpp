/**
 * SettingsJournal unit tests (config/SettingsJournal.h) - native C++, no Arduino.
 *
 * blinky-firmware#49: a settings save must not erase flash while the loop
 * renders. The journal is run on a simulated NOR flash that enforces the
 * nRF52840's rules (program only clears bits; at most nWRITE=2 writes per
 * word between erases; erase sets a whole page to 0xFF) and can cut power
 * after any word. Checked:
 *   - after prepare() (boot), hundreds of typical saves need no erase;
 *   - every state read back equals the last committed save;
 *   - a power cut after any word of a save leaves the previous or the new
 *     state, never anything else, and the next boot's prepare() recovers;
 *   - deltas, FULL fallbacks, SYNC values, page rotation, the erase-when-full
 *     path and garbage pages.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../blinky-things/config/SettingsJournal.h"

static int g_total = 0;
static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        g_total++;                                                      \
        if (!(cond)) {                                                  \
            g_failed++;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

class NorSim : public JournalFlash {
public:
    static const uint32_t PS = 4096;
    explicit NorSim(uint32_t pages) : pages_(pages), mem_(pages * PS, 0xFF), writes_(pages * PS / 4, 0) {}
    const uint8_t* data() const override { return mem_.data(); }
    uint32_t pageSize() const override { return PS; }
    uint32_t pageCount() const override { return pages_; }
    bool erasePage(uint32_t page) override {
        if (dead()) return false;
        erases++;
        std::memset(&mem_[page * PS], 0xFF, PS);
        std::memset(&writes_[page * PS / 4], 0, PS / 4);
        return true;
    }
    bool program(uint32_t off, const uint32_t* words, uint32_t n) override {
        if (off % 4 != 0 || off + n * 4 > mem_.size()) { violations++; return false; }
        for (uint32_t i = 0; i < n; i++) {
            if (dead()) return false;
            if (failAfter == 0) {
                // The SoftDevice refused or timed out (no power loss): the
                // words before this one landed, this call reports failure.
                failAfter = -1;
                return false;
            }
            if (failAfter > 0) failAfter--;
            const uint32_t at = off + i * 4;
            if (++writes_[at / 4] > 2) violations++;  // nWRITE
            uint32_t cur;
            std::memcpy(&cur, &mem_[at], 4);
            if ((cur & words[i]) != words[i] && cur != 0xFFFFFFFFu) {
                // Programming can only clear bits; the journal must never ask
                // to set one (it writes erased words only).
                violations++;
            }
            const uint32_t v = cur & words[i];
            std::memcpy(&mem_[at], &v, 4);
            wordsWritten++;
            if (powerCutAfter >= 0 && --powerCutAfter < 0) cut = true;
        }
        return std::memcmp(&mem_[off], words, n * 4) == 0;
    }
    bool dead() const { return cut; }
    void powerOn() { cut = false; powerCutAfter = -1; }
    void fill(uint32_t page, uint8_t v) { std::memset(&mem_[page * PS], v, PS); }

    long powerCutAfter = -1;  // words until the power goes
    long failAfter = -1;      // words until one program() call fails (power stays on)
    bool cut = false;
    uint32_t erases = 0;
    uint32_t violations = 0;
    uint32_t wordsWritten = 0;

private:
    uint32_t pages_;
    std::vector<uint8_t> mem_;
    std::vector<uint8_t> writes_;
};

static const uint16_t N = 432;  // about sizeof(ConfigData)

struct Rng {
    uint32_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
};

static void mutate(uint8_t* st, Rng& r, int kind) {
    if (kind == 0) {  // one float, like `set drag 0.986`
        const uint32_t at = (r.next() % (N / 4)) * 4;
        const uint32_t v = r.next();
        std::memcpy(st + at, &v, 4);
    } else if (kind == 1) {  // a few scattered fields
        for (int i = 0; i < 5; i++) st[r.next() % N] = static_cast<uint8_t>(r.next());
    } else {  // everything (defaults reload)
        for (uint16_t i = 0; i < N; i++) st[i] = static_cast<uint8_t>(r.next());
    }
}

static bool sameAs(const SettingsJournal& j, const uint8_t* st) {
    return j.hasState() && j.stateLen() == N && std::memcmp(j.state(), st, N) == 0;
}

static void testFreshBootAndNoRuntimeErase() {
    NorSim f(3);
    SettingsJournal j(f);
    CHECK(!j.mount());
    uint8_t st[N];
    Rng r{12345};
    for (auto& b : st) b = static_cast<uint8_t>(r.next());
    CHECK(j.prepare(st, N));
    CHECK(sameAs(j, st));
    const uint32_t erasesAtBoot = f.erases;
    CHECK(erasesAtBoot == 0);  // blank flash: nothing to erase

    // A session of single-setting saves: none may erase.
    int saves = 0;
    for (;;) {
        uint8_t prev[N];
        std::memcpy(prev, st, N);
        mutate(st, r, 0);
        const SettingsJournal::Result res = j.append(st, N, false);
        if (res == SettingsJournal::Result::FAILED) {
            // Journal full: nothing written, the state is the last save.
            CHECK(sameAs(j, prev));
            std::memcpy(st, prev, N);
            break;
        }
        CHECK(res == SettingsJournal::Result::WRITTEN);
        saves++;
        CHECK(sameAs(j, st));
        if (saves > 5000) break;
    }
    std::printf("  single-setting saves before the journal needs an erase: %d\n", saves);
    CHECK(saves >= 400);
    CHECK(f.erases == erasesAtBoot);
    CHECK(f.violations == 0);

    // Reboot: the state is the last save; prepare erases the used pages.
    SettingsJournal j2(f);
    CHECK(j2.mount());
    CHECK(sameAs(j2, st));
    CHECK(j2.prepare(st, N));
    CHECK(sameAs(j2, st));
    CHECK(f.erases > erasesAtBoot);
    CHECK(j2.freeBytes() >= 2 * NorSim::PS + NorSim::PS / 4);
    CHECK(f.violations == 0);

    // After prepare, a session of saves again needs no erase.
    const uint32_t e = f.erases;
    for (int i = 0; i < 300; i++) {
        mutate(st, r, 0);
        CHECK(j2.append(st, N, false) == SettingsJournal::Result::WRITTEN);
    }
    CHECK(f.erases == e);
    SettingsJournal j3(f);
    CHECK(j3.mount());
    CHECK(sameAs(j3, st));
}

static void testUnchangedAndFullFallback() {
    NorSim f(3);
    SettingsJournal j(f);
    uint8_t st[N];
    Rng r{7};
    for (auto& b : st) b = static_cast<uint8_t>(r.next());
    CHECK(j.prepare(st, N));
    const uint32_t w = f.wordsWritten;
    CHECK(j.append(st, N, false) == SettingsJournal::Result::UNCHANGED);
    CHECK(f.wordsWritten == w);
    mutate(st, r, 2);  // whole state changes: a FULL record
    CHECK(j.append(st, N, false) == SettingsJournal::Result::WRITTEN);
    CHECK(f.wordsWritten - w >= N / 4);
    mutate(st, r, 1);
    CHECK(j.append(st, N, false) == SettingsJournal::Result::WRITTEN);
    // A different length (ConfigData grew in a new firmware): FULL.
    uint8_t big[N + 8];
    std::memcpy(big, st, N);
    std::memset(big + N, 0x5A, 8);
    CHECK(j.append(big, N + 8, false) == SettingsJournal::Result::WRITTEN);
    SettingsJournal j2(f);
    CHECK(j2.mount());
    CHECK(j2.stateLen() == N + 8 && std::memcmp(j2.state(), big, N + 8) == 0);
    CHECK(f.violations == 0);
}

static void testSync() {
    NorSim f(3);
    SettingsJournal j(f);
    uint8_t st[N] = {};
    CHECK(j.prepare(st, N));
    CHECK(!j.hasSync());
    CHECK(j.appendSync(0xDEADBEEF, true) == SettingsJournal::Result::WRITTEN);
    CHECK(j.appendSync(0xDEADBEEF, true) == SettingsJournal::Result::UNCHANGED);
    Rng r{99};
    // Fill past a page boundary: the SYNC value must ride along to the new page.
    for (int i = 0; i < 400; i++) {
        mutate(st, r, 0);
        CHECK(j.append(st, N, false) != SettingsJournal::Result::FAILED);
    }
    SettingsJournal j2(f);
    CHECK(j2.mount());
    CHECK(sameAs(j2, st));
    CHECK(j2.hasSync() && j2.syncValue() == 0xDEADBEEF);
    CHECK(j2.prepare(st, N));
    SettingsJournal j3(f);
    CHECK(j3.mount());
    CHECK(j3.hasSync() && j3.syncValue() == 0xDEADBEEF);
}

static void testEraseWhenFull() {
    NorSim f(3);
    SettingsJournal j(f);
    uint8_t st[N];
    Rng r{4242};
    for (auto& b : st) b = static_cast<uint8_t>(r.next());
    CHECK(j.prepare(st, N));
    int erasedSaves = 0;
    for (int i = 0; i < 3000; i++) {
        mutate(st, r, i % 7 == 0 ? 1 : 0);
        const SettingsJournal::Result res = j.append(st, N, true);
        CHECK(res == SettingsJournal::Result::WRITTEN || res == SettingsJournal::Result::WRITTEN_AFTER_ERASE);
        if (res == SettingsJournal::Result::WRITTEN_AFTER_ERASE) erasedSaves++;
    }
    CHECK(erasedSaves > 0);
    CHECK(erasedSaves == static_cast<int>(f.erases));
    SettingsJournal j2(f);
    CHECK(j2.mount());
    CHECK(sameAs(j2, st));
    CHECK(f.violations == 0);
}

static void testGarbageAndOldData() {
    NorSim f(3);
    f.fill(0, 0x00);
    f.fill(1, 0x37);
    f.fill(2, 0xA5);
    SettingsJournal j(f);
    CHECK(!j.mount());
    uint8_t st[N];
    Rng r{1};
    for (auto& b : st) b = static_cast<uint8_t>(r.next());
    CHECK(j.prepare(st, N));
    CHECK(f.erases == 3);
    SettingsJournal j2(f);
    CHECK(j2.mount());
    CHECK(sameAs(j2, st));
    CHECK(j2.freeBytes() >= 2 * NorSim::PS + NorSim::PS / 4);
}

// Cut the power after every word of a save, in the middle of a page and at
// the page rotation, then boot: the state is the old one or the new one, and
// prepare() leaves a journal that keeps working.
static void testPowerCuts() {
    Rng r{2026};
    int cases = 0, sawOld = 0, sawNew = 0;
    for (int scenario = 0; scenario < 3; scenario++) {
        // Build a journal, then position it: 0 mid-page delta, 1 FULL mid-page,
        // 2 the save that rotates to a new page.
        NorSim base(3);
        uint8_t st[N];
        for (auto& b : st) b = static_cast<uint8_t>(r.next());
        {
            SettingsJournal j(base);
            CHECK(j.prepare(st, N));
            for (int i = 0; i < 20; i++) { mutate(st, r, 0); j.append(st, N, false); }
            if (scenario == 2) {
                // Fill the page until the next delta won't fit.
                while (true) {
                    uint8_t t[N];
                    std::memcpy(t, st, N);
                    mutate(t, r, 0);
                    const int32_t page = j.currentPage();
                    // Probe on a copy: does this save rotate?
                    NorSim probe = base;
                    SettingsJournal jp(probe);
                    jp.mount();
                    jp.append(t, N, false);
                    if (jp.currentPage() != page) break;
                    std::memcpy(st, t, N);
                    j.append(st, N, false);
                }
            }
        }
        uint8_t next[N];
        std::memcpy(next, st, N);
        mutate(next, r, scenario == 1 ? 2 : 0);
        // Words the save writes, measured on a copy.
        uint32_t words;
        {
            NorSim probe = base;
            SettingsJournal jp(probe);
            CHECK(jp.mount());
            const uint32_t w0 = probe.wordsWritten;
            CHECK(jp.append(next, N, false) == SettingsJournal::Result::WRITTEN);
            words = probe.wordsWritten - w0;
        }
        for (uint32_t cutAt = 0; cutAt < words; cutAt++) {
            NorSim f = base;
            {
                SettingsJournal j(f);
                CHECK(j.mount());
                f.powerCutAfter = static_cast<long>(cutAt);
                j.append(next, N, false);  // power fails inside
            }
            f.powerOn();
            SettingsJournal j(f);
            CHECK(j.mount());
            const bool isOld = sameAs(j, st), isNew = sameAs(j, next);
            CHECK(isOld || isNew);
            sawOld += isOld;
            sawNew += isNew;
            // Boot maintenance, then the journal keeps taking saves.
            const uint8_t* now = isNew ? next : st;
            CHECK(j.prepare(now, N));
            uint8_t more[N];
            std::memcpy(more, now, N);
            for (int i = 0; i < 50; i++) {
                mutate(more, r, 0);
                CHECK(j.append(more, N, false) == SettingsJournal::Result::WRITTEN);
            }
            SettingsJournal j2(f);
            CHECK(j2.mount());
            CHECK(sameAs(j2, more));
            CHECK(f.violations == 0);
            cases++;
        }
    }
    std::printf("  power cuts: %d cases (old state %d, new state %d)\n", cases, sawOld, sawNew);
    CHECK(cases > 100);
    CHECK(sawOld > 0);
}

// A write that fails while the power stays on (a refused or timed-out
// SoftDevice operation, NrfJournalFlash) after any word of a save: the
// journal leaves the torn record behind, carries the state to a fresh page,
// keeps taking saves, and the next boot reads the newest state.
static void testRuntimeWriteFailure() {
    Rng r{77};
    NorSim base(3);
    uint8_t st[N];
    for (auto& b : st) b = static_cast<uint8_t>(r.next());
    {
        SettingsJournal j(base);
        CHECK(j.prepare(st, N));
        for (int i = 0; i < 10; i++) { mutate(st, r, 0); j.append(st, N, false); }
    }
    uint8_t next[N];
    std::memcpy(next, st, N);
    mutate(next, r, 0);
    int cases = 0;
    for (long failAt = 0; failAt < 4; failAt++) {  // a delta record is 4 words
        NorSim f = base;
        SettingsJournal j(f);
        CHECK(j.mount());
        const int32_t page = j.currentPage();
        f.failAfter = failAt;
        CHECK(j.append(next, N, true) == SettingsJournal::Result::WRITTEN);
        CHECK(f.failAfter == -1);  // the failure fired inside this save
        f.failAfter = -1;
        CHECK(j.currentPage() != page);  // carried to a fresh page, no erase needed
        CHECK(sameAs(j, next));
        uint8_t more[N];
        std::memcpy(more, next, N);
        for (int i = 0; i < 20; i++) {
            mutate(more, r, 0);
            CHECK(j.append(more, N, false) == SettingsJournal::Result::WRITTEN);
        }
        SettingsJournal boot(f);
        CHECK(boot.mount());
        CHECK(sameAs(boot, more));
        CHECK(boot.prepare(more, N));
        CHECK(f.erases == 1);  // the page with the torn record, at boot
        CHECK(f.violations == 0);
        cases++;
    }
    CHECK(cases == 4);
}

static void testLargestRuntimeWrite() {
    // The nRF backend splits writes into short chunks; here, check the
    // journal never asks for an erase from append(..., false) and that a FULL
    // record (the largest run-time write) is a few hundred bytes.
    NorSim f(3);
    SettingsJournal j(f);
    uint8_t st[N];
    Rng r{5};
    for (auto& b : st) b = static_cast<uint8_t>(r.next());
    CHECK(j.prepare(st, N));
    const uint32_t w = f.wordsWritten;
    mutate(st, r, 2);
    CHECK(j.append(st, N, false) == SettingsJournal::Result::WRITTEN);
    CHECK((f.wordsWritten - w) * 4 <= N + 8 + 4);
}

int main() {
    testFreshBootAndNoRuntimeErase();
    testUnchangedAndFullFallback();
    testSync();
    testEraseWhenFull();
    testGarbageAndOldData();
    testPowerCuts();
    testRuntimeWriteFailure();
    testLargestRuntimeWrite();
    std::printf("%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}

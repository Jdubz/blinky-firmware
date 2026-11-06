# blinky-firmware

Firmware for blinky's audio-reactive LED fixtures (XIAO nRF52840 Sense), and its approval and release flow.

- `blinky-things/`: the Arduino sketch: audio (`AudioTracker`, SuperFlux onsets, PLP rhythm), generators and effects, BLE, signed OTA.
- `bootloader/`: the fleet bootloader (submodule `bootloader/src`) and its deploy and verify scripts.
- `blinky-simulator/`: host builds of the renderer.
- `tests/`: host tests, among them the tracker harness with the corpus gate on `corpus/edm`.
- `tools/`, `scripts/`: the pinned toolchain, build, packaging and validators, releases, bench gate and promotion.
- `corpus/`: [blinky-corpus](https://github.com/Jdubz/blinky-corpus), the validation corpus and its scorers (submodule).
- `archive/`: sketches from before `blinky-things` (2017–2025), kept for reference; not built (see [SECURITY.md](SECURITY.md)).

The fleet server, web console, ML training and the test box live in [blinky_time](https://github.com/Jdubz/blinky_time). That repo pins this one as its `firmware/` submodule.

## Flow

Feature PRs target `staging`. Every merge to `staging` is built by CI (`staging-build.yml`) and run on the test box's chips. The box posts the `Hardware test (staging)` status and a report, and on a pass it opens and merges the PR to `master`. Every merge to `master` cuts a release candidate (`release.yml`). The owner promotes a candidate to stable (`tools/release_promote.py`), and only stable releases reach the fleet. Details: [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md).

## Build

```bash
git submodule update --init corpus          # the bootloader submodule is only for bootloader work
bash scripts/setup_toolchain.sh             # once per machine: the pinned, isolated toolchain
./scripts/build.sh --no-bump                # compile check
```

See [CLAUDE.md](CLAUDE.md) for the rules (upload safety, stable releases only, bootloader changes, audio changes), [docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md) and [TESTING.md](TESTING.md).

History comes from [blinky_time](https://github.com/Jdubz/blinky_time) (git filter-repo, 2026-11-04, blinky_time#357).

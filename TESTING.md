# Testing Guide

There are two kinds of tests:
- **Host tests** run firmware code on a PC (no device needed). CI runs them on every PR. See [tests/README.md](tests/README.md).
- **On-device tests** play music to real devices and score what they detect. They are managed through blinky-server (REST API) and the MCP tools.

## Testing Documentation

| Document | Purpose |
|----------|---------|
| [tests/README.md](tests/README.md) | Host tests (timing, OTA manifest, tracker harness, parity) and the CI jobs that run them |
| [docs/AUDIO-TUNING-GUIDE.md](docs/AUDIO-TUNING-GUIDE.md) | **Primary on-device guide** - Tunable parameters, test procedures, troubleshooting |
| [blinky_time docs/TESTING_METHODOLOGY.md](https://github.com/Jdubz/blinky_time/blob/master/docs/TESTING_METHODOLOGY.md) | Tiered testing approach, from the host harness up to full on-device validation |
| [blinky_time blinky-test-player/PARAMETER_TUNING_HISTORY.md](https://github.com/Jdubz/blinky_time/blob/master/blinky-test-player/PARAMETER_TUNING_HISTORY.md) | Historical calibration results |

The only validation corpus is `corpus/edm/` (18 tracks; the blinky-corpus submodule). The task is drum-hit detection (kicks and snares), not beat detection: score kick/snare recall and on-drum share against the drum labels, plus repetition consistency (`repS`) (see [CLAUDE.md](CLAUDE.md) and [docs/AUDIO-TUNING-GUIDE.md](docs/AUDIO-TUNING-GUIDE.md)).

## Quick Start

### Compile Check

```bash
# Verify firmware compiles (pinned toolchain; run scripts/setup_toolchain.sh once first)
./scripts/build.sh --no-bump
```

### Host Tests

```bash
# Rhythm tracker on the edm/ corpus, no device needed (build steps in tests/tracker/README.md)
python tests/tracker/run_tracker_eval.py --harness build/tracker_harness
```

### Run Tests via blinky-server API

Run these against test devices, not installed fleet devices.

```bash
# BLINKY_TESTBOX_URL is the test box's blinky-server, from ~/.config/blinky/hosts.env
# (read just that key, never source the file:
#  BLINKY_TESTBOX_URL="$(sed -n 's/^BLINKY_TESTBOX_URL=//p' ~/.config/blinky/hosts.env | tail -n 1)")
# Run validation suite (all tracks, single device)
curl -X POST "$BLINKY_TESTBOX_URL/api/test/validate" \
  -H 'Content-Type: application/json' \
  -d '{"device_ids": ["DEVICE_ID"], "track_dir": "/path/to/corpus/edm"}'

# Parameter sweep (3 devices, 3 values)
curl -X POST "$BLINKY_TESTBOX_URL/api/test/param-sweep" \
  -H 'Content-Type: application/json' \
  -d '{"device_ids": ["D1","D2","D3"], "param_name": "fluxdelta", "values": [6, 8, 10], "track_dir": "/path/to/corpus/edm"}'

# Poll for results
curl "$BLINKY_TESTBOX_URL/api/test/jobs/{job_id}"
```

### Run Tests via MCP

```
# Validation test (from Claude Code); port is a serial port or device ID
run_test(port: "/dev/ttyACM0")

# Full validation suite
run_validation_suite(ports: ["/dev/ttyACM0", "/dev/ttyACM1"])

# Poll results
check_test_result(job_id: "abc123")
```

See [docs/AUDIO-TUNING-GUIDE.md](docs/AUDIO-TUNING-GUIDE.md) for the full parameter reference.

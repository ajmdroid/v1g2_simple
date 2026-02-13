# Test Fixtures

This directory holds captured logs and JSON fixtures used by offline analysis
tools and CI checks.

## Current Usage

### Stability scorecard fixture (CI)

`test/fixtures/debug_base_stable.log` is used by `.github/workflows/scorecard.yml`.

Run locally:

```bash
python3 tools/scorecard.py test/fixtures/debug_base_stable.log
python3 tools/scorecard.py test/fixtures/debug_base_stable.log --json
```

### Log analysis fixtures

`tools/bench.py`, `tools/analyze_clusters.py`, and `tools/analyze_drive_log.py`
can be run against fixtures in this folder, for example:

```bash
python3 tools/bench.py test/fixtures/drive_session.log
python3 tools/analyze_clusters.py test/fixtures/drive_session.log
python3 tools/analyze_drive_log.py test/fixtures/drive_session.log
```

## Notes

- SD-backed `debug.log` capture and Dev-page log download are removed.
- Keep existing fixtures for repeatable offline analysis and regression checks.

# Building and verifying firmware

This project uses PlatformIO. The primary environment is `waveshare-349`.
Install PlatformIO via `pip install -U platformio` (or use the IDE/plugin of your choice) before running any commands.

## Quick commands

- `pio run -e waveshare-349`  
  Builds and links the main firmware. This is the baseline command used by CI and local development.
- `pio run -e waveshare-349 -t size`  
  Prints memory usage and shows the binary size for each object section. The `scripts/pio-size.sh` helper wraps this command for convenience.
- `pio check -e waveshare-349`  
  Runs PlatformIO's static analysis and sanity checks for the `waveshare-349` environment. Because the default `cppcheck` run trips over ArduinoJson headers, the helper script (`scripts/pio-check.sh`) instead invokes the same environment with `platformio_clangtidy.ini`, which switches the tool to `clang-tidy`.

## Helpers

Instead of typing the full commands every time, you can run the shell scripts that live under `scripts/`:

```sh
scripts/pio-size.sh
scripts/pio-check.sh
```

Each script exits immediately if the underlying PlatformIO command fails.

# Waveshare ESP32-S3-Touch-LCD-3.49 Notes

Lean reference for the hardware quirks used by this project.

## Pinout (matches `platformio.ini`)
- Display (QSPI): `CS=9`, `SCLK=10`, `DATA0=11`, `DATA1=12`, `DATA2=13`, `DATA3=14`, `RST=21`, `BL=8`
- Touch: Integrated AXS15231B at I2C address `0x3B` on `SDA=17` / `SCL=18` @ 400 kHz (reset shares `LCD_RST`; we leave reset unused)
- SD card: SDMMC 1-bit mode on `CLK=41`, `CMD=39`, `D0=40` (used by the alert logger)

## Quirks to remember
- Backlight PWM is inverted: `0` = full brightness, `255` = off
- The display is drawn via `Arduino_GFX` onto a 172×640 canvas and rotated to 640×172 landscape
- Touch reporting is tap-only; gesture support is not exposed by the AXS15231B example flow

## Quick troubleshooting
- **Backlight dark?** Write `0` to `LCD_BL` for max brightness (PWM is inverted)
- **Touch silent?** Confirm the device responds at `0x3B` on `SDA=17/SCL=18`; check the shared reset line
- **SD not mounting?** Ensure `CLK/CMD/D0` pins above are free and SDMMC is started in 1-bit mode

# SATulator

SATulator is a compact ESP32-2432S028 (2.8-inch CYD) Bitcoin/fiat converter built with PlatformIO, LVGL, and TFT_eSPI. It fetches configured public exchange prices when Wi-Fi is available, calculates average and median rates, and preserves downloaded rates plus board preferences in flash.

The startup screen shows the orange Bitcoin logo, SATulator title, project description, MIT license, and copyright information before the conversion interface loads.

## Features

- Live HTTPS exchange rates with configurable source lists for EUR, USD, CHF, GBP, and JPY
- Average, median, and editable custom conversion rates
- NTP-synchronized refresh timestamps when Wi-Fi is connected
- Full-screen source-price table and BTC/satoshi conversion modes
- Saved-rate-only Prices page: a fiat with no saved download displays `0.00`
- Persisted last fiat, direction, BTC/satoshi unit, rate selection, custom rate, and amount
- Configuration statistics for rate payload, board-state payload, and cumulative saved data

## Build and flash

```sh
pio run -t upload
pio device monitor
```

## CI and documentation

GitHub Actions builds the `esp32-2432s028` firmware on every push and pull
request, then exposes the binary, ELF, and linker map as downloadable workflow
artifacts for 30 days. Pushes to the default branch also publish the static
documentation in `docs/` through GitHub Pages.

The interface uses **fiat currency per 1 BTC** (for example, `100000`). Tap an input to open the numeric keypad. The arrow button switches between fiat-to-BTC/satoshi and BTC/satoshi-to-fiat conversions.

Changing fiat never downloads data automatically. Press **Refresh** for the active fiat or **Refresh all** in Configuration to download and save new rates.

## Hardware configuration

The bundled PlatformIO environment targets the common ESP32-2432S028 board: ILI9341 display, XPT2046 touch controller, and a 240x320 portrait display. It uses the fixed XPT2046 portrait mapping defined in `src/main.cpp`.

## Inspiration

SATulator is inspired by [btcpos](https://github.com/RCasatta/btcpos) and [fiat-converter](https://github.com/damianobonazzi/fiat-converter).

## License and copyright

SATulator is released under the [MIT License](LICENSE).

Copyright © 2026 Valerio Vaccaro.

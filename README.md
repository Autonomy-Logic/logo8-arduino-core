# TM4C1294 Ethernet platform (`autonomylogic:tm4c:logo8`)

An `arduino-cli` platform for a TI TM4C1294NCPDT board that uploads over
**Ethernet** (there is no serial/JTAG upload path). It layers a board
definition, a linker script that places the sketch above a resident loader,
and an Ethernet upload recipe on top of the TI/Energia Tiva core.

## Provenance

The Arduino API implementation (`cores/tiva`: Wiring, `millis`, `Serial`, the
on-chip-EMAC `Ethernet` library) and the `arm-none-eabi-gcc` toolchain come from
**[Andy4495/TI_Platform_Cores_For_Arduino](https://github.com/Andy4495/TI_Platform_Cores_For_Arduino)**
(`energia:tivac`). This repository is a fork of it; the core sources retain their
original TI/Energia copyright and license headers. Our changes are limited to the
board/variant definition, the upload recipe, and small core deltas noted in the
source.

## Layout

| Path | Purpose |
|---|---|
| `boards.txt` / `platform.txt` | the `logo8` board + compile/link/upload recipes |
| `variants/logo8/` | linker script + reset handler (sketch base + `VTOR`) |
| `tools/logo-upload.py` | Ethernet upload helper (stdlib UDP) |
| `package_autonomylogic_tm4c_index.json` | board-manager index |

## Build / upload

```
arduino-cli core install autonomylogic:tm4c --additional-urls <index-url>
arduino-cli compile -b autonomylogic:tm4c:logo8 --export-binaries <sketch>
arduino-cli upload  -b autonomylogic:tm4c:logo8 -p <device-ip> <sketch>
```

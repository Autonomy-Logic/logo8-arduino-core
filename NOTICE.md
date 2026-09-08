# NOTICE

This repository is a fork of
[Andy4495/TI_Platform_Cores_For_Arduino](https://github.com/Andy4495/TI_Platform_Cores_For_Arduino)
(the Energia `tivac` core). The Arduino-API core under `cores/tiva`, the
libraries under `libraries/`, and the driverlib under `system/` originate there
and retain their original TI / Energia / lwIP copyright and license headers (see
`LICENSE.txt` and the per-file headers).

Changes in this fork are limited to: the `logo8` board/variant definition
(`boards.txt`, `platform.txt`, `variants/logo8/`), an Ethernet upload helper
(`tools/`), and small deltas in the core (a monotonic `micros()` and a
board-specific SysTick start) noted inline in the sources.

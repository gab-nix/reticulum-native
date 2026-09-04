# Semtech SX126x driver provenance

- Upstream: https://github.com/Lora-net/sx126x_driver
- Version: `v2.5.0`
- Commit: `a10c5dfdf89788c6ac805e9fe98889de44175aa2`
- Source archive: https://github.com/Lora-net/sx126x_driver/archive/a10c5dfdf89788c6ac805e9fe98889de44175aa2.tar.gz
- Source archive SHA-256: `94c00f93c5ba73ffb1bc288078068d2119288354b4e0c9cfba403d8f223427c6`
- License: Clear BSD License, preserved verbatim in `LICENSE.txt`

Each included path in the table below can be retrieved directly by appending
it to this immutable raw-source prefix:

`https://raw.githubusercontent.com/Lora-net/sx126x_driver/a10c5dfdf89788c6ac805e9fe98889de44175aa2/`

Only the default SX126x command driver is included. The optional BPSK and
LR-FHSS sources are deliberately omitted because the initial Reticulum radio
profile uses LoRa modulation and does not enable those upstream build options.
The included files are unchanged from the pinned commit.

## SHA-256 checksums

| File | SHA-256 |
| --- | --- |
| `LICENSE.txt` | `a158fe2180a1429e59d7552ca3c31e4b312aca958423f3e98c15bb63fcb05ea9` |
| `src/sx126x.c` | `364465db57f5ebaf216934dc45676ff83a8cdee162a1d1b5b6da8cea61dfa4ca` |
| `src/sx126x.h` | `798e0aa7d773992371d82f747ebe66e00c6c0ec54b3b3a0f528822828e0d063f` |
| `src/sx126x_driver_version.c` | `c4bf85aac4e36a36d854edbf1ccd2535bfcf41a479d67afdde2fe176b2851ea0` |
| `src/sx126x_driver_version.h` | `25f2fb12dfb5b0c1b0f9116d962abd00f5a961c8235d66d3d8a0920818c75e5f` |
| `src/sx126x_hal.h` | `b70a5de4265a0074ab3c8577a4fd55f10634e02351fabc5c16f752cccc102d2c` |
| `src/sx126x_regs.h` | `dd64a28905140a9d6d21bc91c18db4fa039e24503d8534e333bf48300a3dfb28` |
| `src/sx126x_status.h` | `043780bfeecdfb9d22137cbac33521aa2c40d052e3338c74ff0ff86feb176235` |

Verify the vendored copy from the repository root with:

```sh
sha256sum third_party/semtech/sx126x_driver/LICENSE.txt \
  third_party/semtech/sx126x_driver/src/*
```

The driver provides radio command encoding and a HAL contract only. The
presence of these sources does not mean the Heltec SPI/IRQ/RF-switch/TCXO
backend or physical radio operation is implemented or verified.

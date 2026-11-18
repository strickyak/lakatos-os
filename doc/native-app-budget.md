# Native App (`native.bin`) Flash & RAM Budget

`native.bin` is the multi-headed native C/C++ application bundle built by `tools/build_lapp.py` and linked with `ld/lakatos_app.ld`. Each native app has a tiny LCode `.lapp` launcher stub (e.g. `apps/radio/radio.lgolf`) that calls `join_native("native.bin", "<head>")` to stage (if not already cached) and execute `native.bin` from the `app_slot` flash partition.

---

## 1. Flash Budget (`app_slot` Partition)

Defined in `partitions_lakatos.csv` and `include/lakatos_api.h`:

- **Physical Flash Partition (`app_slot`)**: `0x00190000 .. 0x001BFFFF`
- **Total Flash Budget (`LAKATOS_APP_FLASH_MAX_SIZE`)**: **`196,608 B`** (`192 KB`, `3 × 64 KB` MMU pages)
  - Dual-mapped at runtime into instruction space (`IROM` at `0x40300000`) and read-only data space (`DROM` at `0x3F700000`).
- **Current `native.bin` Size**: **`147,912 B`** (`144.4 KB` — **75.2%** used)
  - `LakatosAppHeader`: `128 B` (`0x00 .. 0x7F`)
  - `.text` (`IROM` `0x40300080`): `105,408 B` (`102.9 KB`)
  - `.rodata` (`DROM` `0x3F719C40`): `41,392 B` (`40.4 KB`)
  - `.data` (initial values in flash): `984 B` (`1.0 KB`)
- **Remaining Flash Headroom**: **`48,696 B`** (**`47.6 KB` free**, 24.8%)

---

## 2. Static RAM Budget (32 KB App DRAM Arena)

Defined in `include/lakatos_api.h` and `ld/lakatos_app.ld`:

- **Static Arena (`gLakatosAppRamArena`)**: `0x3FFC8000 .. 0x3FFCFFFF` (**`32,768 B` / `32 KB`**)
- **Non-overlaid Shared Overhead**: **`1,000 B`**
  - `0x3FFC8000 .. 0x3FFC8007` (`8 B`): `LakatosApi*` pointer + app argument pointer
  - `0x3FFC8008 .. 0x3FFC83DF` (**`984 B`**): `.data` (non-zero initialized globals across all heads + `libhelix-mp3` `640 B`, `radio` `190 B`, `ham` `147 B`)
  - Trailing common `.bss`: `8 B`
- **Overlaid `.bss` Budget per Native Head**: **`31,768 B`** (`~31.0 KB`, `0x3FFC83E0 .. 0x3FFCFFCF`)
  - Because only one native head runs at a time and Lakatos zeroes the entire 32 KB arena before entry, `ld/lakatos_app.ld` uses an `OVERLAY` block so each native head gets the full `~31.0 KB` `.bss` slice to itself rather than summing `.bss` across heads.
- **Dynamic Heap (`os->malloc()`)**: **`~85–110 KB`** of additional dynamic OS heap is available at runtime while a native app executes (automatically freed if leaked when the app exits).

---

## 3. Per-App Breakdown (7 Native Heads)

| Native App | Source | Flash (`.text + .rodata + .data`) | Static `.bss` (overlaid in `31,768 B` max) |
| :--- | :--- | ---: | ---: |
| **`smallertalk`** | `apps/smallertalk/*.cpp` | `63,217 B` (`61.7 KB`) | `30,384 B` (`29.7 KB`) |
| **`radio`** *(+ `libhelix-mp3`)* | `apps/radio/radio.cpp` | `7,922 B` + `~24.5 KB` helix ≈ **`~32.4 KB`** | `25,536 B` (`24.9 KB`) |
| **`modtrack`** | `apps/modtrack/modtrack.cpp` | `16,470 B` (`16.1 KB`) | `30,656 B` (`29.9 KB`) |
| **`midiplay`** | `apps/midiplay/midiplay.cpp` | `15,244 B` (`14.9 KB`) | `31,728 B` (`31.0 KB`) *(current max)* |
| **`ham`** | `apps/ham/ham.cpp` | `12,362 B` (`12.1 KB`) | `23,548 B` (`23.0 KB`) |
| **`ambient`** | `apps/ambient/ambient.cpp` | `5,104 B` (`5.0 KB`) | `5,680 B` (`5.5 KB`) |
| **`piano`** | `apps/piano/piano.cpp` | `3,182 B` (`3.1 KB`) | `0 B` |
| *`crt0` + `libgcc`/`libm` glue* | *(toolchain)* | *`~2.0 KB`* | *`8 B`* |
| **Total `native.bin`** | | **`147,912 B` / `196,608 B`** | **`31,736 B` / `31,776 B` max** |

---

## 4. Guidelines When Adding or Modifying Native Apps

1. **Add new heads to `OVERLAY` in `ld/lakatos_app.ld`**:
   - Any new native head added to `tools/build_lapp.py` **must** also have a `.bss.<head>` entry inside the `OVERLAY` block in `ld/lakatos_app.ld`. Otherwise its `.bss` will fall into the non-overlaid `.bss` catch-all section at the end (`0x3FFCFFD0`) and overflow `0x3FFD0000`!
2. **Avoid non-zero initialized globals (`.data`)**:
   - `.data` is **not** overlaid—every byte of `.data` shrinks the `.bss` ceiling for *all* native heads (and `midiplay` is currently within `40 B` of the `32 KB` arena end).
   - Use `constexpr` / `const` tables (which go into `.rodata` in DROM flash) or zero-initialized `.bss` structs initialized at the start of `app_main()`.
3. **Put large buffers on the heap (`os->malloc`)**:
   - Keep static `.bss` per head `<= 31,768 B`, and allocate larger audio/image/network buffers dynamically with `os->malloc()`.
4. **Inspecting exact section sizes**:
   ```bash
   python3 tools/build_lapp.py
   ~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-objdump -h build/apps/native.elf
   ```

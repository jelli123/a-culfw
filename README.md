# a-culfw for PlatformIO

This repository contains a port of the well-known *a-culfw* (alternative firmware for CUL devices) to the **PlatformIO** build system.

## Aim (Intent)

The goal of this project is to modernize and simplify the development and compilation of the firmware. Instead of relying on complex, hand-written `Makefiles`, this project uses PlatformIO for clean management of dependencies and hardware variants.

**Advantages:**
* **One-Click-Setup:** Automatic installation of the correct toolchains (AVR, STM32).
* **Centralized Management:** All finished firmwares are stored uniformly in the `/binaries` folder in the git root (one level above the code).
* **Automation:** A manifest (`manifest.json`) catalogues all builds including version, date, and hardware platform.

The port is partial on purpose: the hand written `makefile` in every
`culfw/Devices/*` directory still exists and is still the only way to build the
devices that have no PlatformIO environment yet.

## Supported Hardware

Currently, the following environments are defined:

1.  **nanoCUL868 / nanoCUL433** (ATmega328P)
2.  **CUL_V3** (ATmega32U4 with native USB)
3.  **MapleCUNx4_W5100_BL / MapleCUNx4_W5500_BL** (STM32F103CB, Quad-CC1101, Ethernet)
4.  **CUNO** (ATmega644P, ENC28J60 Ethernet; one image for 868 and 433 MHz)
5.  **CUBE_BL / CUBEx4_BL** (eQ-3 MAX! Cube, AT91SAM7X256, Ethernet; single
    and four-CC1101 variant, both for the USB mass storage bootloader)

Every other device in `culfw/Devices/` — COC, CUN, CUNO2, SCC,
megaCUL, miniCUL, CUL-Arduino and the rest — is built with its own `makefile`,
not with PlatformIO.

There is no ESP32 target. A native port was started in January 2026 and removed
again in February.

Two toolchains are in play and they are not the same compiler: PlatformIO pins
`toolchain-atmelavr@~1.50400.0` (avr-gcc 5.4.0), while the makefiles use
whatever `avr-gcc` is on the PATH (verified against 14.2.0). Code size differs
measurably between the two.

## Generating Firmware (Build)

By default, `pio run` builds the four AVR targets listed in `default_envs`. The ARM environments (MapleCUN, CUBe) must be selected explicitly.

### Build Commands in the Terminal

```bash
# Build the default AVR targets (CUL_V3, nanoCUL868, nanoCUL433, CUNO)
pio run

# Build one environment explicitly
pio run -e nanoCUL868
pio run -e MapleCUNx4_W5500_BL
pio run -e CUNO
pio run -e CUBE_BL -e CUBEx4_BL
```

The MapleCUN and CUBe environments need an **x86_64** host: on linux_aarch64
PlatformIO cannot resolve the ARM toolchain the `ststm32` platform asks for.

PlatformIO has no platform for the CUBe's AT91SAM7X256 (ARM7TDMI). Its
environments borrow `ststm32` for the arm-none-eabi toolchain and describe the
chip in `boards/cube.json`; `scripts/at91sam7.py` removes the Cortex-M flags
`ststm32` adds (`-mthumb`, `-fdata-sections`). The CUBe linker scripts place
`board_cstartup.o` first explicitly, because the bootloader only accepts an
image that starts with it. The AVR
targets build on both.

### Build profiles

`TTYSBU` builds a CUL_V3 that talks over a UART on the USB-C SBU pins instead
of USB — PlatformIO environment `CUL_V3_SBU`. Wiring, the runtime mode
detection and what it costs are described in
[`culfw/Devices/CUL/SBU_SUPPORT.md`](culfw/Devices/CUL/SBU_SUPPORT.md).

`SLIM_HM_BUILD` builds a CUL_V3 for HomeMatic only (BidCos + HmIP) and drops
the protocols such a stick does not need. On CUL_V3_868MHZ that is 15736 instead
of 28202 bytes of flash and 992 instead of 2345 bytes of RAM:

```bash
cd culfw/Devices/CUL
make TARGET=CUL_V3 FREQUENCE=_868MHZ MCU=atmega32u4 \
     FLASH_SIZE=32768 BOOTLOADER_SIZE=4096 \
     EXTRA_CFLAGS=-DSLIM_HM_BUILD mostly_clean build size
```

### The Result
After a successful build, all relevant files are located in the `/binaries` directory in the git root:
* `manifest.json`: Contains metadata for all built versions.
* `*.hex`: Firmware for AVR-based CULs.
* `*.bin`: Firmware for STM32 (DFU images).

## Flashing Firmware

### 1. nanoCUL (Arduino Nano)
```bash
avrdude -p atmega328p -c arduino -P /dev/ttyUSB0 -b 57600 -D -U flash:w:../binaries/nanoCUL868.hex:i
```

### 2. CUL V3 (ATmega32U4)
The CUL V3 must be in bootloader mode.
* **Web Flasher (Recommended):** Use the [busware CUL Flasher](https://prov.busware.de/culflasher/) to transfer the `.hex` file directly from the browser.
* **Manual (Linux/Mac):**
  ```bash
  dfu-programmer atmega32u4 erase
  dfu-programmer atmega32u4 flash ../binaries/CUL_V3.hex
  dfu-programmer atmega32u4 reset
  ```

### 3. CUNO (ATmega644P)
The CUNO carries an AVRProg-compatible (avr109) bootloader in the top 2 KiB of
its flash and is flashed over its serial port at 38400 baud. The bootloader
only runs when PD3 is pulled to ground at reset (START_SIMPLE in
Bootloader/main.c). The `B01` command does not help here: CUNO.c does not evaluate the
EEPROM flag it sets, so it only resets the device. Then:
```bash
avrdude -p atmega644p -c avr109 -P /dev/ttyUSB0 -b 38400 -U flash:w:../binaries/CUNO.hex:i
# or
pio run -e CUNO -t upload --upload-port /dev/ttyUSB0
```

### 4. CUBe (eQ-3 MAX! Cube)
Needs the a-culfw USB mass storage bootloader
(see [`culfw/Devices/CUBe/README.md`](culfw/Devices/CUBe/README.md) for
installing it with SAM-BA). Hold the button on the bottom while plugging in USB
(or send `B01` to a running a-culfw); D1 blinks four times a second. Then copy
`CUBE_BL.bin` (or `CUBEx4_BL.bin`) onto the drive it presents, or:
```bash
pio run -e CUBE_BL -t upload --upload-port /media/$USER/<drive>
```

#### Configuration page

The CUBe firmware serves a configuration page on port 80
(`culfw/clib/httpd.c`, enabled by `HAS_HTTPD` in its `board.h`): DHCP, IP
address, netmask, gateway, NTP server, the TCP port for the CUL protocol and
the time zone - the settings the `Wi*` commands write. Saving validates the
whole form first, then stores it and restarts the device.

The page can be protected with a password (HTTP Basic authentication, user
`admin`), set on the page itself. The EEPROM keeps only a salted SHA-256 hash;
five wrong passwords lock the page for 30 seconds.

The password protects this page, not the device: the TCP port accepts every
command without one, and plain HTTP sends the password unencrypted. A POST sent
from a page on another host is refused.

#### Allowed clients (IP whitelist)

Up to four addresses or networks (`192.168.1.0/24, 10.0.0.5`) can be allowed;
TCP and ICMP from any other sender are dropped without an answer - on every
port, the TCP port for the CUL protocol included, and ping. UDP is not filtered:
it only reaches the CUBe's own DHCP and NTP exchanges, whose servers need not be
on the list. An empty list allows everyone. It is set on the configuration page,
which refuses a list that leaves out the computer saving it, or with
`Wif192.168.1.0/24,10.0.0.5` (`Wif` alone clears it) and read back with `Rif`.
`Wif` takes effect at once, so over the network it can lock out the sender;
USB is never filtered. (`clib/ipfilter.c`, enabled by `HAS_IP_FILTER`.)

The whitelist checks the sender's address. From outside the local network that
is hard to fake over TCP, as the answers never reach the forger; inside it, a
device can take an allowed address. Clients that get their address by DHCP are
best allowed by network.

#### Resetting the access protection

Hold the button on the bottom for 10 seconds while the CUBe is running: this
removes the password and the whitelist, and D1 blinks fast. (Held at power-up,
the same button starts the bootloader instead.) The `e` factory reset removes
both as well.

## Repository Structure & Git

To keep the repository clean, only the final products in the `binaries/` folder are tracked. Temporary build files are ignored.

**Git Whitelist Principle:**
Only `.hex`, `.bin` and the `manifest.json` in the `/binaries/` folder are explicitly allowed in the `.gitignore`. All other artifacts in the `.pio/` folder remain local.

## Version Numbering

This project uses a dynamic versioning script:
* The **Base Version** is administered in `version.h`.
* The **Build Number** is automatically incremented (locally or via CI/CD).
* The `collect_binaries.py` script extracts this version and writes it directly into the global manifest.

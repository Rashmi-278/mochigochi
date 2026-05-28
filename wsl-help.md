# Flashing the ESP32-C3 from WSL2

Notes on everything that had to be fixed to get `make flash` working under
WSL2 (Ubuntu). WSL2 is a VM with its own kernel, so USB serial devices need
extra plumbing that doesn't exist on a native Linux/macOS host.

## 1. `clang-format` missing (for `make format` / `format-check`)

Not installed, and `apt` needs `sudo` we couldn't supply non-interactively.
Installed at user level instead — no root required:

```sh
pip install --user clang-format
```

(Installs `clang-format` into `~/.local/bin`. Apt alternative: `sudo apt install clang-format`.)

## 2. Forward the USB device into WSL (usbipd-win)

WSL2 can't see Windows USB devices by default. Use [`usbipd-win`](https://github.com/dorssel/usbipd-win):

```powershell
# Windows PowerShell (Admin) — one time:
winget install usbipd

# Each session: find the board, bind once, then attach.
usbipd list                                  # note the ESP32's BUSID, e.g. 2-4
usbipd bind   --busid 2-4                     # one-time per device
usbipd attach --wsl --busid 2-4 --auto-attach # --auto-attach survives re-enumeration
```

`--auto-attach` matters: the ESP32-C3's native USB re-enumerates often, and
without it the attach drops and won't come back on its own.

## 3. Load the CDC-ACM serial driver in WSL

The driver ships with the WSL kernel but isn't loaded by default, so no
`/dev/ttyACM*` node is ever created even after a successful attach:

```sh
sudo modprobe cdc_acm
```

Verify: `lsmod | grep cdc_acm`. Once loaded, an attached board shows up as
`/dev/ttyACM0` (confirm in `dmesg`: `cdc_acm 1-1:1.0: ttyACM0: USB ACM device`).

## 4. Stop the board reset-looping → force download mode

The board was re-enumerating every ~0.6s (dmesg showed device number climbing
into the 60s, each lasting <1s). That's a firmware crash / watchdog boot-loop —
the native USB drops on every reset, so usbipd can never hold the attach.

Fix: force the ROM bootloader (download mode), which doesn't run the crashing
firmware so USB stays stable long enough to flash:

> **Hold BOOT → tap RESET → release BOOT.**

The usbipd flapping log goes quiet and `/dev/ttyACM0` persists.

## 5. Makefile `PORT` autodetect was macOS-only

`PORT` defaulted to `ls /dev/cu.usbmodem*` (macOS naming). On Linux/WSL that
matched nothing, so `PORT` was empty and `arduino-cli upload` got malformed
args (`accepts at most 1 arg(s), received 2`). Patched the Makefile to also
detect Linux ports:

```make
PORT ?= $(shell ls /dev/cu.usbmodem* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1)
```

## 6. Permission denied on `/dev/ttyACM0`

usbipd-attached devices come up owned `root:root` mode `600` (the udev rule
that normally assigns them to the `dialout` group doesn't fire under WSL), so
even a `dialout`-group user can't open the port. Quick fix before each flash:

```sh
sudo chmod 666 /dev/ttyACM0
make flash
```

The `chmod` resets every time the device re-attaches (new node). For a
permanent fix, add a udev rule so every `ttyACM*` comes up world-accessible:

```sh
echo 'KERNEL=="ttyACM[0-9]*", MODE="0666"' | sudo tee /etc/udev/rules.d/99-ttyacm.rules
sudo udevadm control --reload-rules
```

(udev rules only fire if udev/systemd is actually running in your WSL distro.)

## TL;DR — flashing from a cold start

```powershell
# Windows (Admin):
usbipd attach --wsl --busid 2-4 --auto-attach
```
```sh
# WSL:
sudo modprobe cdc_acm           # once per boot
# Hold BOOT, tap RESET, release BOOT  (puts board in download mode)
ls /dev/ttyACM*                 # confirm /dev/ttyACM0 is present & stable
sudo chmod 666 /dev/ttyACM0
make flash
```

## Caveats

- WSL flashing is finicky because of the ESP32-C3's USB re-enumeration during
  reset. If it keeps fighting you, flashing from **native Windows** arduino-cli
  is the more reliable path — it skips the usbipd hop entirely.
- The Makefile's `upload` target has a macOS `gochi`/`launchctl` block to
  release the serial port before flashing. On Linux those commands don't exist,
  so the block harmlessly no-ops.

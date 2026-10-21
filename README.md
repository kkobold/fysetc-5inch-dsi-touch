# fysetc-5inch-dsi-touch

Touch and backlight drivers for the **FYSETC Raspberry Pi 5 inch DSI Touch Screen MIPI LCD V1.1** on Raspberry Pi OS Bookworm, shipped as a `.deb` package.

> [!WARNING]
> **This screen impersonates the official Raspberry Pi 7" touchscreen.**
> The FYSETC video controller and touch controller emulate the official DSI hardware and identify themselves exactly like it. There is no unique ID to tell them apart.
> Because of that, this package **disables the stock drivers (`edt_ft5x06` and `rpi_panel_attiny_regulator`)**. While it is installed, **the official 7" display will not work correctly, and other FT5x06 touchscreens will not work**. The two cannot work on the same system. Only install it if you use the FYSETC screen.
>
> The stock drivers stay disabled even if the modules cannot be built for a kernel (for example, a new kernel without its headers). That kernel then boots **without video on the DSI screen**; see [Troubleshooting](#troubleshooting).

## The problems

After recent Raspberry Pi OS updates (6.x kernels), the FYSETC video keeps working, but the touch and the backlight do not.

### Touch

With the stock drivers:

- **With the modern driver (`edt_ft5x06`)**, there is a continuous stream of ghost touches, even when nobody touches the screen.
- **With the legacy firmware path (`rpi-ft5406`)**, the ghost touches go away, but the coordinates are wrong, mostly on Y.

The cause is in the hardware. An MCU on the FYSETC board answers at I²C address `0x38` pretending to be a FocalTech FT5x06 controller, but its read implementation is incomplete:

- The first byte of every read comes from the right register.
- The following bytes come from a 3-entry ring, with an internal pointer that advances with every byte read and **is never reset**.

The stock drivers read long blocks and get misaligned data. That is why no calibration, Xorg, libinput or KlipperScreen tweak can fix it.

### Backlight

After the screen goes to sleep, it stays dark. The panel controller at `0x45` only turns the backlight on with `0xff` and ignores any other brightness, which the stock driver (`rpi_panel_attiny_regulator`) writes.

## The fix

### Touch: `fysetc_touch`

The `fysetc_touch` kernel module talks to the controller the way it actually works:

- it reads the touch count with 1-byte reads;
- it reads each point with reads of exactly 4 bytes, which keeps the ring aligned;
- it validates the alignment on every read and corrects it if it changes;
- it honours the overlay's `touchscreen-*` properties (size, inversion, swap) and registers a standard Linux multi-touch touchscreen, which libinput, Xorg and KlipperScreen pick up with no extra configuration.

### Backlight: `fysetc_sleep`

The `fysetc_sleep` kernel module replaces `rpi_panel_attiny_regulator` at `0x45` with an on/off backlight. Brightness keeps the `0`–`255` range: below 30% (`0`–`76`) is off, `77`–`255` is on at full brightness. Touch keeps working while the backlight is off.

### Packaging

The modules are built on the Pi itself, for every installed Raspberry Pi kernel (Raspberry Pi OS ships both `rpi-v8` and `rpi-2712`, so the same package covers the Pi 3, 4 and 5 kernels). A kernel hook rebuilds them automatically whenever `apt` installs a new kernel. A failed build is logged and never breaks `apt`.

The package only adds:

- the module sources in `/usr/src/fysetc-touch/`;
- the `fysetc-touch-build` tool and the kernel hooks in `/etc/kernel/{header_postinst.d,postinst.d,postrm.d}/fysetc-touch`;
- the built modules in `/lib/modules/<kernel>/updates/fysetc_touch/`;
- `/lib/modprobe.d/fysetc-touch.conf`, which blacklists `edt_ft5x06` and `rpi_panel_attiny_regulator`.

It also regenerates the initramfs on install and removal. It does not modify any other system file, and removing the package deletes all of the above. You edit `config.txt` yourself (step 2).

## Requirements

- Raspberry Pi OS **Bookworm 64-bit** (arm64), kernel 6.1 or newer.
- Screen connected to the standard DSI connector.
- Tested on a Raspberry Pi 3 Model B+ with kernel `6.12.109+rpt-rpi-v8`. Other models are untested; reports are welcome.
- The Raspberry Pi kernel headers (`linux-headers-rpi-v8`, `linux-headers-rpi-2712`) and `make`. `apt` installs them as dependencies; most Raspberry Pi OS images already have them. Keep the headers installed: every new kernel needs its headers to build the modules.

## Installation

### 1. Download and install the package

Get the latest `.deb` from [Releases](https://github.com/kkobold/fysetc-5inch-dsi-touch/releases):

```bash
VERSION=0.2.0
wget https://github.com/kkobold/fysetc-5inch-dsi-touch/releases/download/v${VERSION}/fysetc-5inch-dsi-touch_${VERSION}_arm64.deb
sudo apt install ./fysetc-5inch-dsi-touch_${VERSION}_arm64.deb
```

The installation builds the modules on the Pi for each installed kernel, which takes a few seconds per kernel, and regenerates the initramfs. Check the result with:

```bash
sudo fysetc-touch-build status
```

### 2. Edit `/boot/firmware/config.txt`

The driver needs **KMS** video and the DSI screen overlay **with touch enabled**.

Remove or comment out these lines if they are present. They are common in older FYSETC guides:

```ini
#dtoverlay=vc4-fkms-v3d
#dtoverlay=rpi-ft5406
#dtoverlay=vc4-kms-dsi-7inch,disable_touch
```

Add at the end of the file:

```ini
[all]
display_auto_detect=0
disable_touchscreen=1
dtoverlay=vc4-kms-v3d
dtoverlay=vc4-kms-dsi-7inch
```

If `dtoverlay=vc4-kms-v3d` is already in the file (the Raspberry Pi OS default), don't repeat it.

`disable_touchscreen=1` stops the firmware from also reading the touch controller. Two concurrent readers would scramble the controller's pointer.

### 3. Reboot and check

```bash
sudo reboot
```

After boot:

```bash
dmesg | grep fysetc_touch
grep -A4 'FYSETC 5inch DSI Touch' /proc/bus/input/devices
ls -l /sys/bus/i2c/devices/*-0045/driver
```

The input device should show up as **`FYSETC 5inch DSI Touch`**, and the `0x45` driver should be **`fysetc_sleep`**. To watch raw touch events: `sudo evtest`.

## Rotation

The FYSETC touch has the same native orientation as the official display, and the `vc4-kms-dsi-7inch` overlay already inverts both axes by default. With the plain `dtoverlay=vc4-kms-dsi-7inch` line, touch matches the image.

If you rotate the display, use the overlay's own touch parameters, exactly as with the official display:

```ini
dtoverlay=vc4-kms-dsi-7inch,invx,invy      # 180°: removes the default inversion
dtoverlay=vc4-kms-dsi-7inch,swapxy         # swap X/Y
```

## Uninstall

```bash
sudo apt remove fysetc-5inch-dsi-touch
sudo reboot
```

Removal deletes the modules, the kernel hooks and the blacklist, and regenerates the initramfs. `edt_ft5x06` and `rpi_panel_attiny_regulator` work again on the next boot. Revert the step 2 changes too if you want.

## Troubleshooting

- **No video on the DSI screen:** the modules were not built for the running kernel. Log in over SSH and run `sudo fysetc-touch-build status`:
  - `no headers`: install the matching `linux-headers-rpi-*` package; the kernel hook builds the modules. Then reboot.
  - `failed (<log>)` or `installed (last build failed: <log>)`: the log shows the build error.
  - To get the stock drivers back meanwhile: `sudo apt remove fysetc-5inch-dsi-touch` and reboot.
- **Installation warns `update-initramfs failed`:** until the initramfs is regenerated, the stock panel driver can still load from it, and the screen may stay dark after sleep. Fix the cause (usually a full `/boot`), run `sudo update-initramfs -u -k all` and reboot.
- **Removal warns `update-initramfs failed`:** run `sudo update-initramfs -u -k all` and reboot; until then the stock drivers stay blacklisted.
- **No `FYSETC 5inch DSI Touch` device:** check `dmesg | grep fysetc_touch` and `sudo fysetc-touch-build status`, as above.
- **`no answer at 0x38` or a dead backlight at boot:** check the power supply: `vcgencmd get_throttled` should print `throttled=0x0`; bits `0x1`/`0x10000` mean undervoltage. A weak supply can hang the screen's I²C bus, and only a full power cycle (unplug and replug) recovers it.
- **Touch on the wrong side or mirrored:** see [Rotation](#rotation).
- **Screen stays dark after sleep:** if the `0x45` driver is `rpi_touchscreen_attiny`, run `sudo update-initramfs -u -k all` and reboot.

## License

MIT, see [`LICENSE`](https://github.com/kkobold/fysetc-5inch-dsi-touch/blob/main/LICENSE). Source files with an `SPDX-License-Identifier` header are licensed as stated in that header; the GPL-2.0 text is in [`COPYING`](https://github.com/kkobold/fysetc-5inch-dsi-touch/blob/main/COPYING). In the installed package, see `/usr/share/doc/fysetc-5inch-dsi-touch/copyright`.

# libfprint with Enhanced Goodix TLS 52XD Driver

[![License: LGPL v2.1](https://img.shields.io/badge/License-LGPL_v2.1-blue.svg)](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html)
[![libfprint: v1.94.100](https://img.shields.io/badge/libfprint-v1.94.100-green.svg)](https://gitlab.freedesktop.org/libfprint/libfprint)
[![Hardware: 27c6:521d](https://img.shields.io/badge/hardware-27c6%3A521d-orange.svg)](https://linux-hardware.org/?id=usb:27c6-521d)

Open-source [libfprint][Upstream] (`v1.94.100`) fork featuring an enhanced, hardened driver for the **Goodix TLS 52XD (`27c6:521d`)** fingerprint sensor.

Commonly found on modern laptops such as:
- **ASUS ROG Zephyrus G14 / G15 / G16**
- **ASUS ZenBook & VivoBook series**
- Various Dell, HP, and Lenovo models equipped with Goodix TLS GM168SEC / 521d sensors.

---

## Key Enhancements & Fixes

Previous community drivers for the Goodix 52XD suffered from high False Acceptance Rates (impostor fingers unlocking the system) and frequent "No Match" rejections on genuine touches. This fork thoroughly overhauls the signal and minutiae processing pipeline:

### 1. Zero False Accept Rate (FAR) via Hardware Defect Filtering
* **The Root Cause:** Analysis of raw sensor output revealed **7 static hardware defective pixels** on the sensor die (`(41, 114)`, `(52..53, 78)`, `(56, 90)`, `(98, 74)`, `(104, 70)`, `(48, 132)`, `(68, 140)`). Because these physical defects existed on every scan, Bozorth matching matched them against each other, allowing impostor fingers to score up to **19** (surpassing typical matching thresholds).
* **The Solution:** Added static defect suppression in [`libfprint/fpi-print.c`](libfprint/fpi-print.c). Defect points are excised during minutiae conversion.
* **Result:** Offline evaluation against extensive capture datasets confirmed **maximum impostor Bozorth scores dropped from 19 to $\le 3$**, guaranteeing a **0.0% False Accept Rate** at threshold 10.

### 2. Multi-Frame ADC Noise Cancellation Averaging
* Older drivers inherited a swipe-style frame assembler (`fpi_do_movement_estimation`) meant for swipe sensors, corrupting static press captures.
* Replaced with a **5-frame temporal average** on press to cancel out high-frequency analog-to-digital converter (ADC) noise.

### 3. Bilinear Upscaling for NIST NBIS Mindtct
* The physical sensor die is compact (64×80 pixels at ~500 DPI). The NIST NBIS `mindtct` minutiae detection algorithm expects analysis blocks of 24×24 pixels, severely limiting detectable minutiae on tiny images.
* Implemented a **2×2 bilinear upscale to 128×160 pixels**, allowing `mindtct` to smoothly track ridge flows and extract 12–25 valid minutiae per touch.

### 4. Active Interior Percentile Contrast Squashing & Border Clamping
* Clamps the physical sensor boundary ramp-to-zero (rows 0–3, 76–79; cols 0–2, 61–63) to prevent false edge minutiae.
* Applies dynamic percentile contrast stretching (2%–98% percentile) strictly over active interior pixels (`[4:76, 3:60]`).

### 5. Calibrated Threshold & Minutiae Bound
* Configured `bz3_threshold = 4` calibrated for the compact 64×80 sensor die (with impostors capped at $\le 3$, this guarantees 0.0% FAR while ensuring genuine touches reliably unlock).
* Lowered `MIN_COMPUTABLE_BOZORTH_MINUTIAE` from 10 to 5 in [`bozorth.h`](libfprint/nbis/include/bozorth.h) so partial touches with 6–9 minutiae are still scored accurately instead of silently discarded.

### 6. 16-Stage Comprehensive Paced Enrollment & Auto-Override
* Set `nr_enroll_stages = 16` and added a 1.8-second finger-off pacing delay to give users ample time and prompts to record flat, tip, side-tilted, and pad angles.
* When verifying, `libfprint` evaluates touches against all 16 templates, ensuring instant recognition from any angle.

---

## Building and Installation

### Prerequisites (Arch Linux / Fedora / Ubuntu)

#### Arch Linux:
```bash
sudo pacman -S meson ninja gcc pkgconf glib2 pixman libgusb openssl nss polkit fprintd
```

#### Ubuntu / Debian:
```bash
sudo apt install meson ninja-build gcc pkg-config libglib2.0-dev libpixman-1-dev libgusb-dev libssl-dev libnss3-dev libpolkit-gobject-1-dev fprintd
```

### Build Just the Goodix 52XD Driver

```bash
meson setup build -Ddrivers=goodixtls52xd -Dintrospection=false -Ddoc=false -Dgtk-examples=false
ninja -C build
```

### Install & Restart Service

```bash
# Backup existing library
sudo cp /usr/lib/libfprint-2.so.2.0.0 /usr/lib/libfprint-2.so.2.0.0.bak

# Install new library
sudo cp build/libfprint/libfprint-2.so.2.0.0 /usr/lib/libfprint-2.so.2.0.0

# Restart fprintd
sudo systemctl restart fprintd.service
```

---

## Usage

### 1. Enroll Your Finger

Run the enrollment command and touch the sensor 16 times at varying angles (flat, tip, edges):
```bash
fprintd-enroll -f right-index-finger
```

### 2. Verify Your Finger

Test verification with:
```bash
fprintd-verify
```

### 3. Optional: Auto-Override Wrapper

To automatically overwrite existing fingerprints during `fprintd-enroll` without having to run `fprintd-delete` manually:

```bash
sudo mv /usr/bin/fprintd-enroll /usr/bin/fprintd-enroll.real
sudo tee /usr/bin/fprintd-enroll > /dev/null << 'EOF'
#!/usr/bin/env bash
finger="right-index-finger"
user=""
is_help=0
for ((i=1; i<=$#; i++)); do
    arg="${!i}"
    if [[ "$arg" == "-h" || "$arg" == "--help" ]]; then is_help=1; break;
    elif [[ "$arg" == "-f" || "$arg" == "--finger" ]]; then
        next_i=$((i+1)); [[ $next_i -le $# ]] && finger="${!next_i}"
    elif [[ "$arg" =~ ^--finger=(.*)$ ]]; then finger="${BASH_REMATCH[1]}"
    elif [[ "$arg" != -* ]]; then
        prev_i=$((i-1))
        [[ $prev_i -ge 1 && ("${!prev_i}" == "-f" || "${!prev_i}" == "--finger") ]] && continue
        user="$arg"
    fi
done
if [[ $is_help -eq 0 ]]; then
    target_user="${user:-$(whoami)}"
    /usr/bin/fprintd-delete "$target_user" -f "$finger" >/dev/null 2>&1 || true
    sleep 0.5
fi
exec /usr/bin/fprintd-enroll.real "$@"
EOF
sudo chmod 755 /usr/bin/fprintd-enroll
```

---

## Offline Testing Suite

An offline verification tool (`verify_offline.c`) is included to evaluate genuine vs. impostor captures and verify Bozorth scores without needing hardware interaction:

```bash
gcc -O2 -Ilibfprint/nbis/include -Ilibfprint/nbis/libfprint-include -Ilibfprint \
    $(pkg-config --cflags glib-2.0 gio-2.0 pixman-1) \
    verify_offline.c build/libfprint/libnbis.a \
    $(pkg-config --libs glib-2.0 gio-2.0 pixman-1) -lm -o verify_offline

./verify_offline probe.pgm gallery.pgm --invert
```

---

## Credits & Provenance

This driver builds on the work of the open-source community:
- **[libfprint][Upstream]** by the freedesktop.org libfprint team.
- **[infinytum/libfprint][Infinytum]** for the original reverse engineering of the Goodix TLS protocol and driver implementation.
- **[arsfeld/libfprint-goodixtls][Arsfeld]** for rebasing community drivers onto `v1.94.100` and adding GM168SEC firmware support.
- **[mudoker/libfprint-goodix-521d][Mudoker]** for sensor defect artifact suppression, multi-frame ADC averaging, 2x bilinear pipeline, and NBIS tuning for the 52XD sensor.

[Upstream]: https://gitlab.freedesktop.org/libfprint/libfprint
[Infinytum]: https://github.com/infinytum/libfprint
[Arsfeld]: https://github.com/arsfeld/libfprint-goodixtls
[Mudoker]: https://github.com/mudoker/libfprint-goodix-521d

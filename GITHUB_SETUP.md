# Getting Started with GitHub Actions (No PC Needed)

This guide walks you through putting the KAOS code on GitHub and
downloading the compiled firmware files — all from your phone.

---

## Step 1 — Create a GitHub account

If you don't have one: go to **github.com** → Sign up.
Free account is all you need.

---

## Step 2 — Create a new repository

1. Tap the **+** icon (top right) → **New repository**
2. Name it something like `kaos-portal`
3. Set it to **Private** (so your code isn't public)
4. Leave everything else as default
5. Tap **Create repository**

---

## Step 3 — Upload the code

GitHub lets you upload files directly from the browser.

1. Open your new repository
2. Tap **Add file** → **Upload files**
3. You need to recreate the folder structure. GitHub's web uploader
   handles this — just drag the extracted folder, or upload files
   one folder at a time.

**Folder structure to upload:**
```
.github/
  workflows/
    build.yml          ← the workflow file
esp32/
  CMakeLists.txt
  sdkconfig.defaults
  main/
    main.c
    web_ui.c
    web_ui.h
    pico_bridge.c
    pico_bridge.h
    Skylander.c
    Skylander.h
    SkylanderCrypt.c
    SkylanderCrypt.h
    rijndael.c
    rijndael.h
    skylander_ids.h
    kaos_protocol.h
    CMakeLists.txt
pico/
  main.c
  skylander_slots.c
  skylander_slots.h
  usb_descriptors.c
  usb_descriptors.h
  SkylanderCrypt.c
  SkylanderCrypt.h
  rijndael.c
  rijndael.h
  kaos_protocol.h
  tusb_config.h
  CMakeLists.txt
  pico_sdk_import.cmake
kaos_protocol.h
README.md
```

> **Tip:** On Android, use the **Working Copy** or **Pocket Git** app.
> On iPhone, use **Working Copy** — it can upload whole folder structures
> to GitHub easily.

---

## Step 4 — Watch the build run

1. Once you've uploaded the files and committed, tap the **Actions** tab
   at the top of your repository
2. You'll see a workflow called **Build KAOS Firmware** running
3. There are three jobs — a yellow circle means running, green ✓ means done
4. The whole build takes about **5–8 minutes**

If anything goes red ✗, tap it to see the error log.

---

## Step 5 — Download the firmware

1. Once all jobs are green, tap the completed workflow run
2. Scroll down to the **Artifacts** section
3. You'll see **kaos-firmware-bundle** — tap it to download
4. You get a `.zip` file containing:
   - `pico/kaos_pico.uf2` — flash this to the Pi Pico
   - `esp32/kaos_esp32_dual.bin` + bootloader files — flash to ESP32
   - `HOW_TO_FLASH.txt` — exact flash commands

---

## Step 6 — Flash the Pico (from phone)

The Pico shows up as a USB drive when in bootloader mode.

**On Android:**
- Use an OTG adapter (USB-C to USB-A)
- Hold BOOTSEL on the Pico, plug it into your phone via OTG
- It should appear as a USB storage device
- Copy `kaos_pico.uf2` onto it using a file manager app
- The Pico reboots automatically

**On iPhone:**
- iPhones can't write to USB storage devices directly
- You'll need a PC, Mac, or Raspberry Pi just for this one step
- OR: buy a cheap Windows tablet / ask a friend

---

## Step 7 — Flash the ESP32 (from phone)

This needs `esptool.py`. The easiest mobile option:

**Android — use Termux:**
```bash
pkg install python
pip install esptool
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  write_flash \
  0x1000  bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 kaos_esp32_dual.bin
```
You'll need an OTG adapter and the right USB serial driver.

**Easier alternative — ESP Flash Tool (Windows only)**
If you have any Windows machine available even briefly, the
[Espressif Flash Download Tool](https://www.espressif.com/en/support/download/other-tools)
has a simple GUI — just point it at the three .bin files.

---

## Making changes later

If you edit any code file on GitHub (tap the file → pencil icon),
the build automatically re-runs and produces new firmware files.
You never need to touch a compiler yourself.

---

## Triggering a build manually

Go to **Actions** tab → **Build KAOS Firmware** → **Run workflow**
(grey button on the right) → **Run workflow**.
Useful if you want to rebuild without changing any code.

# M5Stack Paper S3 EPUB Reader Guide

This guide explains how to install and use the EPUB reader firmware on the **M5Stack Paper S3**, with a focus on the reading experience and the in-book reader menu.

---

## 1. Requirements

- **Hardware**
  - M5Stack **Paper S3**.
  - USB‑C cable (data‑capable).
  - microSD card (8–32 GB, **FAT32** formatted).

- **Software (developer setup)**
  - [VS Code](https://code.visualstudio.com/) + [PlatformIO](https://platformio.org/install/ide?install=vscode).
  - Git (if you build from source).

If you have a prebuilt firmware `.bin`, you can use `esptool.py` or the M5 burning tool instead of PlatformIO, but the steps below assume a standard PlatformIO build.

---

## 2. Preparing the SD Card

1. **Format** the microSD card as **FAT32**.
2. Create the following directories at the root of the card:

   - `/Books`  – EPUB files go here.
   - `/Images` – optional sleep images (PNG/JPEG).
   - `/Pics`   – optional extra sleep images (also PNG/JPEG).
   - `/fonts`  – optional TTF font(s) for the reader.

3. Copy your content:

   - Put `.epub` files under `/Books`.
   - Put any cover / artwork images you want to use for random sleep images in `/Images` or `/Pics`.
   - (Optional) Copy a TTF font to:

     ```
     /fonts/reader.ttf
     ```

     When available, this font is used for the main reading view; otherwise the built‑in bitmap font is used.

4. Safely eject the SD card and insert it into the **Paper S3**.

---

## 3. Flashing the Firmware

There are two typical scenarios:

- **Developer install (PlatformIO)** – you build and flash from source.
- **End‑user install (prebuilt release)** – you download a ready‑made `.bin` bundle and flash it with `esptool.py` or the M5 burning tool.

### 3.1 Developer install with PlatformIO

1. **Clone / open the project** in VS Code.
2. Install the **PlatformIO** extension if it is not already installed.
3. Connect the Paper S3 to your computer via **USB‑C**.
4. In the VS Code **status bar**, choose the PlatformIO environment for the Paper S3, for example:

   - `paper_s3_idf` (for debug builds), or
   - `paper_s3_release` (for optimised release builds).

5. From the PlatformIO sidebar or status bar, run:

   - **Build** – to compile the firmware for the selected environment.
   - **Upload** – to flash the firmware onto the Paper S3.

The board will reset after flashing. On first boot the firmware:

- Mounts the SD card as `/fs`.
- Scans `/fs/Books` and builds an index of available EPUBs.
- Shows the **library view** (grid of covers or list, depending on your settings).

### 3.2 End‑user install with a prebuilt release (esptool.py)

If you have downloaded a **prebuilt Paper S3 release** from the project’s `/releases` folder, you will typically receive a set of ESP‑IDF images for the **`paper_s3_release`** environment, for example:

- `bootloader.bin`
- `partitions.bin` (or `partition-table.bin`)
- `firmware.bin` (main app image built for Paper S3)

These are usually placed together in a folder such as:

```text
releases/paper_s3_YYYYMMDD/
```

To flash them with `esptool.py`:

1. **Install esptool.py** (if you do not already have it):

   ```bash
   pip install esptool
   ```

2. Put the Paper S3 into download/flash mode (follow M5Stack’s instructions) and connect it via USB‑C.

3. Identify the serial port (for example `/dev/tty.usbserial-*` on macOS, `/dev/ttyUSB0` on Linux, or `COM3` on Windows).

4. From the `releases/paper_s3_…` folder, run a command similar to:

   ```bash
   esptool.py \
     --chip esp32s3 \
     --port /dev/ttyUSB0 \
     --baud 1500000 \
     write_flash -z \
       0x0      bootloader.bin \
       0x8000   partitions.bin \
       0x10000  firmware.bin
   ```

   - Adjust `--port` to match your system.
   - If your partition image is called `partition-table.bin`, substitute that filename in place of `partitions.bin`.

5. After the command completes, the Paper S3 will reboot into the EPUB reader firmware.

### 3.3 End‑user install with the M5 burning tool

Alternatively, you can use M5Stack’s **burning tool**:

1. Launch the M5 burning tool on your computer.
2. Select **ESP32‑S3** as the target chip (or the closest Paper S3 preset, if available).
3. Add the three images from your `releases/paper_s3_…` folder, using the same offsets as above:

   - `bootloader.bin` at **0x0000**
   - `partitions.bin` (or `partition-table.bin`) at **0x8000**
   - `firmware.bin` at **0x10000**

4. Select the correct serial port for the Paper S3 and start the burn process.
5. When flashing is complete, press reset / power‑cycle the device. It should boot straight into the Paper S3 EPUB reader.

---

## 4. Library View

The library view lists all EPUBs found under `/fs/Books`.

- **Grid vs List**
  - The library can render books as a **grid of covers** or a **text list**.
  - You can switch between these layouts via the **reader menu** (see below).

- **Navigation**
  - Tap a **cover** (grid) or **row** (list) to open that book.
  - Use the **bottom bar** to move between pages of results:
    - `<<` – jump to first page.
    - `<`  – previous page.
    - `>`  – next page.
    - `>>` – jump to last page.
  - The center of the bar shows the current page as `X / Y`.

- **Paper S3‑specific behaviour**
  - On first boot after flashing, the Paper S3 shows a **"Book library is loading"** splash while it scans `/fs/Books` and builds the index.
  - This splash is specific to the Paper S3 port; the desktop/Linux build typically just opens directly into the library.
  - When returning from a book via **Back to library** in the reader menu, the Paper S3 also shows the same loading splash and forces a full refresh while the library is redrawn.

---

## 5. Reading a Book

When you open a book, the main reading view appears.

- **Page turns**
  - Tap **left** or **right** areas of the screen to go to the previous/next page.
  - The exact mapping (left = previous or left = next) depends on your **Tap zones** setting (see reader menu).

- **Status bar (Paper S3)**
  - When enabled, the top status bar on Paper S3 shows **only the battery indicator**.
  - Title and reading progress text, which are present in the regular desktop/M5 builds, are **not shown** on Paper S3 to maximise reading area and clarity.
  - You can toggle the status bar permanently from the **reader menu** or via a **long‑press gesture** (see below).

- **Long‑press & swipe gestures (defaults)**
  - Long‑press **top‑center** while reading: toggle status bar ON/OFF.
  - Long‑press bottom‑left / bottom‑right: previous / next **section** (chapter), KOReader‑style.
  - Horizontal swipe (left/right): chapter‑level navigation may also be used, depending on gesture settings.
  - Vertical swipe up: mapped to **back** in some contexts (e.g. back to library).
  - **Two‑finger swipe down anywhere (while reading)**: force a **full‑screen refresh** of the current page (equivalent to `[R] Refresh screen` in the reader menu) to clear ghosting.
  - **Two‑finger swipe up anywhere (while in the library view)**: open the **reader menu** directly (advanced settings) without first opening a book.

---

## 6. Reader Menu (In‑Book Settings)

The reader menu is where you configure most behaviour. On Paper S3 there are two levels, with some differences from the desktop/M5 builds:

- A **compact main menu** with **6 items**.
- An **Advanced settings** menu with **10 options**, opened by tapping **More** and navigated with a bottom bar instead of a "Back" row.

### 6.1 Opening and closing the reader menu

While reading:

- **Open menu**
  - Tap near the **top edge** of the screen, or
  - Perform a **swipe up** gesture.

- **Close menu / return to book**
  - In the compact menu, tap **Return to book**.
  - In the advanced menu on Paper S3, tap the bottom‑bar `<<` (left double arrow) to go **back to the compact menu**, then choose **Return to book**.

### 6.2 Compact reader menu (Paper S3)

On Paper S3, the first‑level menu shows **6 items**:

1. **Return to book**  
   Close the menu and go back to the current page.

2. **Table of contents**  
   Open the TOC (if available) for the current book.

3. **Back to library**  
   Exit the book and return to the library view. On Paper S3 this shows the **"Book library is loading"** splash and forces a full refresh before the library appears.

4. **More**  
   Open the **Advanced settings** menu (see below).

5. **[R] Refresh screen**  
   Perform a full‑screen refresh of the current page to clear ghosting artefacts.

6. **[Zz] Sleep**  
   Immediately request sleep using the current **Sleep image** setting.

Tap any row to activate that option.

### 6.3 Advanced reader menu

From the compact menu, tap **More** to open **Advanced settings**. The labels show the current value inline. On Paper S3 the list of options is the same as the regular app, but you **navigate pages and go back using the bottom bar** instead of a dedicated "Back" row:

1. **Status bar: ON / OFF**  
   Show or hide the top status bar. On Paper S3 this controls the **battery‑only** bar at the top of the screen. This setting is also toggled by the long‑press top‑center gesture while reading.

2. **Library view: Grid / List**  
   Controls how the library home screen displays books.

3. **Startup: Last book / Library**  
   Decide what happens on **cold boot** (from power‑off or after flashing):
   - **Last book** – jump directly into the last opened book.
   - **Library** – open the library view first.

   > **Paper S3 difference:** Deep‑sleep **wake** always resumes directly into the last‑opened book and page, regardless of this setting. The **Startup** option only affects cold boots; the desktop/M5 builds may behave slightly differently.

4. **Sleep image: Cover / Random / Off**  
   What to show when the device goes to deep sleep:
   - **Cover** – reuse the current book cover as the sleep image.
   - **Random** – pick a random image from the SD card:
     - Prefer `/fs/Images`, then `/fs/images`, then `/fs/Pics`.
     - If no suitable image can be loaded, the reader falls back to drawing the current/last book **cover** instead of leaving the last page.
   - **Off** – blank screen (no custom image).

5. **Font size: Small / Medium / Large**  
   Adjusts the reading font size when a TTF font (`/fonts/reader.ttf`) is available. If no TTF is present, the label is still shown but may have limited effect.

6. **Alignment: Left / Justified**  
   Toggle paragraph alignment between left‑aligned and fully‑justified text. This is implemented via the EPUB layout engine and is specific to the reader view.

7. **Tap zones: Normal / Inverted**  
   Defines how left/right taps are interpreted while reading:
   - **Normal** – left = previous page, right = next page.
   - **Inverted** – left = next page, right = previous page.

8. **Idle: Short / Normal / Long**  
   Controls how quickly the device enters deep sleep when there is no user activity (touches or button presses):
   - **Short** – sleeps after about **2 minutes** in the library and **10 minutes** while reading.
   - **Normal** – sleeps after about **5 minutes** in the library and **20 minutes** while reading.
   - **Long** – sleeps after about **10 minutes** in the library and **40 minutes** while reading.

9. **Margins: Narrow / Normal / Wide**  
   Sets left/right margins in the reading view. Narrow gives more text per page; wide is more comfortable for some fonts.

10. **Gestures: Low / Medium / High**  
    Adjusts sensitivity for swipe and long‑press gestures:
    - **Low** – requires more movement / longer press, reduces accidental gestures.
    - **Medium** – default.
    - **High** – reacts to lighter touches and shorter swipes.

All these settings are persisted to:

```text
/fs/settings.bin
```

so they survive power‑off and deep sleep.

---

## 7. Sleep Behaviour (Paper S3)

When the device is idle for the configured **Idle** timeout:

- The current page is left on the e‑ink screen.
- Depending on **Sleep image**:
  - The current cover, a random image, or nothing is drawn before deep sleep.
  - In **Random** mode, images are chosen from `/fs/Images`, then `/fs/images`, then `/fs/Pics`. Any error or missing directory causes a **fallback to the book cover**.
- Just before deep sleep, the current reading position and library state are saved.

On **wake from deep sleep**, the Paper S3 firmware always resumes directly into the **last‑opened book and page**, regardless of the **Startup** setting.

- **Cold boot vs wake**
  - **Cold boot** (after power‑off or flashing) respects **Startup: Last book / Library**.
  - **Wake from deep sleep** always goes straight back into the last open book/page.

To wake the device, press the Paper S3 power button as usual.

---

## 8. Troubleshooting

- **No books shown**
  - Ensure the SD card is formatted as **FAT32**.
  - Check that EPUBs are in `/Books` (not nested in deeper subdirectories).

- **Sleep images not appearing**
  - Verify you have files under `/Images` or `/Pics`.
  - Confirm **Sleep image** is not set to **Off**.

- **Font size option seems to do nothing**
  - Make sure `/fonts/reader.ttf` exists on the SD card.
  - Reboot the device so the font can be initialized at startup.

- **Gestures feel too sensitive or unresponsive**
  - Open the reader menu → **More** → adjust **Gestures** to **Low** or **High** as needed.

---

## 9. Quick Start Checklist

1. Flash firmware using PlatformIO (`paper_s3_idf` or `paper_s3_release`).
2. Format SD as FAT32 and create `/Books`, `/Images`, `/Pics`, `/fonts`.
3. Copy EPUBs to `/Books` and an optional `reader.ttf` to `/fonts`.
4. Insert SD, boot device.
5. From the library, open a book.
6. While reading: tap top edge → **More** → tune **Font size**, **Margins**, **Tap zones**, **Gestures** to taste.

You’re now ready to use the M5Stack Paper S3 as a fast, tuned EPUB reader.

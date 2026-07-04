# Holy Grail Aggregator Board - Troubleshooting & Repair History

This document logs the hardware diagnostics, repairs, and software fixes performed to establish communication between the ESP32, the SC16IS752 UART bridge array, the W5500 Ethernet controller, and the HLK-LD2410C radar sensors.

---

## 1. SPI Bus & MISO Isolation Analysis

### Symptom
* Initial firmware returned `0xFF` (all `1`s) for all registers across all six SC16IS752 chips on the SPI bus.

### Diagnostic Actions
* **Bit-Bang Diagnostics**: Wrote a manual GPIO bit-bang SPI routine in `main.c` to control Chip Select (`~CS`), `SCLK`, and `MOSI` independently of the hardware SPI peripheral.
* **Address Line Loopback**: Configured decoder lines (`A0`, `A1`, `A2`, `E3`) as `INPUT_OUTPUT` during testing and successfully read back the driven voltages to verify there were no physical shorts or loading on the decoder inputs.
* **MISO Floating Net Isolation**: 
  * Discovered that when all SC16IS752 chips are deselected or held in reset, the shared `/SO` (MISO) line floats. 
  * The CMOS input stage of the MISO buffer `IC12` Pin 2 drifts high when floating, driving the ESP32 MISO line (`GPIO19`) to `1` (`0xFF`).
  * **Fix**: Enabled the ESP32's internal pull-down resistor on `GPIO19` so that a floating `/SO` net reads back as `0x00` instead of `0xFF`.

---

## 2. SPI Clock Short-to-GND Resolution (`IC15`)

### Symptom
* Loopback checks on the SPI control pins revealed that the SPI Clock Pin (**`SCLK` / `GPIO18`**) was stuck `LOW` (reading `0` even when driven `HIGH`).
* `MOSI` (`GPIO23`) was confirmed to be healthy and toggling.
* The lack of a clock signal prevented all SPI chips and the W5500 from clocking out register handshakes, resulting in total silence.

### Diagnostic Actions
* Traced the SCLK line `/ESP_SCLK` to the input of buffer **`IC15`** (Pin 2) and ground (Pin 3), which are adjacent pins on a SOT-23-5 package.
* Measured resistance to ground on the board and identified a direct solder bridge.

### Repair
* **Physical Fix**: Solder bridge cleared on **`IC15`** pins 2 and 3. SCLK signal was restored, passing the drive-and-readback verification test.

---

## 3. Sensor Wiring & Baud Rate Correction

### Symptom
* After repairing SCLK, SPI communications with the SC16IS752 chips succeeded (returning the expected test pattern `0x55` on the scratchpad registers), but the HLK-LD2410C sensors returned `0` bytes and began getting warm to the touch.

### Diagnostic Actions
* Added rate-limited hex dump logging of raw incoming bytes from the sensors.
* Verified that `rx_len` remained `0` and analyzed the heating behavior.
* **TX/RX Swap Identified**: Determined that the board's TX (outputting idle HIGH) was shorted to the sensor's TX (driving high/low during transmission), causing output driver contention (heating) and preventing the RX line from receiving data.

### Repair & Configuration
* **Physical Cable Fix**: Swapped the TX and RX connections on the sensor cables to match the board's DB25 breakout mapping:
  * **Board RX** (Pin 7 on `J6`, Pin 1 on `J3` for Sensor 1) connects to **Sensor TX**.
  * **Board TX** (Pin 8 on `J6`, Pin 2 on `J3` for Sensor 1) connects to **Sensor RX**.
* **Baud Rate Mismatch Fixed**: 
  * Once the wiring was corrected, raw bytes started arriving, but they appeared as scrambled garbage (`FC 9E 5F AF...`).
  * The sensor defaults to **256,000 baud**, but a 14.7456 MHz crystal on the SC16IS752 can only generate **230,400 baud** (divisor = 4, 0% error) or **307,200 baud** (divisor = 3, 20% error) with integer division.
  * **Fix**: Wireless connection was established with the sensor via the **HLK-Radar** smartphone app over Bluetooth (password: `HiLink`), and the sensor's internal serial port baud rate was changed to **`230400`** to match the board.
  * Optionally, modified the Bluetooth settings on the sensor (changed default password / disabled BLE) to prevent unauthorized remote access.

---

## 4. Software Optimizations & Boot Sequence

* **IOMUX Reclamation**: Added `gpio_reset_pin()` calls at the start of the diagnostic to decouple SCLK, MOSI, and MISO from the hardware SPI peripheral block, allowing standard bit-bang operations.
* **Boot self-test**: Modified `main.c` so the diagnostic prints bus health, address validation, and a W5500 read test on boot, then automatically continues normal startup tasks without halting.
* **W5500 Reset pin mapping**: Updated the hardware mapping (`#define PIN_W5500_RST 17`) to match the user's re-routing of the W5500 hardware reset pin to a dedicated GPIO (GPIO17). This provides independent reset control of the Ethernet chip, resolves conflict with the SC16IS752 reset net (GPIO2), and enables full hardware Ethernet networking.

---

## 5. W5500 Ethernet Controller SPI Diagnosis

### Symptom
* The W5500 Ethernet controller returned `0x00` on the `VERSIONR` (address `0x0039`) SPI read diagnostic at boot, causing the ESP-IDF Ethernet driver to fail initialization with `verify chip ID failed` / `ESP_ERR_INVALID_VERSION`.
* The W5500's physical Link/Activity LEDs on the RJ45 jack were fully functional and blinking, proving the analog PHY was active and negotiating network links.

### Diagnostic Actions
* **Reset Voltage Droop**: 
  * Initially measured **2.6V** on W5500 Pin 37 (`RSTn`). 
  * Floated `GPIO13` in software to prevent any default pull-down conflict on the board's bodge wiring. 
  * Identified that the W5500 reset pin has a Schmitt-trigger input with a logic threshold of $\approx 0.8 \times \text{VDD} = 2.61\text{V}$, causing the 2.6V level to hold the digital core in a state of continuous partial reset while leaving the analog PHY running.
* **Bypass Jumper (GPIO16)**: 
  * Configured `GPIO16` (`J9` Pin 2) to output a permanent logic `HIGH` (3.3V) and jumped it to `J9` Pin 3 (W5500 Reset), raising the W5500's reset pin voltage to a solid **3.26V**.
* **Slow-Motion SPI Logic Analysis**: 
  * Configured a super slow-motion SPI routine (2 seconds per bit) and verified with a multimeter that **SCLK (Pin 33)**, **MOSI (Pin 35)**, and **CS (Pin 32)** were all cleanly transitioning between 0V and 3.2V on the W5500 pins.
  * Measured the W5500's **MISO output (Pin 34)** during the slow-motion register read and observed that it remained stuck at **0.1V** (pulled down by the ESP32 internal pull-down) during all clock cycles, including the 6th data bit where the version register `0x04` should have driven it to 3.3V.

### Final Resolution & Success
* **Root Cause Identified**: 
  * Inspected the KiCad schematic and PCB layout for W5500 pins 37 through 42.
  * Discovered that **Pins 37 through 42** (which contain several reserved pins and strap inputs) were incorrectly tied to Ground or 3.3V on the PCB design, whereas the datasheet specified they should be left as No Connect (NC) or floated.
  * These illegal connections forced the W5500's internal digital core into a test/debug boot halt state, disabling its digital SPI interface block and returning `0x00`.
* **Physical Repair**: 
  * Physically **lifted pins 37 through 42** of the W5500 chip off their PCB pads to isolate them completely.
* **Result**: 
  * The W5500 immediately booted its digital core, successfully completed the SPI register handshake, and returned **`0x04`** (the expected version ID) on the SPI bus.
  * The netif stack successfully mounted, and the ESP32 established a solid TCP/IP connection to the MQTT broker to stream the 12-sensor telemetry.
  * The diagnostic code has been disabled and production Ethernet configuration has been re-enabled in [main.c](file:///c:/Users/remas/Desktop/PCBs/DarkMagic/Code/HolyGrail/main/main.c).

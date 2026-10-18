# Sushi Box

A small desk companion built on an **ESP32-C3 Super Mini** with a 0.96" OLED.
It shows a cute animated face that smiles when you come close, and one button
flips through a clock, the weather and a status screen.

WiFi is set up from your phone over Bluetooth, so **no WiFi password is stored in
the code**. Everything else (city, clock format, units…) is changed from a
settings page in your phone's browser.

---

## Features

- **Animated face**: blinks by itself and smiles when someone is closer than 10 cm (ultrasonic sensor)
- **Clock**: time from the internet, time zone follows your city automatically, 12 or 24 hour
- **Weather**: temperature, icon, today's high/low and humidity from [Open-Meteo](https://open-meteo.com) (free, no API key), updated every 15 minutes
- **Info screen**: WiFi status, signal strength, settings page address, uptime
- **One button** to control everything
- **WiFi setup over Bluetooth** with Espressif's free *ESP BLE Provisioning* app
- **Settings page in the browser**: city search, clock format, °C/°F, smile distance, back-to-face time
- **Works without internet**: the face keeps working. The clock keeps time after its first sync, as long as the box stays powered.

---

## Hardware

| Part | Notes |
|---|---|
| ESP32-C3 Super Mini | 4 MB flash |
| 0.96" OLED 128×64, SSD1306, I2C | Address `0x3C` |
| HC-SR04 ultrasonic sensor | Or HC-SR04P (3.3 V version) |
| Push button | Any momentary button |
| 1 kΩ + 2 kΩ resistors | Voltage divider for the sensor's ECHO pin (5 V sensor only) |

### Wiring

| Part | Part pin | ESP32-C3 Super Mini |
|---|---|---|
| OLED | VCC | 3V3 |
| | GND | GND |
| | SDA | GPIO 8 |
| | SCL | GPIO 9 |
| Ultrasonic sensor | VCC | 5V |
| | GND | GND |
| | TRIG | GPIO 3 |
| | ECHO | GPIO 4 (through the divider below) |
| Button | one leg | GPIO 10 |
| | other leg | GND |

**ECHO voltage divider** (needed for a 5 V HC-SR04, because the ESP32-C3 only takes 3.3 V):

```text
Sensor ECHO ──[ 1 kΩ ]──┬── GPIO 4
                        │
                     [ 2 kΩ ]
                        │
                       GND
```

**Notes**
- For a 4-leg button, use two legs that are diagonally opposite.
- The blue LED on the board is on GPIO 8 (SDA), so it may glow faintly while the OLED is updating. That's normal.
- Don't hold the board's **BOOT** button while powering on (it shares GPIO 9).

---

## Software setup

### 1. Arduino IDE

1. Install **Arduino IDE 2**.
2. **Board package:** *Tools → Board → Boards Manager*, search `esp32`, and install **esp32 by Espressif Systems** (version 3.x recommended).
3. **Libraries:** *Sketch → Include Library → Manage Libraries* and install:
   - **Adafruit SSD1306** (click *Install all* so **Adafruit GFX** comes with it)
   - **ArduinoJson** by Benoit Blanchon (version 7)

   Everything else (WiFi, WiFiProv, Preferences, WebServer, ESPmDNS, HTTPClient) comes with the ESP32 board package.

### 2. Board settings

| Setting | Value |
|---|---|
| Tools → Board | **ESP32C3 Dev Module** (or **Nologo ESP32C3 Super Mini** if listed) |
| Tools → Partition Scheme | **Huge APP (3MB No OTA/1MB SPIFFS)** (Bluetooth makes the program too big for the default) |
| Tools → Flash Mode | QIO (change to **DIO** if the board keeps restarting after upload) |
| Tools → Port | The COM port of your board |

### 3. Upload

Open `SushiBox/SushiBox.ino` and click **Upload**.
If the upload won't start, hold **BOOT**, plug in the USB cable, release **BOOT**, and upload again.

---

## Configuration

Settings at the top of `SushiBox.ino`:

| Setting | Default | What it does |
|---|---|---|
| `OLED_SDA`, `OLED_SCL` | `8`, `9` | OLED I2C pins |
| `TRIG_PIN`, `ECHO_PIN` | `3`, `4` | Ultrasonic sensor pins |
| `BUTTON_PIN` | `10` | Button pin (button to GND) |
| `WIFI_DAYA` | `WIFI_POWER_8_5dBm` | WiFi transmit power. Kept low because the C3 Super Mini's small antenna often fails at full power. Try `WIFI_POWER_11dBm` if the signal is weak. |
| `BLE_NAMA` | `"PROV_SushiBox"` | Bluetooth name shown in the app. Must start with `PROV_`. |
| `BLE_PIN` | `"sushi123"` | PIN you type in the app. **Change this to your own.** |
| `AWAL_KOTA`, `AWAL_LAT`, `AWAL_LON` | Jakarta | Starting city, before you change it in the settings page |
| `AWAL_ZONA` | `25200` | Starting time zone in seconds from UTC (25200 = UTC+7). Updates automatically from the city. |
| `BATAS_JARAK_CM` | `10` | Starting smile distance in cm |
| `AUTO_BALIK_MS` | `30000` | Starting back-to-face time (ms) |

City, clock format, units, smile distance and back-to-face time can all be changed later from the settings page. You don't need to re-upload.

---

## First-time WiFi setup

1. Install **ESP BLE Provisioning** by Espressif on your phone
   ([Android](https://play.google.com/store/apps/details?id=com.espressif.provble) /
   [iPhone](https://apps.apple.com/app/esp-ble-provisioning/id1473590141)).
2. Power on the Sushi Box. With no saved WiFi, the Info screen shows:
   ```text
   Setup WiFi with app:
   ESP BLE Provisioning
   Choose: no QR code
   Name: PROV_SushiBox
   PIN:  sushi123
   ```
3. On your phone, turn on **Bluetooth** (and **Location** on Android).
4. In the app, tap **Provision Device**, then **I don't have a QR code**.
5. Choose **PROV_SushiBox** and type the PIN.
6. Choose your WiFi and type its password.

The box saves the WiFi and connects by itself every time it starts. Bluetooth turns
off after setup.

> The ESP32-C3 only works with **2.4 GHz** WiFi. If your router has a separate
> "-5G" network, pick the other one.

### Changing WiFi

Go to the **Info** screen and **long-press the button twice within 5 seconds**.
The box forgets the saved WiFi, restarts and waits for the app again.

---

## Settings page

Once the box is on WiFi:

1. Connect your phone to the **same WiFi**.
2. Open the **Info** screen. It shows the address, for example:
   ```text
   Settings in browser:
   http://192.168.1.23
   ```
3. Type that address into Chrome or Safari.
   On iPhone, Mac and Windows, **http://sushibox.local** also works.

| Setting | Description |
|---|---|
| City | Type a name, tap **Search**, then tap the right result. The weather location and the clock time zone change to match. |
| Clock | 24 hour or 12 hour (AM/PM) |
| Temperature | °C or °F |
| Smile when closer than | Distance in cm that makes the face smile |
| Back to face after | Seconds without a button press before returning to the face (0 = never) |

Tap **Save**. Settings are stored on the ESP32 and kept after power-off.

---

## Using the button

| | Face | Clock | Weather | Info |
|---|---|---|---|---|
| **Short press** | Next screen | Next screen | Next screen | Back to face |
| **Long press** (~1 s) | Smile | Switch 12/24 h | Update weather now | Press twice within 5 s to forget WiFi and set it up again |

When you leave a screen, it goes back to the face after 30 seconds by default.
You can change this on the settings page.

### Screens

1. **Face**: blinks every few seconds and smiles when someone comes close.
2. **Clock**: date, big time with blinking colon, seconds bar.
3. **Weather**: city, weather icon, temperature, condition, today's high/low and humidity, and how long ago it was updated.
4. **Info**: WiFi status, network name, signal, settings page address, uptime.

---

## Troubleshooting

| Problem | What to do |
|---|---|
| **Sketch too big** when compiling | Set *Partition Scheme* to **Huge APP**. |
| Board keeps restarting after upload | Set *Flash Mode* to **DIO** and upload again. |
| Screen stays black | Check OLED wiring (SDA 8, SCL 9, 3V3, GND). Most modules use address `0x3C`. |
| Button does nothing | Upload `ButtonTest/ButtonTest.ino`. It shows *PRESSED* / *released* on the OLED. If it always says *PRESSED*, rotate a 4-leg button 90°. |
| Face never smiles | Check sensor wiring and the ECHO voltage divider. Try a larger smile distance on the settings page. |
| WiFi setup says **"Can't connect… Wrong password?"** | The box keeps the WiFi and restarts to retry; on the C3 Super Mini this often fixes itself. If the Info screen still says *connecting...*, check the password (capital letters!) and set up again (Info → long-press twice). |
| WiFi setup says **"WiFi not found"** | Use a **2.4 GHz** network and move closer to the router. |
| Still can't connect | Set the router to **WPA2/WPA3** (not WPA3-only). Test with a phone hotspot. On iPhone, turn on *Maximize Compatibility*. |
| City search on the settings page fails | The phone needs internet access on the same WiFi. |
| `sushibox.local` doesn't open | Many Android phones don't support `.local` addresses. Use the numbers from the Info screen. |

---

## Credits

- Weather data by [Open-Meteo.com](https://open-meteo.com) (CC BY 4.0)
- Time from `pool.ntp.org`
- Display libraries by Adafruit, JSON parsing by ArduinoJson

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiProv.h>      // Setup WiFi lewat Bluetooth (sudah termasuk di board ESP32)
#include <Preferences.h>
#include <WebServer.h>     // Halaman pengaturan di browser (sudah termasuk di board ESP32)
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // Library Manager: "ArduinoJson" by Benoit Blanchon (v7)
#include <time.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// ===== Pin ESP32-C3 Super Mini =====
// Pin I2C bawaan C3 Super Mini (SDA = GPIO8, SCL = GPIO9)
#define OLED_SDA 8
#define OLED_SCL 9

// Pin Sensor Ultrasonik (ECHO pakai pembagi tegangan kalau sensor 5V)
#define TRIG_PIN 3 // GPIO3
#define ECHO_PIN 4 // GPIO4

// Pin Tombol (GPIO10 -> tombol -> GND)
#define BUTTON_PIN 10

// C3 Super Mini: antena kecil, WiFi sering gagal connect di daya penuh.
// Daya pancar diturunkan supaya stabil (kalau sinyal lemah, coba WIFI_POWER_11dBm)
#define WIFI_DAYA WIFI_POWER_8_5dBm

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ===== Setup WiFi lewat HP (Bluetooth) =====
// Tidak ada password WiFi di kode. Atur dari HP pakai app "ESP BLE Provisioning"
// (Partition Scheme: Huge APP): pilih "I don't have a QR code", lalu ketik PIN
#define BLE_NAMA "PROV_SushiBox" // Nama Bluetooth, harus diawali PROV_ supaya muncul di app
#define BLE_PIN  "sushi123"      // PIN yang diketik di app

// ===== Pengaturan awal =====
// Bisa diubah dari HP: buka alamat di layar Info (atau http://sushibox.local) di browser
#define AWAL_KOTA "Jakarta"
#define AWAL_LAT  -6.2088
#define AWAL_LON  106.8456
#define AWAL_ZONA 25200 // Selisih dari UTC dalam detik (25200 = UTC+7, WIB). Otomatis ikut kota

const int BATAS_JARAK_CM = 10;

// Variabel Waktu Animasi
unsigned long lastBlinkTime = 0;
unsigned long blinkInterval = 3000;
unsigned long lastSensorCheck = 0;

// Ukuran Mata Lonjong Vertikal Standby
int eyeWidth = 22;
int eyeHeight = 38;
int eyeRadius = 10;

// Lokasi Pusat Mata & Mulut
const int leftEyeX = 36;
const int rightEyeX = 92;
const int eyeY = 30;
const int mouthX = 64;
const int mouthY = 54;

bool isDetecting = false;
bool wasDetecting = false; // Untuk mendeteksi perubahan status

// ===== Layar =====
enum { LAYAR_WAJAH, LAYAR_JAM, LAYAR_CUACA, LAYAR_INFO, JUMLAH_LAYAR };
int layar = LAYAR_WAJAH;
bool perluGambar = true;
unsigned long lastGambar = 0;
const unsigned long AUTO_BALIK_MS = 30000; // Balik ke wajah setelah 30 detik tanpa tombol (0 = mati)

// ===== Tombol =====
const unsigned long LONG_PRESS_MS = 800;
bool tombolDitekan = false;
bool longPressTerpicu = false;
unsigned long waktuTekan = 0;
unsigned long waktuGantiStatus = 0;
unsigned long lastTombol = 0;
bool eventShortPress = false;
bool eventLongPress = false;

// ===== WiFi, Jam & Cuaca =====
volatile bool modeSetupWiFi = false; // true = sedang menunggu setup WiFi dari HP
bool tadiSetup = false;
unsigned long waktuKonfirmasiReset = 0;
bool ntpDimulai = false;
unsigned long lastCobaWiFi = 0;
volatile bool setupGagal = false;     // Koneksi pertama setelah setup dari HP gagal
volatile bool gagalPassword = false;  // true = password salah, false = WiFi tidak ketemu
char ssidDicoba[33] = "";             // Nama WiFi yang dikirim dari HP
WebServer server(80);
bool webDimulai = false;

// ===== Pengaturan (disimpan di memori ESP32, diubah lewat browser) =====
String namaKota = AWAL_KOTA;
float latitude = AWAL_LAT;
float longitude = AWAL_LON;
long zonaDetik = AWAL_ZONA;
bool format24Jam = true;
bool satuanF = false; // false = Celsius, true = Fahrenheit
int batasJarakCm = BATAS_JARAK_CM;
unsigned long autoBalikMs = AUTO_BALIK_MS;

// Nama konstanta setup Bluetooth berubah sejak ESP32 core 3.1
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 1, 0)
  #define PROV_SKEMA_BLE   NETWORK_PROV_SCHEME_BLE
  #define PROV_BEBASKAN_BT NETWORK_PROV_SCHEME_HANDLER_FREE_BLE
  #define PROV_KEAMANAN    NETWORK_PROV_SECURITY_1
  #define PROV_GAGAL_PASS  NETWORK_PROV_WIFI_STA_AUTH_ERROR
#else
  #define PROV_SKEMA_BLE   WIFI_PROV_SCHEME_BLE
  #define PROV_BEBASKAN_BT WIFI_PROV_SCHEME_HANDLER_FREE_BLE
  #define PROV_KEAMANAN    WIFI_PROV_SECURITY_1
  #define PROV_GAGAL_PASS  WIFI_PROV_STA_AUTH_ERROR
#endif

uint8_t uuidBLE[16] = {0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
                       0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02};

struct DataCuaca {
  float suhu;
  float suhuMax;
  float suhuMin;
  int kelembapan;
  int kode;
};
DataCuaca cuaca;
bool adaDataCuaca = false;
bool pernahCobaCuaca = false;
unsigned long waktuCobaCuaca = 0;
unsigned long waktuCuacaDiambil = 0;

enum { IKON_CERAH, IKON_BERAWAN, IKON_MENDUNG, IKON_KABUT, IKON_HUJAN, IKON_SALJU, IKON_BADAI };

const char* NAMA_HARI[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* NAMA_BULAN[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void setup() {
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for(;;);
  }
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  muatPengaturan();

  // Tidak menunggu WiFi, wajah langsung jalan
  mulaiWiFi();

  display.clearDisplay();
  drawFaceNormal(eyeHeight);
}

void loop() {
  bacaTombol();
  cekSensor(millis());
  urusWiFi(millis());

  // Mode setup WiFi mulai/selesai: tampilkan petunjuk di layar Info
  if (modeSetupWiFi != tadiSetup) {
    tadiSetup = modeSetupWiFi;
    lastTombol = millis();
    if (modeSetupWiFi) gantiLayar(LAYAR_INFO);
  }

  // Koneksi pertama setelah setup gagal. WiFi sudah tersimpan, jadi JANGAN dihapus:
  // di C3 Super Mini sering cuma gagal sesaat (sinyal), setelah restart biasanya tersambung.
  // Kalau password memang salah: layar Info -> tekan lama 2x -> setup ulang.
  if (setupGagal) {
    setupGagal = false;
    display.clearDisplay();
    tulisTengah("Can't connect to", 0, 1);
    tulisTengah(ssidDicoba, 12, 1);
    if (gagalPassword) {
      tulisTengah("Wrong password?", 28, 1);
    } else {
      tulisTengah("WiFi not found", 28, 1);
      tulisTengah("(use 2.4GHz WiFi)", 38, 1);
    }
    tulisTengah("Retrying...", 54, 1);
    display.display();
    jeda(4000);
    ESP.restart();
  }

  unsigned long currentMillis = millis();

  // Halaman pengaturan di browser
  if (webDimulai) server.handleClient();

  // Tekan singkat = layar berikutnya
  if (eventShortPress) {
    eventShortPress = false;
    lastTombol = currentMillis;
    gantiLayar((layar + 1) % JUMLAH_LAYAR);
  }

  // Tekan lama = aksi khusus tiap layar
  if (eventLongPress) {
    eventLongPress = false;
    lastTombol = currentMillis;
    aksiLongPress();
  }

  // Kembali ke wajah kalau tombol lama tidak disentuh (petunjuk setup WiFi tetap tampil)
  bool tahanSetup = modeSetupWiFi && layar == LAYAR_INFO;
  if (autoBalikMs > 0 && layar != LAYAR_WAJAH && !tahanSetup && millis() - lastTombol >= autoBalikMs) {
    gantiLayar(LAYAR_WAJAH);
  }

  if (layar == LAYAR_WAJAH) {
    tampilWajah(millis());
  } else if (perluGambar || millis() - lastGambar >= 100) {
    perluGambar = false;
    lastGambar = millis();
    gambarLayar();
  }
}

// ================= TOMBOL =================

// Baca tombol + debounce, hasilnya event short/long press
void bacaTombol() {
  bool ditekan = (digitalRead(BUTTON_PIN) == LOW);
  unsigned long now = millis();

  if (ditekan != tombolDitekan && now - waktuGantiStatus > 30) {
    waktuGantiStatus = now;
    tombolDitekan = ditekan;
    if (ditekan) {
      waktuTekan = now;
      longPressTerpicu = false;
    } else if (!longPressTerpicu) {
      eventShortPress = true;
    }
  }

  if (tombolDitekan && !longPressTerpicu && now - waktuTekan >= LONG_PRESS_MS) {
    longPressTerpicu = true;
    eventLongPress = true;
  }
}

// Pengganti delay() yang tetap membaca tombol
void jeda(unsigned long ms) {
  unsigned long mulai = millis();
  while (millis() - mulai < ms) {
    bacaTombol();
    delay(1);
  }
}

void gantiLayar(int baru) {
  layar = baru;
  perluGambar = true;
  if (layar == LAYAR_WAJAH) {
    wasDetecting = false; // Biar transisi happy jalan lagi kalau ada orang
    lastBlinkTime = millis();
  }
}

void aksiLongPress() {
  if (layar != LAYAR_WAJAH) {
    // Kedip layar sebagai tanda long press diterima
    display.invertDisplay(true);
    jeda(100);
    display.invertDisplay(false);
  }

  switch (layar) {
    case LAYAR_WAJAH:
      // "Dielus": senyum sebentar
      if (!wasDetecting) {
        transisiKeHappy();
        jeda(1000);
        transisiKeStandby();
      }
      break;

    case LAYAR_JAM:
      format24Jam = !format24Jam;
      simpanPengaturan();
      break;

    case LAYAR_CUACA:
      if (!modeSetupWiFi && WiFi.status() == WL_CONNECTED) {
        display.clearDisplay();
        tulisTengah("Updating...", 28, 1);
        display.display();
        pernahCobaCuaca = true;
        waktuCobaCuaca = millis();
        ambilCuaca();
      }
      break;

    case LAYAR_INFO:
      // Long press 2x (dalam 5 detik) = hapus WiFi & setup ulang dari HP
      if (konfirmasiResetAktif()) {
        resetWiFi();
      } else {
        waktuKonfirmasiReset = millis();
      }
      break;
  }
  perluGambar = true;
}

// ================= SENSOR =================

void cekSensor(unsigned long now) {
  // Cek Sensor Setiap 80ms
  if (now - lastSensorCheck < 80) return;
  lastSensorCheck = now;

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long durasi = pulseIn(ECHO_PIN, HIGH, 20000);
  int jarak = (durasi > 0) ? (durasi * 0.034 / 2) : 999;

  isDetecting = (jarak > 0 && jarak <= batasJarakCm);
}

// ================= WIFI, JAM, CUACA =================

void mulaiWiFi() {
  // Cek apakah sebelum restart diminta setup ulang WiFi
  Preferences pref;
  pref.begin("sushibox", false);
  bool setupUlang = pref.getBool("resetWiFi", false);
  if (setupUlang) pref.remove("resetWiFi");
  pref.end();

  WiFi.onEvent(eventWiFi);
  WiFi.persistent(true);
  if (setupUlang) {
    // Ganti WiFi: hapus WiFi lama dulu, jangan sambung ke WiFi lama
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, true);
  } else {
    WiFi.begin(); // Pakai WiFi yang tersimpan (kalau ada)
  }
  WiFi.setTxPower(WIFI_DAYA);

  // Kalau belum ada WiFi tersimpan, mulai setup lewat Bluetooth (tidak menunggu)
  WiFiProv.beginProvision(PROV_SKEMA_BLE, PROV_BEBASKAN_BT, PROV_KEAMANAN,
                          BLE_PIN, BLE_NAMA, NULL, uuidBLE, setupUlang);
}

// Dipanggil otomatis oleh ESP32 saat status setup WiFi berubah
void eventWiFi(arduino_event_t *e) {
  switch (e->event_id) {
    case ARDUINO_EVENT_WIFI_STA_START:
      WiFi.setTxPower(WIFI_DAYA); // Pasang lagi tiap WiFi menyala
      break;
    case ARDUINO_EVENT_PROV_START:
      modeSetupWiFi = true;
      break;
    case ARDUINO_EVENT_PROV_CRED_RECV:
      memcpy(ssidDicoba, e->event_info.prov_cred_recv.ssid, 32);
      ssidDicoba[32] = 0;
      WiFi.setTxPower(WIFI_DAYA);
      break;
    case ARDUINO_EVENT_PROV_CRED_FAIL:
      gagalPassword = (e->event_info.prov_fail_reason == PROV_GAGAL_PASS);
      setupGagal = true;
      break;
    case ARDUINO_EVENT_PROV_END:
      modeSetupWiFi = false;
      break;
    default:
      break;
  }
}

// Hapus WiFi tersimpan: restart lalu masuk mode setup Bluetooth
void resetWiFi() {
  Preferences pref;
  pref.begin("sushibox", false);
  pref.putBool("resetWiFi", true);
  pref.end();

  display.clearDisplay();
  tulisTengah("Restarting...", 28, 1);
  display.display();
  delay(1000);
  ESP.restart();
}

// Nama WiFi yang tersimpan (diisi dari HP)
String namaWiFi() {
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK || conf.sta.ssid[0] == 0) return "-";
  char ssid[33];
  memcpy(ssid, conf.sta.ssid, 32);
  ssid[32] = 0;
  return String(ssid);
}

bool konfirmasiResetAktif() {
  return waktuKonfirmasiReset != 0 && millis() - waktuKonfirmasiReset < 5000;
}

// ================= PENGATURAN =================

void muatPengaturan() {
  Preferences pref;
  pref.begin("sushibox", true);
  namaKota     = pref.getString("kota", AWAL_KOTA);
  latitude     = pref.getFloat("lat", AWAL_LAT);
  longitude    = pref.getFloat("lon", AWAL_LON);
  zonaDetik    = pref.getLong("zona", AWAL_ZONA);
  format24Jam  = pref.getBool("jam24", true);
  satuanF      = pref.getBool("suhuF", false);
  batasJarakCm = pref.getInt("jarak", BATAS_JARAK_CM);
  autoBalikMs  = pref.getULong("balik", AUTO_BALIK_MS);
  pref.end();
}

void simpanPengaturan() {
  Preferences pref;
  pref.begin("sushibox", false);
  pref.putString("kota", namaKota);
  pref.putFloat("lat", latitude);
  pref.putFloat("lon", longitude);
  pref.putLong("zona", zonaDetik);
  pref.putBool("jam24", format24Jam);
  pref.putBool("suhuF", satuanF);
  pref.putInt("jarak", batasJarakCm);
  pref.putULong("balik", autoBalikMs);
  pref.end();
}

// Zona waktu dari selisih UTC, mis. 25200 detik -> "UTC-7:00" (format POSIX: tanda dibalik)
String teksZonaWaktu(long detik) {
  long menit = -detik / 60;
  char tz[16];
  snprintf(tz, sizeof(tz), "UTC%+ld:%02ld", menit / 60, labs(menit % 60));
  return String(tz);
}

void aturZonaWaktu() {
  setenv("TZ", teksZonaWaktu(zonaDetik).c_str(), 1);
  tzset();
}

// ================= HALAMAN PENGATURAN (BROWSER) =================

const char HALAMAN_SETTINGS[] = R"rawliteral(<!DOCTYPE html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Sushi Box</title><style>
body{font-family:sans-serif;max-width:380px;margin:20px auto;padding:0 16px;color:#222}
label{display:block;margin-top:16px;font-weight:bold}
input,select,button{width:100%;padding:10px;margin-top:6px;font-size:16px;box-sizing:border-box}
#hasil button{text-align:left;background:#f2f2f2;border:1px solid #ccc;border-radius:6px}
.dua{display:flex;gap:8px}
small{color:#666}
</style></head><body>
<h2>Sushi Box settings</h2>
<form method=post action=/save id=f>
<label>City</label>
<input id=q placeholder="Search a city, e.g. Bandung">
<button type=button onclick=cari()>Search</button>
<div id=hasil></div>
<input name=kota value="%KOTA%">
<div class=dua><input name=lat value="%LAT%"><input name=lon value="%LON%"></div>
<small>Latitude, longitude. The clock time zone follows the city.</small>
<label>Clock</label>
<select name=jam><option value=24 %J24%>24 hour</option><option value=12 %J12%>12 hour (AM/PM)</option></select>
<label>Temperature</label>
<select name=suhu><option value=C %SC%>Celsius (&deg;C)</option><option value=F %SF%>Fahrenheit (&deg;F)</option></select>
<label>Smile when closer than (cm)</label>
<input name=jarak type=number min=2 max=200 value="%JARAK%">
<label>Back to face after (seconds, 0 = never)</label>
<input name=balik type=number min=0 max=3600 value="%BALIK%">
<button>Save</button>
</form>
<script>
async function cari(){
 const f=document.getElementById('f'),h=document.getElementById('hasil');
 const q=document.getElementById('q').value.trim();
 if(q.length<2){h.textContent='Type at least 2 letters';return}
 h.textContent='Searching...';
 try{
  const r=await fetch('https://geocoding-api.open-meteo.com/v1/search?count=5&language=en&name='+encodeURIComponent(q));
  const j=await r.json();
  if(!j.results){h.textContent='City not found';return}
  h.innerHTML='';
  j.results.forEach(c=>{
   const b=document.createElement('button');b.type='button';
   b.textContent=c.name+(c.admin1?', '+c.admin1:'')+(c.country?', '+c.country:'');
   b.onclick=()=>{f.kota.value=c.name;f.lat.value=c.latitude;f.lon.value=c.longitude;h.textContent='Selected: '+b.textContent};
   h.appendChild(b);
  });
 }catch(e){h.textContent='Search failed. Is the phone online?'}
}
</script></body></html>)rawliteral";

void mulaiHalamanSettings() {
  server.on("/", HTTP_GET, halamanSettings);
  server.on("/save", HTTP_POST, simpanSettings);
  server.begin();
  MDNS.begin("sushibox"); // Juga bisa dibuka di http://sushibox.local
  MDNS.addService("http", "tcp", 80);
}

void halamanSettings() {
  String html = HALAMAN_SETTINGS;
  html.replace("%KOTA%", escapeHtml(namaKota));
  html.replace("%LAT%", String(latitude, 4));
  html.replace("%LON%", String(longitude, 4));
  html.replace("%J24%", format24Jam ? "selected" : "");
  html.replace("%J12%", format24Jam ? "" : "selected");
  html.replace("%SC%", satuanF ? "" : "selected");
  html.replace("%SF%", satuanF ? "selected" : "");
  html.replace("%JARAK%", String(batasJarakCm));
  html.replace("%BALIK%", String(autoBalikMs / 1000));
  server.send(200, "text/html", html);
}

void simpanSettings() {
  String kota = server.arg("kota");
  kota.trim();
  String lat = server.arg("lat");
  String lon = server.arg("lon");
  if (kota.length() > 0 && lat.length() > 0 && lon.length() > 0) {
    float la = lat.toFloat();
    float lo = lon.toFloat();
    if (la >= -90 && la <= 90 && lo >= -180 && lo <= 180) {
      namaKota = kota.substring(0, 32);
      latitude = la;
      longitude = lo;
    }
  }
  format24Jam = server.arg("jam") != "12";
  satuanF = server.arg("suhu") == "F";
  batasJarakCm = constrain(server.arg("jarak").toInt(), 2, 200);
  autoBalikMs = constrain(server.arg("balik").toInt(), 0, 3600) * 1000UL;
  simpanPengaturan();

  // Ambil cuaca lagi dengan kota & satuan baru
  adaDataCuaca = false;
  pernahCobaCuaca = false;
  perluGambar = true;

  server.send(200, "text/html",
              "<meta name=viewport content='width=device-width,initial-scale=1'>"
              "<body style='font-family:sans-serif;margin:24px'><h2>Saved!</h2>"
              "<p>Sushi Box is updating the weather.</p><a href=/>Back to settings</a></body>");
}

String escapeHtml(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

void urusWiFi(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) {
    // Jaga daya WiFi tetap rendah (kadang kembali penuh setelah WiFi dinyalakan ulang)
    if (WiFi.getMode() != WIFI_MODE_NULL && WiFi.getTxPower() != WIFI_DAYA) WiFi.setTxPower(WIFI_DAYA);

    // Coba sambung ulang tiap 30 detik (kecuali sedang setup dari HP)
    if (!modeSetupWiFi && now - lastCobaWiFi >= 30000) {
      lastCobaWiFi = now;
      WiFi.reconnect();
    }
    return;
  }
  lastCobaWiFi = now;

  // Sinkron jam dari internet (sekali saja, ESP32 lanjut menghitung sendiri)
  if (!ntpDimulai) {
    configTzTime(teksZonaWaktu(zonaDetik).c_str(), "pool.ntp.org", "time.google.com");
    ntpDimulai = true;
  }

  // Selama setup tunggu dulu (Bluetooth masih memakai banyak RAM)
  if (modeSetupWiFi) return;

  // Halaman pengaturan di browser (sekali saja, setelah WiFi tersambung)
  if (!webDimulai) {
    mulaiHalamanSettings();
    webDimulai = true;
  }

  // Cuaca tiap 15 menit, atau tiap 1 menit kalau belum pernah dapat
  unsigned long intervalCuaca = adaDataCuaca ? 15UL * 60 * 1000 : 60000;
  if (!pernahCobaCuaca || now - waktuCobaCuaca >= intervalCuaca) {
    pernahCobaCuaca = true;
    waktuCobaCuaca = now;
    ambilCuaca();
  }
}

bool ambilWaktu(struct tm &t) {
  time_t sekarang = time(nullptr);
  localtime_r(&sekarang, &t);
  return t.tm_year > (2020 - 1900);
}

// Ambil cuaca dari Open-Meteo (gratis, tanpa API key)
void ambilCuaca() {
  WiFiClient client; // HTTP biasa: lebih kecil & cepat, data cuaca bukan rahasia
  HTTPClient http;
  http.setTimeout(5000);

  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + String(latitude, 4) +
               "&longitude=" + String(longitude, 4) +
               "&current=temperature_2m,relative_humidity_2m,weather_code"
               "&daily=temperature_2m_max,temperature_2m_min"
               "&forecast_days=1&timezone=auto";
  if (satuanF) url += "&temperature_unit=fahrenheit";

  if (!http.begin(client, url)) return;

  if (http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      cuaca.suhu       = doc["current"]["temperature_2m"].as<float>();
      cuaca.kelembapan = doc["current"]["relative_humidity_2m"].as<int>();
      cuaca.kode       = doc["current"]["weather_code"].as<int>();
      cuaca.suhuMax    = doc["daily"]["temperature_2m_max"][0].as<float>();
      cuaca.suhuMin    = doc["daily"]["temperature_2m_min"][0].as<float>();
      adaDataCuaca = true;
      waktuCuacaDiambil = millis();

      // Zona waktu jam ikut lokasi kota (termasuk musim panas di negara yang memakainya)
      long zona = doc["utc_offset_seconds"] | zonaDetik;
      if (zona != zonaDetik) {
        zonaDetik = zona;
        aturZonaWaktu();
        simpanPengaturan();
      }
    }
  }
  http.end();
}

// ================= WAJAH =================

void tampilWajah(unsigned long currentMillis) {
  // Logika Transisi Animasi
  if (isDetecting) {
    if (!wasDetecting) {
      // Transisi dari Standby ke Happy (Halus)
      transisiKeHappy();
      wasDetecting = true;
    } else {
      // Tampilkan ekspresi Happy
      drawFaceHappy(1.0);
    }
  } else {
    if (wasDetecting) {
      // Transisi kembali dari Happy ke Standby (Halus)
      transisiKeStandby();
      wasDetecting = false;
    }

    // Efek Kedip Otomatis saat Standby
    if (currentMillis - lastBlinkTime >= blinkInterval) {
      animasiKedip();
      lastBlinkTime = currentMillis;
      blinkInterval = random(2000, 5000);
    } else {
      drawFaceNormal(eyeHeight);
    }
  }
}

// Bulu Mata Lentik 1 Garis
void drawSingleEyelash(bool isBlinking, int h, int offsetX = 0, int offsetY = 0) {
  if (isBlinking && h < 10) return;

  int topY = eyeY - (h / 2) + offsetY;

  // Kiri
  display.drawLine(leftEyeX - 6 + offsetX, topY + 2, leftEyeX - 14 + offsetX, topY - 5, SSD1306_WHITE);
  display.drawLine(leftEyeX - 6 + offsetX, topY + 3, leftEyeX - 14 + offsetX, topY - 4, SSD1306_WHITE);

  // Kanan
  display.drawLine(rightEyeX + 6 - offsetX, topY + 2, rightEyeX + 14 - offsetX, topY - 5, SSD1306_WHITE);
  display.drawLine(rightEyeX + 6 - offsetX, topY + 3, rightEyeX + 14 - offsetX, topY - 4, SSD1306_WHITE);
}

// Wajah Standby Normal
void drawFaceNormal(int h) {
  display.clearDisplay();
  int topY = eyeY - (h / 2);

  // Mata Lonjong
  display.fillRoundRect(leftEyeX - (eyeWidth / 2), topY, eyeWidth, h, eyeRadius, SSD1306_WHITE);
  display.fillRoundRect(rightEyeX - (eyeWidth / 2), topY, eyeWidth, h, eyeRadius, SSD1306_WHITE);

  drawSingleEyelash(true, h);

  // Mulut Standby Kecil
  display.fillRoundRect(mouthX - 4, mouthY, 8, 3, 1, SSD1306_WHITE);

  display.display();
}

// Wajah Happy dengan Parameter Progres (0.0 - 1.0)
void drawFaceHappy(float progress) {
  display.clearDisplay();

  int radius = 10 + (int)(6 * progress); // Membesar halus dari 10 ke 16
  int cutOffset = 8 - (int)(4 * progress); // Cekungan lengkungan

  // Mata Kiri & Kanan ^ ^
  display.fillCircle(leftEyeX, eyeY - 2, radius, SSD1306_WHITE);
  display.fillCircle(leftEyeX, eyeY + cutOffset, radius, SSD1306_BLACK);

  display.fillCircle(rightEyeX, eyeY - 2, radius, SSD1306_WHITE);
  display.fillCircle(rightEyeX, eyeY + cutOffset, radius, SSD1306_BLACK);

  drawSingleEyelash(false, eyeHeight, 0, -2);

  // Mulut Senyum Bertahap
  int mouthRadius = 3 + (int)(7 * progress); // Melebar dari 3 ke 10
  display.fillCircle(mouthX, mouthY - 2, mouthRadius, SSD1306_WHITE);
  display.fillCircle(mouthX, mouthY - 5, mouthRadius, SSD1306_BLACK);

  display.display();
}

// Transisi Halus Masuk ke Mode Happy
void transisiKeHappy() {
  for (int i = 0; i <= 5; i++) {
    float progress = (float)i / 5.0;
    drawFaceHappy(progress);
    jeda(20); // Efek animasi per-frame
  }
}

// Transisi Halus Kembali ke Mode Standby
void transisiKeStandby() {
  for (int i = 5; i >= 0; i--) {
    float progress = (float)i / 5.0;
    drawFaceHappy(progress);
    jeda(20);
  }
}

// Animasi Berkedip Halus
void animasiKedip() {
  for (int h = eyeHeight; h >= 2; h -= 6) {
    drawFaceNormal(h);
    jeda(15);
  }
  for (int h = 2; h <= eyeHeight; h += 6) {
    drawFaceNormal(h);
    jeda(15);
  }
}

// ================= LAYAR INFORMASI =================

void gambarLayar() {
  display.clearDisplay();
  display.setTextSize(1);

  switch (layar) {
    case LAYAR_JAM:   gambarJam();   break;
    case LAYAR_CUACA: gambarCuaca(); break;
    case LAYAR_INFO:  gambarInfo();  break;
  }

  gambarTitikHalaman();
  display.display();
}

// Layar 1: Jam & Tanggal
void gambarJam() {
  char buf[24];
  struct tm t;

  if (ambilWaktu(t)) {
    snprintf(buf, sizeof(buf), "%s, %d %s %d",
             NAMA_HARI[t.tm_wday], t.tm_mday, NAMA_BULAN[t.tm_mon], t.tm_year + 1900);
    tulisTengah(buf, 2, 1);

    int jam = t.tm_hour;
    if (!format24Jam) {
      jam = jam % 12;
      if (jam == 0) jam = 12;
    }

    // Titik dua berkedip tiap detik
    snprintf(buf, sizeof(buf), "%02d%c%02d", jam, (t.tm_sec % 2 == 0) ? ':' : ' ', t.tm_min);
    display.setTextSize(3);
    display.setCursor(19, 18);
    display.print(buf);

    display.setTextSize(1);
    if (!format24Jam) {
      display.setCursor(111, 18);
      display.print(t.tm_hour < 12 ? "AM" : "PM");
    }
    snprintf(buf, sizeof(buf), "%02d", t.tm_sec);
    display.setCursor(111, 32);
    display.print(buf);

    // Bar detik
    display.drawRect(14, 46, 100, 4, SSD1306_WHITE);
    display.fillRect(14, 46, (t.tm_sec + 1) * 100 / 60, 4, SSD1306_WHITE);
  } else {
    tulisTengah(modeSetupWiFi ? "Setup WiFi: see Info" : "Syncing time...", 2, 1);
    tulisTengah("--:--", 18, 3);

    char up[20];
    formatUptime(up, sizeof(up));
    snprintf(buf, sizeof(buf), "Up %s", up);
    tulisTengah(buf, 48, 1);
  }
}

// Layar 2: Cuaca
void gambarCuaca() {
  char buf[24];

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(namaKota.substring(0, 13));

  if (!adaDataCuaca) {
    if (modeSetupWiFi && WiFi.status() != WL_CONNECTED) {
      tulisTengah("Weather needs WiFi", 24, 1);
      tulisTengah("(setup: see Info)", 36, 1);
    } else if (WiFi.status() != WL_CONNECTED) {
      tulisTengah("Connecting WiFi...", 28, 1);
    } else {
      tulisTengah("Loading weather...", 28, 1);
    }
    return;
  }

  // Umur data
  unsigned long menit = (millis() - waktuCuacaDiambil) / 60000;
  if (menit < 60) snprintf(buf, sizeof(buf), "%lum ago", menit);
  else            snprintf(buf, sizeof(buf), "%luh ago", menit / 60);
  tulisKanan(buf, 0);

  gambarIkonCuaca(ikonDariKode(cuaca.kode), 22, 27);

  // Suhu besar
  snprintf(buf, sizeof(buf), "%d", (int)lroundf(cuaca.suhu));
  display.setTextSize(3);
  display.setCursor(50, 14);
  display.print(buf);
  int x = display.getCursorX();
  display.drawCircle(x + 3, 16, 2, SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(x + 7, 14);
  display.print(satuanF ? "F" : "C");

  tulisTengah(labelCuaca(cuaca.kode), 42, 1);

  snprintf(buf, sizeof(buf), "H%d L%d  Hum %d%%",
           (int)lroundf(cuaca.suhuMax), (int)lroundf(cuaca.suhuMin), cuaca.kelembapan);
  tulisTengah(buf, 52, 1);
}

// Layar 3: Info sistem / petunjuk setup WiFi
void gambarInfo() {
  bool tersambung = WiFi.status() == WL_CONNECTED;
  display.setTextSize(1);

  // Belum ada WiFi: tampilkan cara setup dari HP
  if (modeSetupWiFi && !tersambung) {
    tulisTengah("Setup WiFi with app:", 0, 1);
    tulisTengah("ESP BLE Provisioning", 10, 1);
    display.setCursor(0, 24);
    display.print("Choose: no QR code");
    display.setCursor(0, 36);
    display.print("Name: " BLE_NAMA);
    display.setCursor(0, 46);
    display.print("PIN:  " BLE_PIN);
    return;
  }

  display.setCursor(0, 0);
  display.print("WiFi: ");
  display.print(tersambung ? "connected" : "connecting...");

  display.setCursor(0, 10);
  display.print("SSID: ");
  display.print(namaWiFi());

  display.setCursor(0, 20);
  display.print("RSSI: ");
  if (tersambung) {
    int rssi = WiFi.RSSI();
    display.print(rssi);
    display.print(rssi > -60 ? " dBm great" : (rssi > -70 ? " dBm good" : " dBm weak"));
  } else {
    display.print("-");
  }

  // Alamat halaman pengaturan, dibuka di browser HP (WiFi yang sama)
  display.setCursor(0, 32);
  display.print("Settings in browser:");
  display.setCursor(0, 42);
  if (tersambung) {
    String ip = WiFi.localIP().toString();
    if (ip.length() <= 14) display.print("http://");
    display.print(ip);
  } else {
    display.print("(needs WiFi)");
  }

  display.setCursor(0, 52);
  if (konfirmasiResetAktif()) {
    display.print("Hold again: new WiFi");
  } else if (!tersambung) {
    display.print("Hold 2x: new WiFi");
  } else {
    char up[20];
    formatUptime(up, sizeof(up));
    display.print("Up:   ");
    display.print(up);
  }
}

// Titik penanda halaman di bawah (layar wajah tidak dihitung)
void gambarTitikHalaman() {
  int n = JUMLAH_LAYAR - 1;
  int x0 = SCREEN_WIDTH / 2 - (n - 1) * 4;
  for (int i = 0; i < n; i++) {
    int x = x0 + i * 8;
    if (i + 1 == layar) display.fillRect(x - 1, 60, 3, 3, SSD1306_WHITE);
    else                display.drawPixel(x, 61, SSD1306_WHITE);
  }
}

// ================= IKON CUACA =================

int ikonDariKode(int k) {
  if (k == 0) return IKON_CERAH;
  if (k <= 2) return IKON_BERAWAN;
  if (k == 3) return IKON_MENDUNG;
  if (k == 45 || k == 48) return IKON_KABUT;
  if ((k >= 71 && k <= 77) || k == 85 || k == 86) return IKON_SALJU;
  if (k >= 95) return IKON_BADAI;
  return IKON_HUJAN; // Gerimis, hujan, hujan lokal
}

const char* labelCuaca(int k) {
  if (k == 0) return "Clear sky";
  if (k <= 2) return "Partly cloudy";
  if (k == 3) return "Cloudy";
  if (k == 45 || k == 48) return "Foggy";
  if (k >= 51 && k <= 57) return "Drizzle";
  if (k >= 61 && k <= 67) return "Rain";
  if ((k >= 71 && k <= 77) || k == 85 || k == 86) return "Snow";
  if (k >= 80 && k <= 82) return "Rain showers";
  if (k >= 95) return "Thunderstorm";
  return "Unknown";
}

void gambarMatahari(int cx, int cy, int r) {
  display.fillCircle(cx, cy, r, SSD1306_WHITE);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    display.drawLine(cx + (int)(cos(a) * (r + 3)), cy + (int)(sin(a) * (r + 3)),
                     cx + (int)(cos(a) * (r + 6)), cy + (int)(sin(a) * (r + 6)), SSD1306_WHITE);
  }
}

// tambah > 0 untuk menggambar "bayangan" hitam di sekitar awan
void gambarAwan(int cx, int cy, int tambah, uint16_t warna) {
  display.fillCircle(cx - 7, cy + 2, 6 + tambah, warna);
  display.fillCircle(cx + 1, cy - 3, 8 + tambah, warna);
  display.fillCircle(cx + 9, cy + 3, 5 + tambah, warna);
  display.fillRect(cx - 7, cy + 3, 16, 6 + tambah, warna);
}

void gambarIkonCuaca(int ikon, int cx, int cy) {
  switch (ikon) {
    case IKON_CERAH:
      gambarMatahari(cx, cy, 7);
      break;

    case IKON_BERAWAN:
      gambarMatahari(cx - 5, cy - 5, 5);
      gambarAwan(cx + 3, cy + 3, 2, SSD1306_BLACK);
      gambarAwan(cx + 3, cy + 3, 0, SSD1306_WHITE);
      break;

    case IKON_MENDUNG:
      gambarAwan(cx, cy, 0, SSD1306_WHITE);
      break;

    case IKON_KABUT:
      for (int i = 0; i < 3; i++) {
        display.fillRoundRect(cx - 12 + (i % 2) * 3, cy - 7 + i * 6, 22, 3, 1, SSD1306_WHITE);
      }
      break;

    case IKON_HUJAN:
      gambarAwan(cx, cy - 4, 0, SSD1306_WHITE);
      for (int i = 0; i < 3; i++) {
        int x = cx - 6 + i * 7;
        display.drawLine(x, cy + 7, x - 3, cy + 13, SSD1306_WHITE);
      }
      break;

    case IKON_SALJU:
      gambarAwan(cx, cy - 4, 0, SSD1306_WHITE);
      for (int i = 0; i < 3; i++) {
        display.fillCircle(cx - 6 + i * 7, cy + 9 + (i % 2) * 3, 1, SSD1306_WHITE);
      }
      break;

    case IKON_BADAI:
      gambarAwan(cx, cy - 4, 0, SSD1306_WHITE);
      display.drawLine(cx + 1, cy + 5, cx - 3, cy + 9, SSD1306_WHITE);
      display.drawLine(cx - 3, cy + 9, cx + 2, cy + 9, SSD1306_WHITE);
      display.drawLine(cx + 2, cy + 9, cx - 2, cy + 13, SSD1306_WHITE);
      break;
  }
}

// ================= HELPER TEKS =================

void tulisTengah(const char* teks, int y, int ukuran) {
  int16_t x1, y1;
  uint16_t w, h;
  display.setTextSize(ukuran);
  display.getTextBounds(teks, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, y);
  display.print(teks);
}

void tulisKanan(const char* teks, int y) {
  int16_t x1, y1;
  uint16_t w, h;
  display.setTextSize(1);
  display.getTextBounds(teks, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - w, y);
  display.print(teks);
}

void formatUptime(char* buf, size_t n) {
  unsigned long s = millis() / 1000;
  snprintf(buf, n, "%lud %02lu:%02lu:%02lu", s / 86400, (s % 86400) / 3600, (s % 3600) / 60, s % 60);
}

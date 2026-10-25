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

// Pin Sensor Getar SW-420 (DO). VCC sensor ke 3V3, BUKAN 5V (supaya sinyal DO tetap 3.3V)
#define VIBRA_PIN 5 // GPIO5

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
const int AWAL_GETAR = 2;       // Reaksi benturan: 0 = mati, 1 = kurang peka, 2 = normal, 3 = sangat peka
const int AWAL_TIDUR_MENIT = 5; // Tidur setelah 5 menit tanpa aktivitas (0 = tidak pernah)

// ===== Sensor jarak =====
unsigned long lastSensorCheck = 0;
bool adaOrang = false;             // Ada orang dekat (sudah disaring supaya tidak kedip-kedip)
int hitungDekat = 0;
unsigned long waktuTerakhirDekat = 0;

// ===== Sensor getar =====
const unsigned long GETAR_JEDA_MS     = 150;   // Sunyi lebih lama dari ini = satu benturan selesai
const unsigned long GETAR_GONCANG_MS  = 700;   // Getar terus selama ini = digoncang
const unsigned long GETAR_MAKS_MS     = 10000; // Sesi getar lebih lama = sensor terlalu peka, diabaikan
const unsigned long GETAR_SESI_DIAM_MS = 1500; // Sunyi selama ini = sesi getar baru
const unsigned long KETUK_GANDA_MS    = 700;   // 2 ketukan dalam waktu ini = ketuk ganda
const unsigned long ABAIKAN_TOMBOL_MS = 400;   // Menekan tombol ikut menggetarkan kotak

volatile uint32_t jumlahGetar = 0;             // Jumlah perubahan sinyal DO (dihitung di interrupt)
volatile unsigned long waktuGetarTerakhir = 0;
void IRAM_ATTR isrGetar(); // Dideklarasikan dulu (Arduino tidak membuat prototipe fungsi IRAM_ATTR)
bool isrGetarAktif = false;    // Interrupt sensor getar terpasang
uint32_t getarDibaca = 0;
unsigned long lastCekGetar = 0;
bool adaBenturan = false;      // Sedang ada getaran
bool benturanGoncang = false;  // Getaran ini sudah dianggap goncangan
unsigned long benturanMulai = 0;
uint32_t benturanSinyal = 0;
bool sesiGetar = false;        // Sesi getar: getaran yang jedanya < 1.5 detik (lewat tombol / loop tertahan juga)
bool sesiAbaikan = false;      // Sesi terlalu lama (sensor terlalu peka): abaikan sampai benar-benar sunyi
unsigned long sesiGetarMulai = 0;
bool ketukMenunggu = false;    // Ketukan pertama, menunggu ketukan kedua
unsigned long waktuKetuk = 0;  // Awal getaran ketukan pertama
bool ketukLanjutan = false;    // Ketukan sebelumnya sudah dipakai (bangun / ketuk ganda)
unsigned long waktuKetukLanjutan = 0;

// ===== Ekspresi wajah =====
enum { EKS_NORMAL, EKS_PENASARAN, EKS_PUAS, EKS_SENANG, EKS_KAGET, EKS_PUSING,
       EKS_KESAL, EKS_CINTA, EKS_NGANTUK, EKS_TIDUR };

// Status satu frame wajah. Gambar wajah = fungsi murni dari status ini
struct StatusWajah {
  int mood;           // EKS_...
  unsigned long t;    // ms sejak ekspresi ini mulai
  float open;         // Mata terbuka 1.0 ... tertutup 0.0 (kedip & ganti ekspresi)
  float lookX, lookY; // Arah lirikan -1.0 ... 1.0
  float breath;       // Napas pelan -1.0 ... 1.0
};

const unsigned long NGANTUK_MS = 8000; // Lama mengantuk sebelum tidur

int moodTampil = EKS_NORMAL;      // Ekspresi yang sedang tampil
int moodSebelum = EKS_TIDUR;      // Ekspresi sebelumnya (mulai = bangun tidur)
unsigned long moodMulai = 0;
int moodTujuan = EKS_NORMAL;
int faseGanti = 2;                // 0 = diam, 1 = mata menutup (ekspresi lama), 2 = mata membuka (ekspresi baru)
unsigned long faseMulai = 0;

int moodSementara = -1;           // Ekspresi sementara (kaget, pusing, cinta...), -1 = tidak ada
unsigned long sementaraSampai = 0;
int moodBerikut = -1;             // Ekspresi sesudahnya (pusing -> kesal)
unsigned long durasiBerikut = 0;
bool popUlang = false;            // Kaget/pusing lagi saat masih tampil: animasi masuk diulang

unsigned long lastFrame = 0;
unsigned long waktuKedip = 0;     // Mulai kedip terakhir
unsigned long kedipBerikut = 0;
unsigned long kedipKedua = 0;     // Kedip dobel (0 = tidak ada)
float lihatX = 0, lihatY = 0;     // Arah lirikan sekarang
float tujuanX = 0, tujuanY = 0;
float sisiPenasaran = 0.7f;
unsigned long gantiLirik = 0;
unsigned long ekspresiIseng = 0;  // Kapan ekspresi kecil acak berikutnya (penasaran/puas)
float fasaNapas = 0;              // Fase napas, menerus (ganti periode tidak meloncat)
unsigned long lastAktivitas = 0;  // Tombol, orang dekat, getaran
uint32_t hashLayar = 0;           // Frame sama = tidak perlu kirim ulang ke OLED
bool layarRedup = false;

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
int levelGetar = AWAL_GETAR;
int tidurMenit = AWAL_TIDUR_MENIT;

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
unsigned long jedaGagalCuaca = 60000; // Gagal terus (internet putus): coba lagi makin jarang, 1 ... 15 menit

enum { IKON_CERAH, IKON_BERAWAN, IKON_MENDUNG, IKON_KABUT, IKON_HUJAN, IKON_SALJU, IKON_BADAI };

const char* NAMA_HARI[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* NAMA_BULAN[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void setup() {
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Pull-up: tanpa sensor pin tetap HIGH (tidak ada getaran palsu), dan membantu DO modul yang
  // open-collector. Polaritas tidak penting karena yang dihitung perubahan sinyal (CHANGE)
  pinMode(VIBRA_PIN, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for(;;);
  }
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  muatPengaturan();
  aturInterruptGetar(); // Interrupt sensor getar hanya kalau reaksi benturan tidak dimatikan

  // Tidak menunggu WiFi, wajah langsung jalan
  mulaiWiFi();

  // Wajah mulai dengan membuka mata (seperti bangun tidur)
  unsigned long now = millis();
  lastAktivitas = now;
  moodMulai = now;
  faseMulai = now;
  kedipBerikut = now + 2500;
  ekspresiIseng = now + random(20000, 60000);
  urusWajah(now);
}

void loop() {
  bacaTombol();
  cekSensor(millis());
  cekGetar(millis());
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

  // Tekan singkat (atau ketuk ganda) = layar berikutnya. Kalau sedang tidur: bangun saja
  if (eventShortPress) {
    eventShortPress = false;
    lastTombol = currentMillis;
    bool tidur = sedangTidur(currentMillis);
    lastAktivitas = currentMillis;
    if (!tidur) gantiLayar((layar + 1) % JUMLAH_LAYAR);
  }

  // Tekan lama = aksi khusus tiap layar. Kalau sedang tidur: bangun saja
  if (eventLongPress) {
    eventLongPress = false;
    lastTombol = currentMillis;
    bool tidur = sedangTidur(currentMillis);
    lastAktivitas = currentMillis;
    if (!tidur) aksiLongPress();
  }

  // Dielus terus (tombol masih ditahan): mata hati tidak selesai
  if (layar == LAYAR_WAJAH && tombolDitekan && longPressTerpicu && moodSementara == EKS_CINTA) {
    unsigned long m = millis();
    if ((long)(sementaraSampai - (m + 800)) < 0) sementaraSampai = m + 800; // Hanya memperpanjang
    lastAktivitas = m;
  }

  // Kembali ke wajah kalau tombol lama tidak disentuh (petunjuk setup WiFi tetap tampil)
  bool tahanSetup = modeSetupWiFi && layar == LAYAR_INFO;
  if (autoBalikMs > 0 && layar != LAYAR_WAJAH && !tahanSetup && millis() - lastTombol >= autoBalikMs) {
    gantiLayar(LAYAR_WAJAH);
  }

  if (layar == LAYAR_WAJAH) {
    urusWajah(millis());
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
  unsigned long now = millis();
  bool dariLayarLain = (layar != LAYAR_WAJAH);
  layar = baru;
  perluGambar = true;
  hashLayar = 0; // Paksa kirim frame berikutnya ke OLED
  if (layarRedup) {
    layarRedup = false;
    display.dim(false);
  }
  if (layar == LAYAR_WAJAH && dariLayarLain) {
    // Kembali ke wajah: mata membuka, hitungan tidur mulai lagi
    lastAktivitas = now;
    moodSementara = -1;
    moodBerikut = -1;
    moodTampil = pilihMood(now);
    moodSebelum = EKS_TIDUR;
    moodMulai = now;
    faseGanti = 2;
    faseMulai = now;
    kedipBerikut = now + random(2000, 5000);
    kedipKedua = 0;
    // Jadwal idle mulai lagi dari sekarang (bukan langsung menyusul setelah lama di layar lain)
    gantiLirik = now + random(1000, 3000);
    ekspresiIseng = now + random(20000, 60000);
    lihatX = lihatY = tujuanX = tujuanY = 0;
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
      // "Dielus": mata hati sebentar
      setSementara(EKS_CINTA, 2500);
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

  // pulseIn menghitung waktu tunggu sejak dipanggil, termasuk jeda sebelum modul memancar
  // (HC-SR04 klasik ~0.5 ms, klon / HC-SR04P bisa lebih lambat). Jadi ~3 ms untuk awal pantulan
  // + sedikit di atas batas jarak (58 us per cm): tetap singkat supaya animasi wajah tidak tertahan
  unsigned long batasTunggu = 3000 + (batasJarakCm + 5) * 58UL;
  long durasi = pulseIn(ECHO_PIN, HIGH, batasTunggu);
  int jarak = (durasi > 0) ? (durasi * 0.034 / 2) : 999;

  // Saring: 2 bacaan dekat berturut-turut = ada orang, 0.4 detik tanpa bacaan dekat = pergi
  int batas = batasJarakCm + (adaOrang ? 3 : 0);
  if (jarak > 0 && jarak <= batas) {
    if (hitungDekat < 2) hitungDekat++;
    waktuTerakhirDekat = now;
  } else {
    hitungDekat = 0;
  }
  if (!adaOrang && hitungDekat >= 2) adaOrang = true;
  if (adaOrang && now - waktuTerakhirDekat > 400) adaOrang = false;
  if (adaOrang) lastAktivitas = now;
}

// ================= SENSOR GETAR =================

// Dipanggil setiap sinyal DO sensor berubah (naik atau turun), jadi modul yang
// sinyalnya terbalik pun tetap terbaca
void IRAM_ATTR isrGetar() {
  jumlahGetar++;
  waktuGetarTerakhir = millis();
}

// Mati = interrupt dilepas, tidak membebani CPU (modul yang sinyalnya bergetar terus pun tidak mengganggu)
void aturInterruptGetar() {
  bool mau = levelGetar > 0;
  if (mau == isrGetarAktif) return;
  if (mau) attachInterrupt(digitalPinToInterrupt(VIBRA_PIN), isrGetar, CHANGE);
  else detachInterrupt(digitalPinToInterrupt(VIBRA_PIN));
  isrGetarAktif = mau;
}

// Jumlah sinyal minimum supaya dihitung ketukan (makin peka, makin sedikit)
uint32_t minSinyalKetuk() {
  if (levelGetar >= 3) return 1;
  if (levelGetar == 2) return 3;
  return 6;
}

// Kelompokkan sinyal getar jadi: ketukan, ketuk ganda, atau goncangan
void cekGetar(unsigned long now) {
  uint32_t n = jumlahGetar;
  uint32_t baru = n - getarDibaca;
  getarDibaca = n;

  // Loop sempat tertahan lama (mis. sedang ambil cuaca): getaran lama dibuang (kecuali saat tidur, lihat bawah)
  bool basi = now - lastCekGetar > 300;
  lastCekGetar = now;

  // Tombol sedang/baru ditekan: getarannya dari tombol. Tombol menang atas ketukan yang menunggu
  bool tombolBaru = tombolDitekan || now - waktuGantiStatus < ABAIKAN_TOMBOL_MS;
  if (tombolBaru || levelGetar == 0) ketukMenunggu = false;

  // Ketukan tunggal di layar jam/cuaca/info baru dijalankan setelah pasti bukan ketuk ganda
  // (jendela dihitung dari awal ketukan; tunggu kalau ketukan kedua masih bergetar)
  bool keduaMungkin = adaBenturan && benturanMulai - waktuKetuk <= KETUK_GANDA_MS; // Ketukan kedua masih bergetar
  if (ketukMenunggu && !keduaMungkin && now - waktuKetuk > KETUK_GANDA_MS) {
    ketukMenunggu = false;
    if (layar != LAYAR_WAJAH) ketukTunggal(now);
  }

  // Sesi getar: jeda < 1.5 detik = sesi yang sama. Tetap dihitung walau tombol ditekan / loop
  // tertahan, supaya sensor yang terlalu peka tidak "dimulai ulang" oleh tombol atau ambil cuaca
  if (sesiGetar && (long)(now - waktuGetarTerakhir) > (long)GETAR_SESI_DIAM_MS) {
    sesiGetar = false;
    sesiAbaikan = false; // Sudah benar-benar sunyi
  }
  if (baru > 0 && !sesiGetar) {
    sesiGetar = true;
    sesiGetarMulai = now;
  }
  // Getar >10 detik tanpa jeda 1.5 detik: tidak mungkin digoncang selama itu, sensor terlalu peka
  if (sesiGetar && now - sesiGetarMulai > GETAR_MAKS_MS) sesiAbaikan = true;

  // Wajah tidur tapi loop tertahan (ambil cuaca boleh saat tidur): ketukan selama itu tetap membangunkan
  if (basi && levelGetar > 0 && !tombolBaru && !sesiAbaikan && baru >= minSinyalKetuk() && sedangTidur(now)) {
    adaBenturan = false;
    benturanMulai = now;
    adaKetukan(now);
    return;
  }

  // Abaikan: fitur dimatikan, data basi, getaran dari tombol, atau sensor terlalu peka
  if (levelGetar == 0 || basi || tombolBaru || sesiAbaikan) {
    adaBenturan = false;
    return;
  }

  if (baru > 0 && !adaBenturan) {
    adaBenturan = true;
    benturanGoncang = false;
    benturanMulai = now;
    benturanSinyal = 0;
  }
  if (!adaBenturan) return;
  benturanSinyal += baru;

  // Sudah sunyi: getaran selesai. Ketukan = getaran pendek (< 700 ms). Selisih bisa negatif
  // (semua sinyal datang sebelum benturanMulai dicatat), jadi dibandingkan sebagai long
  long diam = (long)(now - waktuGetarTerakhir);
  if (diam > (long)GETAR_JEDA_MS) {
    adaBenturan = false;
    if (!benturanGoncang && (long)(waktuGetarTerakhir - benturanMulai) < (long)GETAR_GONCANG_MS &&
        benturanSinyal >= minSinyalKetuk()) adaKetukan(now);
    return;
  }

  // Masih bergetar: lama = goncangan
  unsigned long lama = now - benturanMulai;
  if (lama >= GETAR_GONCANG_MS && benturanSinyal >= minSinyalKetuk() * 4) {
    if (!benturanGoncang) {
      benturanGoncang = true;
      mulaiGoncang(now);
    } else {
      masihGoncang(now);
    }
  }
}

void adaKetukan(unsigned long now) {
  bool tidur = sedangTidur(now);
  lastAktivitas = now;

  // Ketukan lanjutan dari rangkaian yang sudah dipakai (tok tok tok): diabaikan
  if (ketukLanjutan && now - waktuKetukLanjutan <= KETUK_GANDA_MS) {
    waktuKetukLanjutan = now;
    return;
  }
  ketukLanjutan = false;

  // Dibangunkan dengan ketukan: kesal
  if (tidur) {
    ketukMenunggu = false;
    ketukLanjutan = true;
    waktuKetukLanjutan = now;
    setSementara(EKS_KESAL, 2000);
    return;
  }

  // Masih pusing (ekor goncangan / kotak diletakkan): ketukan diabaikan, tidak pindah layar
  if (layar == LAYAR_WAJAH && moodSementara == EKS_PUSING) {
    ketukMenunggu = false;
    return;
  }

  // Petunjuk setup WiFi tetap tampil (ketukan & ketuk ganda tidak pindah layar)
  if (modeSetupWiFi && layar == LAYAR_INFO) {
    ketukMenunggu = false;
    return;
  }

  // Ketukan kedua (dihitung dari awal getaran ke awal getaran): sama dengan tekan singkat (layar berikutnya)
  if (ketukMenunggu && benturanMulai - waktuKetuk <= KETUK_GANDA_MS) {
    ketukMenunggu = false;
    ketukLanjutan = true;
    waktuKetukLanjutan = now;
    eventShortPress = true;
    return;
  }

  // Ketukan pertama. Di wajah langsung kaget, di layar lain tunggu dulu (lihat cekGetar)
  ketukMenunggu = true;
  waktuKetuk = benturanMulai;
  if (layar == LAYAR_WAJAH) setSementara(EKS_KAGET, 1200);
}

// Ketukan tunggal di layar lain: kembali ke wajah, kaget
void ketukTunggal(unsigned long now) {
  if (modeSetupWiFi && layar == LAYAR_INFO) return; // Petunjuk setup WiFi tetap tampil
  lastTombol = now;
  gantiLayar(LAYAR_WAJAH);
  setSementara(EKS_KAGET, 1200);
}

void mulaiGoncang(unsigned long now) {
  lastAktivitas = now;
  ketukMenunggu = false;
  if (modeSetupWiFi && layar == LAYAR_INFO) return;
  lastTombol = now;
  if (layar != LAYAR_WAJAH) gantiLayar(LAYAR_WAJAH);
  setSementara(EKS_PUSING, 2500, EKS_KESAL, 1500); // Pusing, lalu kesal sebentar
}

void masihGoncang(unsigned long now) {
  lastAktivitas = now;
  // Selama masih digoncang, pusingnya tidak selesai
  if (moodSementara == EKS_PUSING) sementaraSampai = now + 2500;
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
  levelGetar   = pref.getInt("getar", AWAL_GETAR);
  tidurMenit   = pref.getInt("tidur", AWAL_TIDUR_MENIT);
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
  pref.putInt("getar", levelGetar);
  pref.putInt("tidur", tidurMenit);
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
<label>Bump reactions (vibration sensor)</label>
<select name=getar><option value=0 %G0%>Off</option><option value=1 %G1%>Less sensitive</option><option value=2 %G2%>Normal</option><option value=3 %G3%>Very sensitive</option></select>
<small>Tap = surprised, two taps = next screen, shake = dizzy.</small>
<label>Fall asleep after (minutes, 0 = never)</label>
<input name=tidur type=number min=0 max=240 value="%TIDUR%">
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
  html.replace("%G0%", levelGetar == 0 ? "selected" : "");
  html.replace("%G1%", levelGetar == 1 ? "selected" : "");
  html.replace("%G2%", levelGetar == 2 ? "selected" : "");
  html.replace("%G3%", levelGetar == 3 ? "selected" : "");
  html.replace("%TIDUR%", String(tidurMenit));
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
  if (server.hasArg("getar")) levelGetar = constrain(server.arg("getar").toInt(), 0, 3);
  aturInterruptGetar();
  if (server.hasArg("tidur")) tidurMenit = constrain(server.arg("tidur").toInt(), 0, 240);
  lastAktivitas = millis(); // Jangan langsung tidur setelah pengaturan diubah
  simpanPengaturan();

  // Ambil cuaca lagi dengan kota & satuan baru
  adaDataCuaca = false;
  pernahCobaCuaca = false;
  jedaGagalCuaca = 60000;
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

  // Cuaca tiap 15 menit, atau lebih sering kalau belum pernah dapat
  // Ambil cuaca memblokir loop (bisa belasan detik kalau internet putus): update berkala jangan saat
  // wajah sedang hidup. Yang pertama (setelah nyala / ganti kota) langsung, supaya layar cuaca siap
  const unsigned long LIMA_BELAS_MENIT = 15UL * 60 * 1000;
  bool wajahSibuk = (layar == LAYAR_WAJAH && !sedangTidur(now) && pernahCobaCuaca);
  unsigned long intervalCuaca = adaDataCuaca ? LIMA_BELAS_MENIT : jedaGagalCuaca;
  if (!wajahSibuk && (!pernahCobaCuaca || now - waktuCobaCuaca >= intervalCuaca)) {
    pernahCobaCuaca = true;
    waktuCobaCuaca = now;
    if (ambilCuaca()) {
      jedaGagalCuaca = 60000;
    } else if (jedaGagalCuaca < LIMA_BELAS_MENIT) {
      // Gagal (mis. internet putus): coba lagi makin jarang, 1 -> 2 -> 4 -> 8 -> 15 menit
      jedaGagalCuaca *= 2;
      if (jedaGagalCuaca > LIMA_BELAS_MENIT) jedaGagalCuaca = LIMA_BELAS_MENIT;
    }
  }
}

bool ambilWaktu(struct tm &t) {
  time_t sekarang = time(nullptr);
  localtime_r(&sekarang, &t);
  return t.tm_year > (2020 - 1900);
}

// Ambil cuaca dari Open-Meteo (gratis, tanpa API key). true = berhasil
bool ambilCuaca() {
  WiFiClient client; // HTTP biasa: lebih kecil & cepat, data cuaca bukan rahasia
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  bool berhasil = false;

  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + String(latitude, 4) +
               "&longitude=" + String(longitude, 4) +
               "&current=temperature_2m,relative_humidity_2m,weather_code"
               "&daily=temperature_2m_max,temperature_2m_min"
               "&forecast_days=1&timezone=auto";
  if (satuanF) url += "&temperature_unit=fahrenheit";

  if (!http.begin(client, url)) return false;

  if (http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      cuaca.suhu       = doc["current"]["temperature_2m"].as<float>();
      cuaca.kelembapan = doc["current"]["relative_humidity_2m"].as<int>();
      cuaca.kode       = doc["current"]["weather_code"].as<int>();
      cuaca.suhuMax    = doc["daily"]["temperature_2m_max"][0].as<float>();
      cuaca.suhuMin    = doc["daily"]["temperature_2m_min"][0].as<float>();
      adaDataCuaca = true;
      berhasil = true;
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
  return berhasil;
}

// ================= WAJAH =================
// Animasi tidak memakai delay: tiap putaran loop() wajah dihitung ulang dari waktu,
// jadi tombol & sensor tetap terbaca selama kedip / ganti ekspresi.

bool sedangTidur(unsigned long now) {
  return layar == LAYAR_WAJAH && tidurMenit > 0 && now - lastAktivitas >= tidurMenit * 60000UL;
}

int prioritasMood(int m) {
  if (m == EKS_PUSING) return 3;
  if (m == EKS_KAGET || m == EKS_KESAL || m == EKS_CINTA) return 2;
  return 1; // Ekspresi kecil acak (penasaran, puas)
}

// Ekspresi sementara. Tidak menimpa ekspresi yang lebih penting yang masih berjalan
void setSementara(int mood, unsigned long durasi, int berikut, unsigned long durasiSesudah) {
  unsigned long now = millis();
  if (moodSementara >= 0 && (long)(sementaraSampai - now) > 0 &&
      prioritasMood(mood) < prioritasMood(moodSementara)) return;
  moodSementara = mood;
  sementaraSampai = now + durasi;
  moodBerikut = berikut;
  durasiBerikut = durasiSesudah;
  // Diketuk / digoncang lagi saat masih kaget / pusing: animasi masuknya diulang
  if (mood == moodTampil && (mood == EKS_KAGET || mood == EKS_PUSING)) popUlang = true;
}

void setSementara(int mood, unsigned long durasi) {
  setSementara(mood, durasi, -1, 0);
}

// Ekspresi dasar dari keadaan sekitar
int moodDasar(unsigned long now) {
  if (adaOrang) return EKS_SENANG;
  if (tidurMenit > 0) {
    unsigned long diam = now - lastAktivitas;
    unsigned long batas = tidurMenit * 60000UL;
    if (diam >= batas) return EKS_TIDUR;
    if (diam + NGANTUK_MS >= batas) return EKS_NGANTUK;
  }
  return EKS_NORMAL;
}

int pilihMood(unsigned long now) {
  // Ekspresi sementara habis: lanjut ke ekspresi berikutnya (kalau ada)
  if (moodSementara >= 0 && (long)(now - sementaraSampai) >= 0) {
    if (moodBerikut >= 0) {
      moodSementara = moodBerikut;
      sementaraSampai = now + durasiBerikut;
      moodBerikut = -1;
    } else {
      moodSementara = -1;
    }
  }
  int dasar = moodDasar(now);
  // Ekspresi kecil acak hanya kalau sedang santai
  if (moodSementara >= 0 && prioritasMood(moodSementara) == 1 && dasar != EKS_NORMAL) moodSementara = -1;
  return moodSementara >= 0 ? moodSementara : dasar;
}

// Kaget & pusing langsung berganti (tanpa kedip dulu)
bool gantiLangsung(int mood) {
  return mood == EKS_KAGET || mood == EKS_PUSING;
}

unsigned long lamaTutup(int dari, int ke) {
  if (dari == EKS_TIDUR) return 0;     // Mata sudah tertutup
  if (ke == EKS_TIDUR) return 900;     // Pelan-pelan tertidur
  return 70;
}

unsigned long lamaBuka(int dari, int ke) {
  if (ke == EKS_TIDUR) return 0;
  if (dari == EKS_TIDUR) return 350;   // Bangun: mata membuka pelan
  return 110;
}

// Kurva halus 0..1
float halus(float x) {
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  return x * x * (3 - 2 * x);
}

// Bukaan mata saat berkedip (tutup cepat, buka sedikit lebih lambat)
float bukaKedip(unsigned long dt) {
  if (dt < 60)  return 1.0f - halus(dt / 60.0f);
  if (dt < 160) return halus((dt - 60) / 100.0f);
  return 1.0f;
}

bool bisaKedip(int mood) {
  return mood == EKS_NORMAL || mood == EKS_PENASARAN || mood == EKS_KESAL;
}

void urusWajah(unsigned long now) {
  if (now - lastFrame < 33) return; // ~30 fps
  unsigned long dt = now - lastFrame;
  if (dt > 200) dt = 200;
  lastFrame = now;

  // ---- Pilih & ganti ekspresi ----
  int target = pilihMood(now);
  if (gantiLangsung(target) && (target != moodTampil || popUlang)) {
    moodSebelum = moodTampil;
    moodTampil = target;
    moodMulai = now;
    faseGanti = 0;
  } else if (faseGanti == 0 && target != moodTampil && now - waktuKedip >= 160) { // Tunggu kedip selesai, supaya mata tidak meloncat terbuka
    moodTujuan = target;
    faseGanti = 1;
    faseMulai = now;
  }
  popUlang = false;
  if (faseGanti == 1) {
    moodTujuan = target;
    if (moodTujuan == moodTampil) {
      // Batal ganti (mis. orang hanya lewat sebentar): buka mata lagi
      moodSebelum = moodTampil; // Buka mata dengan kecepatan biasa
      faseGanti = 2;
      faseMulai = now;
    } else if (now - faseMulai >= lamaTutup(moodTampil, moodTujuan)) {
      moodSebelum = moodTampil;
      moodTampil = moodTujuan;
      moodMulai = now;
      faseGanti = 2;
      faseMulai = now;
    }
  }
  if (faseGanti == 2 && now - faseMulai >= lamaBuka(moodSebelum, moodTampil)) faseGanti = 0;

  // ---- Bukaan mata ----
  float buka = 1.0f;
  if (faseGanti == 1) {
    unsigned long lama = lamaTutup(moodTampil, moodTujuan);
    buka = lama ? 1.0f - halus((now - faseMulai) / (float)lama) : 0.0f;
  } else if (faseGanti == 2) {
    unsigned long lama = lamaBuka(moodSebelum, moodTampil);
    buka = lama ? halus((now - faseMulai) / (float)lama) : 1.0f;
  } else if (bisaKedip(moodTampil)) {
    // Kedip acak, kadang kedip dobel
    if ((long)(now - kedipBerikut) >= 0) {
      waktuKedip = now;
      kedipBerikut = now + random(2000, 6000);
      kedipKedua = (random(100) < 25) ? now + 260 : 0;
    }
    if (kedipKedua && (long)(now - kedipKedua) >= 0) {
      waktuKedip = now;
      kedipKedua = 0;
    }
    buka = bukaKedip(now - waktuKedip);
  }
  // Tidak sedang bisa kedip (ganti ekspresi, tidur, senang...): jadwal kedip jangan basi
  if (faseGanti != 0 || !bisaKedip(moodTampil)) {
    if ((long)(kedipBerikut - now) < 1000) kedipBerikut = now + random(1000, 3000);
    kedipKedua = 0;
  }

  // ---- Lirikan ----
  if (moodTampil == EKS_PENASARAN || (faseGanti == 1 && moodTujuan == EKS_PENASARAN)) {
    // Penasaran: melirik ke satu sisi (dipilih saat ekspresi dimulai)
    tujuanX = sisiPenasaran;
    tujuanY = -0.3f;
  } else if (moodTampil == EKS_NORMAL && faseGanti == 0) {
    if ((long)(now - gantiLirik) >= 0) {
      if (tujuanX != 0 || tujuanY != 0) {
        // Kembali ke tengah, diam agak lama
        tujuanX = 0;
        tujuanY = 0;
        gantiLirik = now + random(2000, 5000);
      } else {
        const float pilihanX[] = {-1.0f, -0.6f, -0.3f, 0.3f, 0.6f, 1.0f};
        const float pilihanY[] = {-0.6f, -0.3f, 0.0f, 0.0f, 0.4f};
        tujuanX = pilihanX[random(6)];
        tujuanY = pilihanY[random(5)];
        gantiLirik = now + random(1000, 3000);
        if (random(100) < 30) waktuKedip = now; // Kedip saat melirik jauh, seperti orang
      }
    }
    // Ekspresi kecil acak sesekali
    if ((long)(now - ekspresiIseng) >= 0) {
      ekspresiIseng = now + random(20000, 60000);
      if (moodSementara < 0) {
        sisiPenasaran = random(2) ? 0.7f : -0.7f;
        setSementara(random(2) ? EKS_PENASARAN : EKS_PUAS, random(1500, 2500));
      }
    }
  } else if (moodTampil != EKS_NORMAL) {
    tujuanX = 0;
    tujuanY = 0;
    // Jadwal idle mulai lagi setelah kembali normal (bukan langsung menyusul)
    if ((long)(gantiLirik - now) < 1000) gantiLirik = now + random(1000, 3000);
    if ((long)(ekspresiIseng - now) < 20000) ekspresiIseng = now + random(20000, 60000);
  }
  float k = dt * 0.012f; // Lirikan cepat seperti mata sungguhan
  if (k > 1) k = 1;
  lihatX += (tujuanX - lihatX) * k;
  lihatY += (tujuanY - lihatY) * k;

  // ---- Gambar ----
  StatusWajah s;
  s.mood = moodTampil;
  s.t = now - moodMulai;
  s.open = buka;
  s.lookX = lihatX;
  s.lookY = lihatY;
  unsigned long periode = (moodTampil == EKS_TIDUR) ? 5000 : 4000; // Tidur: napas lebih pelan
  fasaNapas += dt * (2 * PI / periode); // Fase menerus: kelopak tidak meloncat saat mulai tidur
  if (fasaNapas >= 2 * PI) fasaNapas -= 2 * PI;
  s.breath = sinf(fasaNapas);

  display.clearDisplay();
  gambarWajah(s);

  // Tidur: OLED diredupkan
  bool redup = (moodTampil == EKS_TIDUR);
  if (redup != layarRedup) {
    layarRedup = redup;
    display.dim(redup);
  }

  // Kirim ke OLED hanya kalau gambarnya berubah (kirim 1 frame ~25 ms)
  uint32_t h = 2166136261UL;
  const uint8_t *buf = display.getBuffer();
  for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT / 8; i++) h = (h ^ buf[i]) * 16777619UL;
  if (h != hashLayar) {
    hashLayar = h;
    display.display();
  }
}

// ===== GAMBAR WAJAH (port dari faces-final.js) =====
// Port 1:1 dari desain wajah final (FaceLab/faces-final.js). Pikselnya harus sama persis
// dengan versi JS, jadi aturannya ketat:
// - Hitungan pecahan memakai double, sama seperti angka di JS. Dengan float, nilai yang
//   tepat di .5 (mis. R(2 + sin 150 derajat)) bisa jatuh ke sisi lain dan 1 dari 5 frame
//   pusing jadi beda. Tetap ringan: paling banyak ~30 sin/cos per frame (< 1 ms).
// - Semua koordinat int. JS I(x) = (int)x, idiv(a,b) = a / b (int), R(x) = wjR(x) =
//   lroundf((float)x), persis R() di JS (dibulatkan ke float dulu, lalu .5 menjauhi nol).
// - '/' biasa di JS = pembagian pecahan (mis. te / 250.0), bukan pembagian int.
// - s.t uint32: putaran dihitung (t % P) dulu sebagai int, baru jadi pecahan. te = min(t, 1000)
//   BERTANDA, supaya te - N boleh negatif (uint32 akan meluap dan animasi masuk terlewat).
// - Urutan gambar sama persis (warna hitam menimpa putih sesuai urutan).
// - Tanpa state global yang berubah: gambar = fungsi murni dari StatusWajah.

static const double WJ_PI = 3.14159265;
static const int WJ_LX = 36, WJ_RX = 92, WJ_EY = 30; // Pusat mata kiri / kanan, garis mata
static const int WJ_EW = 23, WJ_EH = 38;             // Ukuran mata (lebar ganjil = cermin tepat)
static const int WJ_MX = 64, WJ_MY = 54;             // Pusat mulut

// Data bersama satu frame (objek g di JS), dihitung sekali di gambarWajah()
struct WjFrame {
  unsigned long t;       // ms sejak ekspresi mulai
  long te;               // min(t, 1000), bertanda (te - N boleh negatif)
  double op, lx, ly, br; // Bukaan mata, lirikan X/Y, napas (sudah dibatasi)
  int ex, ey, bb;        // Geser mata karena lirikan, naik-turun karena napas
  int mx, my, cy;        // Pusat mulut, garis tengah mata
};

// Prototipe ekspresi ditulis sendiri di sini. Kalau tidak, Arduino IDE membuat prototipe
// otomatis di bagian atas file (sebelum WjFrame dikenal) dan compile gagal.
static void wjWajahNormal(const WjFrame &g);
static void wjWajahPenasaran(const WjFrame &g);
static void wjWajahPuas(const WjFrame &g);
static void wjWajahSenang(const WjFrame &g);
static void wjWajahKaget(const WjFrame &g);
static void wjWajahPusing(const WjFrame &g);
static void wjWajahKesal(const WjFrame &g);
static void wjWajahCinta(const WjFrame &g);
static void wjWajahNgantuk(const WjFrame &g);
static void wjWajahTidur(const WjFrame &g);

// ---- Bantuan matematika ----
static int wjMinI(int a, int b) { return a < b ? a : b; }
static int wjMaxI(int a, int b) { return a > b ? a : b; }
static double wjMaxD(double a, double b) { return a > b ? a : b; }
static int wjR(double x) { return (int)lroundf((float)x); } // R(): ke float, lalu bulatkan (.5 menjauhi nol)
static double wjClamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// smooth(): kurva halus 0..1
static double wjHalus(double k) {
  k = wjClamp(k, 0.0, 1.0);
  return k * k * (3.0 - 2.0 * k);
}

// easeBack(): 0 -> 1 dengan lewat ~10% lalu kembali
static double wjEaseBack(double p) {
  if (p <= 0.0) return 0.0;
  if (p >= 1.0) return 1.0;
  double q = p - 1.0;
  return 1.0 + 2.70158 * q * q * q + 1.70158 * q * q;
}

// squish(): goyang "boing" yang mereda; 0 saat t = 0 dan saat t >= T
static double wjSquish(long t, long T) {
  if (t >= T) return 0.0;
  double p = (double)t / (double)T;
  return (1.0 - p) * (1.0 - p) * sin(p * 3.0 * WJ_PI);
}

// Putaran: t (uint32) dibungkus periodenya dulu (int), baru double
static double wjWave(unsigned long t, unsigned long per) { // -1..1
  return sin((double)(t % per) * (2.0 * WJ_PI / (double)per));
}
static double wjHump(unsigned long t, unsigned long per) { // 0..1..0
  return sin((double)(t % per) * (WJ_PI / (double)per));
}
static double wjProg(unsigned long t, unsigned long per) { // 0..1
  return (double)(t % per) / (double)per;
}

// ---- Bagian wajah ----

// oval: mata kotak-bulat di tengah (cx, cy), sudut maks. 10 seperti mata asli
static void wjOval(int cx, int cy, int w, int h, uint16_t warna) {
  int r = wjMinI(10, wjMinI(w / 2, h / 2));
  display.fillRoundRect(cx - w / 2, cy - h / 2, w, h, r, warna);
}

// lash: bulu mata 2 px miring 45 derajat dari (x, y), ke atas dan ke luar
static void wjBulu(int x, int y, int dir, int n) {
  display.drawLine(x, y, x + dir * n, y - n, SSD1306_WHITE);
  display.drawLine(x, y + 1, x + dir * n, y + 1 - n, SSD1306_WHITE);
}

// lidU: mata terpejam santai, dasar datar 2 px di y, ujung melengkung, ujung luar ada bulu mata
static void wjKelopakU(int cx, int y, int dir) {
  display.fillRect(cx - 7, y, 15, 2, SSD1306_WHITE);
  display.drawLine(cx - 10, y - 3, cx - 7, y, SSD1306_WHITE);
  display.drawLine(cx - 10, y - 2, cx - 8, y, SSD1306_WHITE);
  display.drawLine(cx + 10, y - 3, cx + 7, y, SSD1306_WHITE);
  display.drawLine(cx + 10, y - 2, cx + 8, y, SSD1306_WHITE);
  int ox = cx + 10 * dir; // Sayap bulu mata: dua kotak 2x2 ke luar (cermin tepat)
  display.fillRect(dir > 0 ? ox + 1 : ox - 2, y - 4, 2, 2, SSD1306_WHITE);
  display.fillRect(dir > 0 ? ox + 3 : ox - 4, y - 5, 2, 2, SSD1306_WHITE);
}

// lidArch: mata terpejam tersenyum, lengkung 3 px di y, bulu mata di bahu luar
static void wjKelopakSenyum(int cx, int y, int dir) {
  display.fillCircle(cx, y + 7, 11, SSD1306_WHITE);
  display.fillCircle(cx, y + 10, 11, SSD1306_BLACK);
  display.fillRect(cx - 11, y + 2, 23, 17, SSD1306_BLACK);
  wjBulu(cx + dir * 8, y - 3, dir, 5);
}

// lidBar: mata terpejam datar (kaget / pusing), batang 3 px + bulu mata
static void wjKelopakDatar(int cx, int y, int dir) {
  display.fillRoundRect(cx - 11, y - 1, 23, 3, 1, SSD1306_WHITE);
  wjBulu(cx + dir * 10, y - 2, dir, 4);
}

// lidSqueeze: mata memicing "> <", garis 3 px, ujung lancip ke arah hidung
static void wjKelopakPicing(int cx, int cy, int dir) {
  int ax = cx - 5 * dir;
  for (int i = 0; i < 3; i++) {
    display.drawLine(ax + (9 + i) * dir, cy - 9, ax + i * dir, cy, SSD1306_WHITE);
    display.drawLine(ax + i * dir, cy, ax + (9 + i) * dir, cy + 9, SSD1306_WHITE);
  }
}

// eyeStd: mata tinggi biasa + bulu mata, bukaan op; kalau hampir tertutup jadi lidU
static void wjMataStd(int cx, int cy, int dir, int w, int h, double op) {
  int hh = wjR(h * op);
  if (hh < 7) {
    wjKelopakU(cx, cy + 1, dir);
    return;
  }
  int c = wjMaxI(cy, 5 + hh / 2); // Bulu mata tetap di dalam layar
  wjOval(cx, c, w, hh, SSD1306_WHITE);
  wjBulu(cx + dir * (w / 2 - 5), c - hh / 2 + 2, dir, 7);
}

// blush: pipi merona, tiga garis miring pendek ///
static void wjRona(int cx, int cy) {
  for (int i = -1; i <= 1; i++) display.drawLine(cx + i * 4 - 1, cy + 2, cx + i * 4 + 2, cy - 1, SSD1306_WHITE);
}

// sparkle: kilau 4 arah, tengahnya padat kalau besar
static void wjKilau(int x, int y, int n) {
  display.drawFastHLine(x - n, y, 2 * n + 1, SSD1306_WHITE);
  display.drawFastVLine(x, y - n, 2 * n + 1, SSD1306_WHITE);
  if (n >= 2) display.fillRect(x - 1, y - 1, 3, 3, SSD1306_WHITE);
}

// heart: hati, jari-jari lobus r, pusat lobus di baris y, ujung di y + 2r + 2
static void wjHati(int cx, int y, int r) {
  display.fillCircle(cx - r, y, r, SSD1306_WHITE);
  display.fillCircle(cx + r, y, r, SSD1306_WHITE);
  display.fillTriangle(cx - 2 * r, y + 1, cx + 2 * r, y + 1, cx, y + 2 * r + 2, SSD1306_WHITE);
}

// vein: tanda urat marah, empat siku 2 px, sudutnya menghadap ke tengah
static void wjUrat(int vx, int vy, int gp) {
  for (int q = 0; q < 4; q++) {
    int sx = (q & 1) ? 1 : -1, sy = (q & 2) ? 1 : -1;
    int x0 = vx + sx * gp, y0 = vy + sy * gp;
    display.fillRect(sx > 0 ? x0 : x0 - 3, sy > 0 ? y0 : y0 - 1, 4, 2, SSD1306_WHITE);
    display.fillRect(sx > 0 ? x0 : x0 - 1, sy > 0 ? y0 : y0 - 3, 2, 4, SSD1306_WHITE);
  }
}

// ---- Mulut ----

// mouthCat: mulut kucing 'w', lebar 13 px
static void wjMulutKucing(int x, int y) {
  display.fillCircle(x - 3, y, 3, SSD1306_WHITE);
  display.fillCircle(x + 3, y, 3, SSD1306_WHITE);
  display.fillCircle(x - 3, y - 3, 3, SSD1306_BLACK);
  display.fillCircle(x + 3, y - 3, 3, SSD1306_BLACK);
  display.fillRect(x - 6, y - 3, 13, 2, SSD1306_BLACK);
}

// mouthO: mulut 'o' (cincin)
static void wjMulutO(int x, int y, int r) {
  display.fillCircle(x, y, r, SSD1306_WHITE);
  if (r > 3) display.fillCircle(x, y, r - 2, SSD1306_BLACK);
  else display.fillRect(x - 1, y - 1, 3, 3, SSD1306_BLACK);
}

// mouthSmile: senyum kecil 'u', 2 px
static void wjMulutSenyum(int x, int y, int r) {
  display.fillCircle(x, y, r, SSD1306_WHITE);
  display.fillCircle(x, y - 2, r, SSD1306_BLACK);
}

// mouthD: senyum lebar 'D' padat, atasnya datar di y
static void wjMulutD(int x, int y, int r) {
  display.fillCircle(x, y, r, SSD1306_WHITE);
  display.fillRect(x - r, y - r, 2 * r + 1, r, SSD1306_BLACK);
}

// mouthFrown: cemberut (trapesium), 2 px
static void wjMulutCemberut(int x, int y) {
  display.fillRect(x - 3, y - 2, 7, 2, SSD1306_WHITE);
  display.drawLine(x - 4, y - 1, x - 7, y + 2, SSD1306_WHITE);
  display.drawLine(x - 4, y, x - 7, y + 3, SSD1306_WHITE);
  display.drawLine(x + 4, y - 1, x + 7, y + 2, SSD1306_WHITE);
  display.drawLine(x + 4, y, x + 7, y + 3, SSD1306_WHITE);
}

// mouthWave: mulut berombak (pusing), ~1.75 gelombang, 2 px
static void wjMulutGelombang(int x, int y, double ph) {
  int px = x - 12, py = y + wjR(2.0 * sin(ph));
  for (int i = 1; i <= 6; i++) {
    int nx = x - 12 + 4 * i;
    int ny = y + wjR(2.0 * sin(ph + (double)i * 1.83));
    display.drawLine(px, py, nx, ny, SSD1306_WHITE);
    display.drawLine(px, py + 1, nx, ny + 1, SSD1306_WHITE);
    px = nx;
    py = ny;
  }
}

// ---- Ekspresi ----

// normal: mata biasa, masuk dengan "boing"; mata di sisi lirikan sedikit lebih tinggi
static void wjWajahNormal(const WjFrame &g) {
  double k = wjSquish(g.te, 420);
  int w = WJ_EW + 2 * wjR(k * 2.0);
  int h = WJ_EH - wjR(k * 6.0);
  int p = wjR(g.lx * 2.0);
  wjMataStd(WJ_LX + g.ex, g.cy, -1, w, h - p, g.op);
  wjMataStd(WJ_RX + g.ex, g.cy, 1, w, h + p, g.op);
  wjMulutKucing(g.mx, g.my);
}

// curious (penasaran): kepala miring, satu mata lebih besar, mulut 'o', tanda '?'
static void wjWajahPenasaran(const WjFrame &g) {
  double k = wjEaseBack((double)g.te / 260.0);
  int tilt = wjR(2.0 * k + wjHalus((double)g.te / 600.0) * wjWave(g.t, 1800)); // + goyang pelan
  int p = wjR(g.lx * 2.0);
  wjMataStd(WJ_LX + g.ex, g.cy + wjR(2.0 * k) + tilt, -1, WJ_EW - 2 * wjR(k), WJ_EH - wjR(10.0 * k) - p, g.op);
  wjMataStd(WJ_RX + g.ex, g.cy + wjR(3.0 * k) - tilt, 1, WJ_EW + 2 * wjR(2.0 * k), WJ_EH + wjR(2.0 * k) + p, g.op);
  wjMulutO(g.mx + wjR(3.0 * k), g.my, 3);
  if (g.t > 120) { // '?' besar, turun 1 px lalu naik-turun. (t % 900 + 455) = (t + 455) tanpa meluap
    display.setTextSize(2);
    display.setCursor(4, g.t < 220 ? 1 : 2 + wjR(wjWave(g.t % 900 + 455, 900)));
    display.print('?');
  }
}

// eyeContent: mata puas, pipi (lingkaran r15) naik dari bawah, sisa bulan sabit tebal
static void wjMataPuas(int cx, int cy, int dir, double k0, double op) {
  double k = wjMaxD(k0, 1.0 - op); // Menutup saat masuk -> langsung pose akhir
  int w = WJ_EW + 2 * wjR(k);       // 23 -> 25
  int h = WJ_EH - wjR(13.0 * k);   // 38 -> 25
  int r = 10 + wjR(2.0 * k);       // Sudut -> lingkaran penuh
  int top = cy - h / 2;
  display.fillRoundRect(cx - w / 2, top, w, h, r, SSD1306_WHITE);
  int th = 3 + wjR((wjR(h - (h - 10) * k) - 3) * op); // Tebal pita di tengah
  if (th < h) {
    int ct = top + th;             // Atas lingkaran pipi
    int ch = wjMinI(31, 64 - ct);  // Dekat tepi bawah jadi pipih
    display.fillRoundRect(cx - 15, ct, 31, ch, 15, SSD1306_BLACK);
    if (ct + 15 < top + h) display.fillRect(cx - 13, ct + 15, 27, top + h - ct - 15, SSD1306_BLACK);
  }
  wjBulu(cx + dir * (6 + wjR(2.0 * k)), top + 2 + wjR(k), dir, 7 - wjR(k));
}

// content (puas): mata sabit tersenyum, senyum kecil, not musik bersenandung
static void wjWajahPuas(const WjFrame &g) {
  double k = g.te < 20 ? 0.0 : 0.5 + 0.5 * wjHalus((double)(g.te - 20) / 230.0);
  int c = g.cy - wjR(2.0 * k);
  wjMataPuas(WJ_LX + g.ex, c, -1, k, g.op);
  wjMataPuas(WJ_RX + g.ex, c, 1, k, g.op);
  wjMulutSenyum(g.mx, g.my - 3, 5);
  // Not musik naik 16 px selama 1350 ms, lalu hilang 450 ms sebelum not berikutnya
  if (g.t >= 200) {
    double p = wjProg(g.t - 200, 1800);
    if (p < 0.75) {
      double q = p / 0.75;
      int nx = 120 + wjR(1.5 * sin(q * 2.0 * WJ_PI)), ny = 26 - wjR(16.0 * q);
      display.fillCircle(nx, ny, 2, SSD1306_WHITE);
      display.fillRect(nx + 1, ny - 7, 2, 7, SSD1306_WHITE);
      display.drawLine(nx + 3, ny - 7, nx + 5, ny - 5, SSD1306_WHITE);
      display.drawLine(nx + 3, ny - 6, nx + 5, ny - 4, SSD1306_WHITE);
    }
  }
}

// eyeHappy: kelopak bawah naik jadi pita ^ tebal (6 px terbuka, 3 px tertutup)
static void wjMataSenang(int cx, int cy, int dir, double k, double op) {
  double ks = wjMaxD(wjClamp(k, 0.0, 1.0), 1.0 - op); // Menutup saat masuk -> lengkung akhir
  int w = WJ_EW + 2 * wjR(2.0 * k);                     // 23 -> 27
  int r = 10 + wjR(3.0 * ks);                           // Atas jadi setengah lingkaran
  int h = WJ_EH - wjR(8.0 * ks);
  int top = cy - 19 + wjR(8.0 * ks);
  int th = wjR(3.0 + 3.0 * op);
  int ct = top + th + wjR((h - th) * (1.0 - ks) * op); // Atas lingkaran pemotong
  display.fillRoundRect(cx - w / 2, top, w, h, r, SSD1306_WHITE);
  if (ct < top + h) {
    int rc = wjMinI(13, (63 - ct) / 2); // Tetap di dalam layar saat rendah
    display.fillCircle(cx, ct + rc, rc, SSD1306_BLACK);
    if (ct + rc < top + h) display.fillRect(cx - 14, ct + rc, 29, top + h - ct - rc, SSD1306_BLACK);
  }
  wjBulu(cx + dir * (6 + wjR(3.0 * ks)), top + 2 + wjR(2.0 * ks), dir, 7 - wjR(ks));
}

// happy (senang): mata ^ ^, mulut D, pipi merona, kilau di pojok, lompat kecil
static void wjWajahSenang(const WjFrame &g) {
  double k = wjEaseBack((double)g.te / 250.0);
  // Lompat 2 px tiap 650 ms, baru sesudah animasi masuk (te bertanda: te - 250 boleh negatif)
  int hop = -wjR(2.0 * wjHump(g.t, 650) * wjHalus((double)(g.te - 250) / 200.0));
  int cy = g.cy + hop;
  wjMataSenang(WJ_LX + g.ex, cy, -1, k, g.op);
  wjMataSenang(WJ_RX + g.ex, cy, 1, k, g.op);
  if (g.te > 120) {
    int bx = g.ex - wjR(g.ex * 0.5); // Pipi bergeser setengah dari mata
    wjRona(WJ_LX - 6 + bx, cy + 15);
    wjRona(WJ_RX + 6 + bx, cy + 15);
  }
  wjMulutD(g.mx, g.my - 4 + hop, 3 + wjR(4.0 * wjClamp(k, 0.0, 1.0)));
  int a = wjR(1.5 + 1.5 * wjWave(g.t, 900)); // Kilau pojok berkedip bergantian
  wjKilau(7, 8, a + 1);
  wjKilau(120, 9, 4 - a);
}

// surprised (kaget): mata gepeng -> melar -> tenang, pupil kecil, mulut 'o', '!' dan keringat
static void wjWajahKaget(const WjFrame &g) {
  long te = g.te;
  double w, h;
  if (te < 80) {
    double q = (double)te / 80.0;
    w = 33.0 - 10.0 * q;
    h = 28.0 + 18.0 * q;
  } else if (te < 170) {
    double q = (double)(te - 80) / 90.0;
    w = 23.0 + 6.0 * q;
    h = 46.0 - 8.0 * q;
  } else if (te < 260) {
    double q = (double)(te - 170) / 90.0;
    w = 29.0 - 2.0 * q;
    h = 38.0 + 2.0 * q;
  } else {
    w = 27.0;
    h = 40.0;
  }
  int wi = wjR(w) | 1; // Lebar ganjil supaya cermin tepat
  int jump = te < 300 ? -wjR(3.0 * sin((double)te * WJ_PI / 300.0)) : 0;
  int jit = te < 600 ? (((te / 60) & 1) ? 1 : -1) : 0;
  int hh = wjR(h * g.op);
  int pm = wjMinI(wi, hh) / 2 - 5; // Gerak pupil: >= 3 px dari tepi mata
  int pdx = wjMaxI(-pm, wjMinI(pm, jit + wjR(g.lx * 3.0)));
  for (int e = 0; e < 2; e++) {
    int dir = e ? 1 : -1;
    int cx = (e ? WJ_RX : WJ_LX) + g.ex;
    if (hh < 12) {
      wjKelopakDatar(cx, g.cy + 1, dir);
      continue;
    }
    int c = wjMaxI(g.cy + jump, 5 + hh / 2);
    wjOval(cx, c, wi, hh, SSD1306_WHITE); // Mata biasa, membesar jadi 27x40 ...
    if (hh >= 18) display.fillCircle(cx + pdx, c + wjR(g.ly * ((hh - 18) / 4)), 2, SSD1306_BLACK); // ... pupil kecil
    wjBulu(cx + dir * (wi / 2 - 6), c - hh / 2 + 2, dir, 7);
  }
  wjMulutO(g.mx, g.my + jump, 4);
  if (te > 60) { // '!' tebal
    display.fillRoundRect(62 + jit, 1, 5, 10, 2, SSD1306_WHITE);
    display.fillRect(62 + jit, 13, 5, 3, SSD1306_WHITE);
  }
  double sp = wjProg(g.t, 1500);
  if (te > 150 && sp <= 0.85) { // Tetes keringat meluncur, hilang sebentar, lalu menetes lagi
    int sy = 20 + wjR(4.0 * sp);
    display.fillCircle(120, sy, 3, SSD1306_WHITE);
    display.fillTriangle(117, sy - 1, 123, sy - 1, 120, sy - 7, SSD1306_WHITE);
    display.drawFastVLine(119, sy - 1, 2, SSD1306_BLACK);
  }
}

// spiral: mata spiral, 2 cincin + inti; baji hitam berputar membuka cincin dan dua jembatan
// menyambung inti -> cincin tengah -> cincin luar. s = +1 / -1 mencerminkan arahnya
static void wjSpiral(int cx, int cy, double a, int s) {
  display.fillCircle(cx, cy, 13, SSD1306_WHITE);
  display.fillCircle(cx, cy, 10, SSD1306_BLACK);
  display.fillCircle(cx, cy, 7, SSD1306_WHITE);
  display.fillCircle(cx, cy, 4, SSD1306_BLACK);
  display.fillCircle(cx, cy, 2, SSD1306_WHITE);
  display.fillTriangle(cx, cy, cx + wjR(16.0 * cos(a - 0.35)), cy + wjR(16.0 * sin(a - 0.35)),
                       cx + wjR(16.0 * cos(a + 0.35)), cy + wjR(16.0 * sin(a + 0.35)), SSD1306_BLACK);
  double a0 = a + s * 0.5, a1 = a - s * 0.5;
  double c0 = cos(a0), s0 = sin(a0), c1 = cos(a1), s1 = sin(a1);
  for (int i = 0; i < 2; i++) {
    double r0 = i ? 5.5 : 1.5, r1 = i ? 11.5 : 5.5;
    int x0 = cx + wjR(r0 * c0), y0 = cy + wjR(r0 * s0);
    int x1 = cx + wjR(r1 * c1), y1 = cy + wjR(r1 * s1);
    display.drawLine(x0, y0, x1, y1, SSD1306_WHITE);
    display.drawLine(x0 + 1, y0, x1 + 1, y1, SSD1306_WHITE);
    display.drawLine(x0, y0 + 1, x1, y1 + 1, SSD1306_WHITE);
    display.drawLine(x0 + 1, y0 + 1, x1 + 1, y1 + 1, SSD1306_WHITE);
  }
}

// dizzy (pusing): mata spiral berputar, wajah goyang, mulut berombak, bintang mengitari kepala
static void wjWajahPusing(const WjFrame &g) {
  unsigned long tq = g.t - g.t % 75; // Putar / orbit / goyang bergerak tiap 75 ms yang sama
  double amp = 3.0 + 2.0 * wjClamp(1.0 - (double)g.te / 500.0, 0.0, 1.0); // Hentakan awal besar
  int wob = wjR(amp * wjWave(tq, 900));
  int sway = wjR(2.0 * wjWave(tq, 900));
  double a = (double)(tq % 900) * (2.0 * WJ_PI / 900.0);
  int cover = wjR(28.0 * (1.0 - g.op)); // Kelopak turun menutupi spiral
  for (int e = 0; e < 2; e++) {
    int dir = e ? 1 : -1;
    int cx = (e ? WJ_RX : WJ_LX) + g.ex + wob;
    int cy = g.cy + (e ? -sway : sway);
    if (g.op < 0.3) {
      wjKelopakDatar(cx, cy, dir);
      continue;
    }
    wjSpiral(cx, cy, e ? WJ_PI - a : a, e ? -1 : 1);
    if (cover > 0) display.fillRect(cx - 14, cy - 14, 29, cover, SSD1306_BLACK);
    // Bulu mata di luar cincin luar, ikut tepi kelopak (tidak pernah memotong spiral)
    int lxo = 10 + wjR(3.0 * wjClamp((double)(cover - 4) / 6.0, 0.0, 1.0));
    wjBulu(cx + dir * lxo, wjMaxI(cy - 10, cy - 14 + cover), dir, 5);
  }
  wjMulutGelombang(g.mx + wob, g.my, (double)(tq % 700) * (2.0 * WJ_PI / 700.0));
  for (int i = 0; i < 3; i++) { // Bintang mengitari kepala, yang di depan lebih besar
    double b = (double)(tq % 1500) * (2.0 * WJ_PI / 1500.0) + (double)i * 2.094;
    double sb = sin(b);
    if (sb < -0.5) continue; // Sepertiga belakang orbit tersembunyi di balik kepala
    wjKilau(64 + wjR(16.0 * cos(b)), 4 + wjR(2.0 * sb), sb > 0.0 ? 2 : 1);
  }
}

// annoyed (kesal): "> <" dulu, lalu melotot dengan kelopak miring, melirik ke samping,
// mulut cemberut, hembusan "hmph", urat marah
static void wjWajahKesal(const WjFrame &g) {
  long te = g.te; // Bertanda: te - 450 / te - 650 boleh negatif
  int shake = te < 300 ? (((te / 50) & 1) ? 1 : -1) : 0; // Geleng "hmph"
  double go = wjEaseBack((double)(te - 450) / 200.0);
  int aside = -wjR(5.0 * wjHalus((double)(te - 650) / 300.0));
  int hh = wjR(30.0 * go * g.op);
  int cy = g.cy + 4;
  for (int e = 0; e < 2; e++) {
    int dir = e ? 1 : -1;
    int cx = (e ? WJ_RX : WJ_LX) + g.ex + shake + aside;
    if (hh < 9) { // Juga keadaan tertutup (open < 0.3)
      wjKelopakPicing(cx, cy, dir);
      continue;
    }
    wjOval(cx, cy, WJ_EW, hh, SSD1306_WHITE);
    int top = cy - hh / 2;
    int drop = wjR(14.0 * wjClamp(go, 0.0, 1.0) * g.op);
    // Kelopak miring: sudut dalam lebih rendah
    display.fillTriangle(cx - dir * 13, top - 1, cx + dir * 13, top - 1, cx - dir * 13, top + drop, SSD1306_BLACK);
    if (hh >= 16) display.fillCircle(cx + aside, cy + hh / 2 - 7, 2, SSD1306_BLACK); // Pupil melirik
    wjBulu(cx + dir * 8, top + 1, dir, 5);
  }
  int fx = g.mx + shake + aside / 2;
  wjMulutCemberut(fx, g.my + 1);
  // Hembusan "hmph": cincin di samping mulut dan kepulan kecil yang menjauh
  double pp = wjProg(g.t, 2200);
  if (g.t > 250 && pp < 0.45) {
    double q = pp / 0.45;
    int px = fx + 13 + wjR(5.0 * q), py = g.cy + 25; // >= 2 px di bawah mata
    wjMulutO(px, py, 3);
    display.fillCircle(px + 6 + wjR(3.0 * q), py + 1 - wjR(q), 2, SSD1306_WHITE);
  }
  wjUrat(121, 8, te < 120 ? 1 : ((g.t % 700) < 160 ? 3 : 2));
}

// love (cinta): mata hati berdetak "lub-dub", pipi merona, hati kecil melayang naik
static void wjWajahCinta(const WjFrame &g) {
  double k = 0.3 + 0.7 * wjEaseBack((double)g.te / 280.0);
  unsigned long ph = g.t % 900;
  int beat = (ph < 108 || (ph >= 198 && ph < 306)) ? 1 : 0;
  int r = wjMinI(8, wjR((6 + beat) * k * (0.3 + 0.7 * g.op))); // Hati mengecil saat menutup
  int y = g.cy - 4;
  for (int e = 0; e < 2; e++) {
    int dir = e ? 1 : -1;
    int cx = (e ? WJ_RX : WJ_LX) + g.ex;
    if (r < 3) {
      wjKelopakSenyum(cx, g.cy, dir);
      continue;
    }
    wjHati(cx, y, r);
    wjBulu(cx + dir * (2 * r - 1), y - r + 2, dir, 6);
  }
  int bx = g.ex - wjR(g.ex * 0.5);
  wjRona(WJ_LX - 4 + bx, g.cy + 16);
  wjRona(WJ_RX + 4 + bx, g.cy + 16);
  wjMulutKucing(g.mx, g.my);
  // Hati kecil naik di antara mata (ikut lirikan), mengecil di atas, hilang sebentar
  double p = wjProg(g.t, 1400);
  if (p < 0.8) wjHati(64 + g.ex + wjR(2.0 * sin(p * 2.0 * WJ_PI)), 22 - wjR(18.0 * p), p < 0.65 ? 2 : 1);
}

// drowsyLid: tutupan kelopak 0..1. Turun, melorot pelan, tertidur, tersentak bangun (putaran
// 3.2 s); mulai 6.7 s kelopak turun terus sampai tertutup, lalu tidur melanjutkan pose itu
static double wjKelopakNgantuk(unsigned long t) {
  if (t < 300) return 0.45 * wjHalus((double)t / 300.0);
  if (t >= 6700) return 0.45 + 0.55 * wjHalus((double)(t - 6700) / 1000.0); // Tertidur, tanpa sentak
  unsigned long q = (t - 300) % 3200;
  if (q < 2100) return 0.45 + 0.37 * wjHalus((double)q / 2100.0); // Melorot pelan ...
  if (q < 2400) return 0.82;                                       // ... tertidur sebentar
  if (q < 2560) return 0.82 - 0.52 * wjHalus((double)(q - 2400) / 160.0); // Tersentak bangun
  return 0.3 + 0.15 * wjHalus((double)(q - 2560) / 640.0);
}

// drowsy (ngantuk): kelopak berat, mengangguk, menguap, 'z' kecil
static void wjWajahNgantuk(const WjFrame &g) {
  double f0 = wjKelopakNgantuk(g.t);
  double ya = 0.0; // Menguap tiap dua putaran (tidak saat tertidur akhir)
  if (g.t >= 300 && g.t < 6700) {
    unsigned long q = (g.t - 300) % 3200;
    if ((((g.t - 300) / 3200) & 1) == 0 && q >= 300 && q < 1500) ya = sin((double)(q - 300) * WJ_PI / 1200.0);
  }
  double f = wjClamp(f0 + 0.12 * ya, 0.0, 1.0);
  f = f + (1.0 - f) * (1.0 - g.op); // Kedip / ganti ekspresi menutup kelopak
  int nod = wjR(wjClamp((f0 - 0.3) * 6.0, 0.0, 3.0));
  int cy = g.cy + nod;
  int top = cy - WJ_EH / 2;
  int dy = wjR(WJ_EH * f); // Dalam kelopak: garis kelopak di baris top+dy-1, top+dy
  for (int e = 0; e < 2; e++) {
    int dir = e ? 1 : -1;
    int cx = (e ? WJ_RX : WJ_LX) + g.ex;
    if (dy >= 31) { // Tertutup: U di garis kelopak terakhir
      wjKelopakU(cx, top + 30, dir);
      continue;
    }
    wjOval(cx, cy, WJ_EW, WJ_EH, SSD1306_WHITE);
    if (dy > 0) display.fillRect(cx - 12, top - 1, 25, dy, SSD1306_BLACK);
    if (dy < 9) { // Kelopak masih di atas yang bulat: potong datar + bulu mata biasa
      wjBulu(cx + dir * (6 + dy / 3), top + dy + 2, dir, 7);
      continue;
    }
    display.fillRect(cx - 12, top + dy - 1, 25, 2, SSD1306_WHITE); // Garis kelopak berat
    display.drawLine(cx + dir * 12, top + dy - 1, cx + dir * 15, top + dy + 2, SSD1306_WHITE); // Bulu mata layu
    display.drawLine(cx + dir * 12, top + dy, cx + dir * 14, top + dy + 2, SSD1306_WHITE);
  }
  if (ya > 0.1) { // Menguap: 'O' tinggi
    int oh = wjR(3.0 + 8.0 * ya), w = 7 + 2 * wjR(ya);
    int my = g.my + 1 - oh / 2;
    display.fillRoundRect(g.mx - w / 2, my, w, oh, 3, SSD1306_WHITE);
    if (oh > 6) display.fillRoundRect(g.mx - w / 2 + 2, my + 2, w - 4, oh - 4, 2, SSD1306_BLACK);
  } else {
    display.fillRect(g.mx - 3, g.my, 7, 2, SSD1306_WHITE);
  }
  if (g.t >= 300 && g.t < 6700) { // 'z' kecil naik saat melorot, hilang saat tersentak
    unsigned long q = (g.t - 300) % 3200;
    if (q >= 400 && q < 2400) {
      double zp = (double)(q - 400) / 2000.0;
      display.setTextSize(1);
      display.setCursor(116 + wjR(4.0 * zp), 16 - wjR(12.0 * zp));
      display.print('z');
    }
  }
}

// sleep (tidur): mata terpejam, pipi merona, mulut ikut napas, "z z Z" melayang
static void wjWajahTidur(const WjFrame &g) {
  int y = g.cy + 14; // Kelopak tepat di posisi akhir ngantuk (angguk 3)
  wjKelopakU(WJ_LX + g.ex, y, -1);
  wjKelopakU(WJ_RX + g.ex, y, 1);
  int bx = g.ex - wjR(g.ex * 0.5);
  wjRona(WJ_LX - 4 + bx, y + 7);
  wjRona(WJ_RX + 4 + bx, y + 7);
  wjMulutO(g.mx, g.my, g.br > 0.2 ? 4 : 3); // Mulut ikut napas
  // z z Z muncul satu per satu, yang tertua hilang dulu (putaran 3 s); yang terakhir Z besar
  unsigned long q = g.t % 3000;
  for (int i = 0; i < 3; i++) {
    unsigned long a0 = (unsigned long)i * 600UL;
    if (q < a0 || q >= a0 + 1800) continue;
    double zp = (double)(q - a0) / 1800.0;
    display.setTextSize(i == 2 ? 2 : 1);
    display.setCursor((i == 0 ? 100 : (i == 1 ? 108 : 116)) + wjR(2.0 * zp),
                      (i == 0 ? 24 : (i == 1 ? 15 : 4)) - wjR((i == 2 ? 4.0 : 5.0) * zp));
    display.print(i == 2 ? 'Z' : 'z');
  }
}

// Gambar wajah satu frame. Mesin animasi sudah memanggil display.clearDisplay()
void gambarWajah(const StatusWajah &s) {
  WjFrame g;
  g.t = s.t;
  g.te = s.t < 1000UL ? (long)s.t : 1000L; // Bertanda, supaya te - N boleh negatif
  g.op = wjClamp(s.open, 0.0, 1.0);
  g.lx = wjClamp(s.lookX, -1.0, 1.0);
  g.ly = wjClamp(s.lookY, -1.0, 1.0);
  g.br = wjClamp(s.breath, -1.0, 1.0);
  g.ex = wjR(g.lx * 6.0);
  g.ey = wjR(g.ly * 4.0);
  g.bb = wjR(g.br);
  g.mx = WJ_MX + wjR(g.lx * 2.0);
  g.my = WJ_MY + wjR(g.ly) + g.bb;
  g.cy = WJ_EY + g.ey + g.bb;
  switch (s.mood) {
    case EKS_PENASARAN: wjWajahPenasaran(g); break;
    case EKS_PUAS:      wjWajahPuas(g);      break;
    case EKS_SENANG:    wjWajahSenang(g);    break;
    case EKS_KAGET:     wjWajahKaget(g);     break;
    case EKS_PUSING:    wjWajahPusing(g);    break;
    case EKS_KESAL:     wjWajahKesal(g);     break;
    case EKS_CINTA:     wjWajahCinta(g);     break;
    case EKS_NGANTUK:   wjWajahNgantuk(g);   break;
    case EKS_TIDUR:     wjWajahTidur(g);     break;
    default:            wjWajahNormal(g);    break;
  }
}
// ===== AKHIR GAMBAR WAJAH =====

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

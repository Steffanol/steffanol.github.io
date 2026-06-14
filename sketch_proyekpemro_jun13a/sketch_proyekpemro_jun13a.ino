#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <HX711.h>

// ─── Konfigurasi WiFi ───────────────────────────────────────────
const char* ssid     = "NAMA_WIFI_KAMU";
const char* password = "PASSWORD_WIFI_KAMU";

// ─── URL Railway (ganti dengan URL deploy kamu) ─────────────────
const String URL_BASE          = "https://nama-app-kamu.up.railway.app";
const String urlDeteksi        = URL_BASE + "/api/deteksi-ultrasonik";
const String urlKonfirmasi     = URL_BASE + "/api/konfirmasi-loadcell";
const String urlCekServo       = URL_BASE + "/api/cek-servo";

// ─── Pin Ultrasonik ─────────────────────────────────────────────
const int pinTrig = 12;
const int pinEcho = 13;

// ─── Pin Servo ──────────────────────────────────────────────────
const int pinServo = 14;
Servo penutupPos;

// ─── Pin HX711 (Load Cell) ──────────────────────────────────────
const int pinDT  = 26;   // Data pin HX711
const int pinSCK = 27;   // Clock pin HX711
HX711 loadCell;
float faktorKalibrasi = -420.0;  // Sesuaikan setelah kalibrasi

// ─── State Mesin ────────────────────────────────────────────────
bool objekTerdeteksi       = false;   // Ultrasonik sudah deteksi, tunggu load cell
unsigned long waktuDeteksi = 0;
const unsigned long TIMEOUT_KONFIRMASI = 5000;  // 5 detik tunggu load cell konfirmasi
const unsigned long JEDA_POLLING_SERVO = 3000;  // Cek perintah servo tiap 3 detik
unsigned long waktuPollingServo        = 0;

void setup() {
  Serial.begin(115200);

  // Ultrasonik
  pinMode(pinTrig, OUTPUT);
  pinMode(pinEcho, INPUT);

  // Servo
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  penutupPos.setPeriodHertz(50);
  penutupPos.attach(pinServo, 500, 2400);
  penutupPos.write(0);

  // Load Cell HX711
  loadCell.begin(pinDT, pinSCK);
  loadCell.set_scale(faktorKalibrasi);
  loadCell.tare();  // Tare (nol-kan) saat startup
  Serial.println("[LOADCELL] Tare selesai. Siap membaca berat.");

  // WiFi
  WiFi.begin(ssid, password);
  Serial.print("[WIFI] Menghubungkan");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WIFI] Terhubung! IP: " + WiFi.localIP().toString());
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Terputus, mencoba reconnect...");
    WiFi.reconnect();
    delay(2000);
    return;
  }

  // ── 1. Polling perintah buka servo dari web (tiap 3 detik) ──
  if (millis() - waktuPollingServo >= JEDA_POLLING_SERVO) {
    waktuPollingServo = millis();
    cekPerintahTombolWeb();
  }

  // ── 2. Baca ultrasonik (hanya jika belum ada objek pending) ──
  if (!objekTerdeteksi) {
    long jarak = bacaJarakUltrasonik();
    Serial.printf("[ULTRASONIK] Jarak: %ld cm\n", jarak);

    if (jarak > 0 && jarak < 15) {
      Serial.println("[ULTRASONIK] Objek terdeteksi! Mengirim ke server, menunggu konfirmasi load cell...");
      kirimDeteksiUltrasonik("Paket");
      objekTerdeteksi = true;
      waktuDeteksi    = millis();
    }
  }

  // ── 3. Jika ada objek pending → pantau load cell ──
  if (objekTerdeteksi) {
    float berat = 0;
    if (loadCell.is_ready()) {
      berat = loadCell.get_units(3);  // Rata-rata 3 pembacaan (gram)
      if (berat < 0) berat = 0;       // Buang nilai negatif karena noise
      Serial.printf("[LOADCELL] Berat terbaca: %.1f gram\n", berat);
    }

    if (berat > 10.0) {
      // Berat terdeteksi → konfirmasi ke server
      Serial.printf("[LOADCELL] Berat terkonfirmasi: %.1f gram. Mengirim konfirmasi...\n", berat);
      kirimKonfirmasiLoadCell((int)berat);
      objekTerdeteksi = false;  // Reset state
    } else if (millis() - waktuDeteksi > TIMEOUT_KONFIRMASI) {
      // Timeout: objek mungkin hanya lewat, bukan paket
      Serial.println("[TIMEOUT] Load cell tidak konfirmasi dalam 5 detik. Deteksi dibatalkan.");
      batalkanPending();
      objekTerdeteksi = false;
    }
  }

  delay(500);
}

// ─── Fungsi Baca Ultrasonik ─────────────────────────────────────
long bacaJarakUltrasonik() {
  digitalWrite(pinTrig, LOW);
  delayMicroseconds(2);
  digitalWrite(pinTrig, HIGH);
  delayMicroseconds(10);
  digitalWrite(pinTrig, LOW);
  long durasi = pulseIn(pinEcho, HIGH, 30000);  // Timeout 30ms
  if (durasi == 0) return -1;                    // Tidak ada echo
  return durasi * 0.034 / 2;
}

// ─── Kirim Deteksi Ultrasonik ke Server ─────────────────────────
void kirimDeteksiUltrasonik(String jenisObjek) {
  HTTPClient http;
  http.begin(urlDeteksi);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"jenis\":\"" + jenisObjek + "\"}";
  int kode = http.POST(payload);
  Serial.printf("[HTTP POST ultrasonik] Kode: %d\n", kode);
  http.end();
}

// ─── Kirim Konfirmasi Load Cell ke Server ───────────────────────
void kirimKonfirmasiLoadCell(int beratGram) {
  HTTPClient http;
  http.begin(urlKonfirmasi);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"berat\":" + String(beratGram) + "}";
  int kode = http.POST(payload);
  Serial.printf("[HTTP POST loadcell] Kode: %d, Berat: %dg\n", kode, beratGram);
  http.end();
}

// ─── Batalkan Pending jika Timeout ──────────────────────────────
void batalkanPending() {
  // Kirim berat=0 ke server → server akan batalkan pending
  HTTPClient http;
  http.begin(urlKonfirmasi);
  http.addHeader("Content-Type", "application/json");
  http.POST("{\"berat\":0}");
  http.end();
  Serial.println("[HTTP] Pending dibatalkan di server.");
}

// ─── Polling Perintah Buka Servo dari Web ───────────────────────
void cekPerintahTombolWeb() {
  HTTPClient http;
  http.begin(urlCekServo);

  int kode = http.GET();
  if (kode == 200) {
    String respons = http.getString();
    if (respons.indexOf("\"buka\":true") >= 0) {
      Serial.println("[SERVO] Perintah diterima: Membuka pintu...");
      penutupPos.write(90);
      delay(4000);
      penutupPos.write(0);
      Serial.println("[SERVO] Pintu tertutup kembali.");
    }
  } else {
    Serial.printf("[HTTP GET servo] Gagal, kode: %d\n", kode);
  }
  http.end();
}
# ☁️ DarkCloud MEGA v6.1

**Deneyap Kart (ESP32-S3) üzerinde çalışan yerel ağ mesajlaşma ve dosya paylaşım sistemi.**

[![Live Demo](https://img.shields.io/badge/🌐_Demo-GitHub_Pages-blue)](https://eren-oztk.github.io/DarkCloud-MEGA)

WiFi SoftAP modunda kendi ağını oluşturur; bağlanan kullanıcılar tarayıcı üzerinden WhatsApp benzeri bir arayüzle mesajlaşabilir ve dosya paylaşabilir.

---

## Özellikler

- WhatsApp tarzı modern mobil arayüz (responsive)
- Kullanıcı profilleri — rastgele renk, emoji avatar, online/offline/yazıyor durumu
- Emoji seçici
- Dosya yükleme ve indirme (SD kart üzerinden)
- XOR şifreleme (SD'ye yazılan mesajlar; *gerçek kriptografi değildir*, ayrıntılar aşağıda)
- NVS (Preferences) ile şifreleme durumu kalıcı olarak saklanır
- Admin paneli (/clearChat, /setEncryption uç noktaları)
- Toast bildirimi + yeni mesaj ses efekti (WebAudio API)

---

## Donanım & Pin Bağlantıları

### Hedef Kart

| Parametre | Değer |
|-----------|-------|
| Kart      | **Deneyap Kart** (ESP32-S3 tabanlı) |
| Güç       | USB veya harici 5 V |
| Flash     | NVS (Preferences) ile şifreleme ayarı saklanır |

### SD Kart — Özel SPI Pinleri

Deneyap Kart'ta varsayılan SPI0 pinleriyle çakışmamak için ayrı pinler kullanılır.

| SD Kart Pini | Deneyap Kart Pini | Açıklama |
|:------------:|:-----------------:|----------|
| SCK          | **D15**           | SPI saat sinyali |
| MISO         | **D9**            | Kart → Mikrodenetleyici veri |
| MOSI         | **D14**           | Mikrodenetleyici → Kart veri |
| CS (SS)      | **D13**           | Chip Select (aktif LOW) |
| VCC          | 3.3 V             | Kart beslemesi (5 V toleranssız) |
| GND          | GND               | Ortak toprak |

> **Not:** Piyasadaki bazı SD modülleri 5 V → 3.3 V çevirici içerir.
> Deneyap Kart GPIO'ları 3.3 V seviyesindedir; doğrudan SD modülü kullanıyorsanız
> seviye çevirici (level shifter) veya uyumlu modül kullanın.

### Devre Şeması (Özet)

```
Deneyap Kart          SD Kart Modülü
─────────────         ──────────────
D15 (SCK)  ──────────── SCK
D9  (MISO) ──────────── MISO
D14 (MOSI) ──────────── MOSI
D13 (CS)   ──────────── CS
3.3V       ──────────── VCC
GND        ──────────── GND
```

Başka harici bileşen **gerekmez** — WiFi SoftAP tamamen dahilidir.

---

## Kütüphaneler

Arduino IDE veya arduino-cli ile aşağıdaki kütüphaneler gereklidir:

| Kütüphane      | Kaynak                        |
|----------------|-------------------------------|
| `WiFi.h`       | ESP32 Arduino Core (dahili)   |
| `WebServer.h`  | ESP32 Arduino Core (dahili)   |
| `SPI.h`        | Arduino Core (dahili)         |
| `SD.h`         | Arduino Core / ESP32 (dahili) |
| `Preferences.h`| ESP32 Arduino Core (dahili)   |

ESP32 desteği için Board Manager'a şu URL'yi ekleyin:

```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

---

## Kurulum

1. Repoyu klonlayın:
   ```bash
   git clone <repo-url>
   ```

2. `DarkCloud_MEGA_v6.1.ino` dosyasını Arduino IDE ile açın.

3. **Kimlik bilgilerini güncelleyin** (`secrets.h` önerilir, bkz. Güvenlik):
   ```cpp
   const char* WIFI_SSID     = "DarkCloud_MEGA";
   const char* WIFI_PASSWORD = "darkcloud2025";  // En az 8 karakter
   const char* ADMIN_TOKEN   = "gizli_admin_tok";
   ```

4. Arduino IDE'de kart seçimi:
   - **Board:** Deneyap Kart (veya ESP32S3 Dev Module)
   - **Port:** Bağlı COM portunu seçin

5. Kodu derleyip yükleyin (`Ctrl+U`).

6. Seri monitörü 115200 baud ile açın — başlangıç mesajlarını ve IP adresini göreceksiniz.

7. Telefon veya bilgisayardan `DarkCloud_MEGA` ağına bağlanın ve tarayıcıda `192.168.4.1` adresini açın.

---

## HTTP API Uç Noktaları

| Uç Nokta | Metot | Açıklama |
|----------|-------|----------|
| `/` | GET | Ana HTML arayüzü |
| `/chatoku` | GET | Chat geçmişini HTML olarak döndürür |
| `/chatyaz` | POST | Yeni mesaj kaydeder (`nick`, `mesaj` parametreleri) |
| `/users` | GET | Aktif kullanıcıları JSON olarak döndürür |
| `/typing` | GET | Kullanıcı durumunu günceller (`nick`, `status`) |
| `/dosyaListesi` | GET | SD'deki dosyaları HTML kart olarak listeler |
| `/clearChat` | GET | Chat geçmişini siler (admin) |
| `/setEncryption` | GET | Şifrelemeyi açar/kapar (`status=1\|0`) |
| `/upload` | POST | Dosya yükler (multipart/form-data) |
| `/<dosyaadi>` | GET | SD'den dosya indirir |

---

## Güvenlik Notları

> Bu proje yerel ağ / eğitim amaçlıdır. Açık internete maruz bırakmayın.

| Konu | Durum | Öneri |
|------|-------|-------|
| WiFi şifresi kodda gömülü | ⚠️ | `secrets.h` + `.gitignore` kullanın |
| XOR şifreleme | ⚠️ Zayıf | Gerçek gizlilik için **AES-GCM** (mbedtls) kullanın |
| `/clearChat` herkese açık | ⚠️ | `ADMIN_TOKEN` kontrolünü aktif edin |
| `/setEncryption` herkese açık | ⚠️ | Token koruması ekleyin |
| XSS | ✅ | `escapeHTML()` ile önlendi (v6) |
| Path Traversal | ✅ | `..` içeren yollar reddedilir (v6) |
| Nick/Mesaj doğrulama | ✅ | Sunucu taraflı uzunluk kontrolü (v6) |
| Bağlantı sınırı | ✅ | `maxConn=8` (DoS önlemi) |

### Kimlik Bilgilerini Koda Gömmeyin

`secrets.h` dosyası oluşturun ve `.gitignore`'a ekleyin:

```cpp
// secrets.h  ← GİT'E COMMIT ETMEYİN
#pragma once
#define WIFI_SSID     "Agim"
#define WIFI_PASSWORD "guclu_sifre"
#define ADMIN_TOKEN   "rastgele_uzun_token"
```

Ana `.ino` dosyasında:
```cpp
#include "secrets.h"
```

---

## Değişiklik Günlüğü

### v6.1
- Pin tanımları Deneyap Kart için güncellendi

### v6.0
- `escapeHTML()` eklendi — XSS önlendi
- Nick/mesaj uzunluğu sunucu taraflı doğrulandı
- `/upload`'da path traversal koruması
- `/typing`'de status whitelist doğrulaması
- `onNotFound`'da `..` içeren yollar engellendi
- `WiFi.softAP maxConn=8` ile DoS sınırlaması
- WhatsApp tarzı mesaj balonları (mine/other)
- Tam mobil uyumlu sidebar toggle
- Toast bildirimi ve emoji picker iyileştirmeleri
- Textarea otomatik yükseklik

---

## Lisans

Kişisel / eğitim amaçlı kullanım için serbesttir.

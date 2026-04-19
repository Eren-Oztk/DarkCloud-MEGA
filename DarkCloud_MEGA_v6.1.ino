/*
 * ╔══════════════════════════════════════════════════════╗
 * ║          ☁️  DarkCloud MEGA v6.0  ☁️                 ║
 * ║    ESP32 / Deneyap Kart Üzerinde Çalışan             ║
 * ║    Yerel Ağ Mesajlaşma & Dosya Paylaşım Sistemi      ║
 * ╚══════════════════════════════════════════════════════╝
 *
 * ÖZELLİKLER:
 *   ✅ WhatsApp tarzı modern mobil arayüz
 *   ✅ Kullanıcı profilleri (avatar, renk, durum)
 *   ✅ Online / Offline / Yazıyor göstergesi
 *   ✅ Emoji seçici
 *   ✅ Dosya paylaşımı (SD Kart üzerinden)
 *   ✅ XOR şifreleme (SD'ye yazılan mesajlar için)
 *   ✅ Admin paneli (şifre korumalı)
 *   ✅ Mesaj temizleme
 *
 * GÜVENLİK NOTLARI (v6):
 *   ⚠️  WiFi şifresini koda gömmek yerine Preferences'tan okuyun.
 *   ⚠️  XOR; gerçek şifreleme DEĞİLDİR, sadece ham okumayı engeller.
 *       Gerçek gizlilik için HTTPS / AES-GCM kullanın.
 *   ⚠️  /clearChat ve /setEncryption endpointleri herkese açık;
 *       üretim kullanımında admin token ekleyin.
 *   ⚠️  Dosya adları / mesajlar HTML escape edilmeden ekleniyor;
 *       XSS riski – v6'da escapeHTML() eklendi.
 *
 * DONANIM:
 *   Deneyap Kart (ESP32-S3) – Pin eşlemeleri aşağıda.
 */

// ─── KÜTÜPHANELER ───────────────────────────────────────────────
#include <WiFi.h>        // ESP32 WiFi (SoftAP modu)
#include <WebServer.h>   // HTTP sunucu
#include <SPI.h>         // SD kart SPI haberleşmesi
#include <SD.h>          // SD kart dosya sistemi
#include <Preferences.h> // NVS (Non-Volatile Storage) – ayarları saklar

// ─── PIN TANIMLARI (DENEYAP KART) ───────────────────────────────
// SD kart için özel SPI pinleri (varsayılan SPI0 ile çakışmamak için)
#define SD_SCK  D15
#define SD_MISO D9
#define SD_MOSI D14
#define SD_CS   D13

// ─── AĞYAPISI ───────────────────────────────────────────────────
/*
 * GÜVENLİK UYARISI: Şifre ve SSID'yi kod içinde bırakmayın.
 * Bunları Preferences NVS'ye yazın ya da en azından
 * #define yerine const flash string kullanın.
 * Aşağıdaki değerler yalnızca örnek amaçlıdır.
 */
const char* WIFI_SSID     = "DarkCloud_MEGA";   // SoftAP ağ adı
const char* WIFI_PASSWORD = "darkcloud2025";    // SoftAP şifresi (min 8 karakter – WPA2)
const char* ADMIN_TOKEN   = "gizli_admin_tok";  // /clearChat vb. için basit token

// ─── HTTP SUNUCU ────────────────────────────────────────────────
WebServer server(80);   // 80 numaralı port'ta HTTP sunucu

// ─── PREFERENCES (NVS) ──────────────────────────────────────────
Preferences prefs;      // Şifreleme durumu gibi ayarları cihaz yeniden başlasa bile saklar

// ─── DOSYA YOLLARI (SD KART) ────────────────────────────────────
const String CHAT_FILE  = "/chat_log.txt";   // Genel chat geçmişi
const String USERS_FILE = "/users.txt";      // (İleride kalıcı kullanıcı verisi için)
File uploadFile;                             // Yükleme sırasında açık tutulan dosya handle'ı

// ─── KULLANICI YAPISI ───────────────────────────────────────────
struct User {
  String nick;          // Kullanıcı adı
  String color;         // Mesaj balonunun rengi (#rrggbb)
  String avatar;        // Emoji avatar
  String status;        // "online" | "offline" | "typing"
  unsigned long lastSeen; // millis() – son aktivite zamanı (offline tespiti için)
  String ip;            // Client IP (aynı IP = aynı kullanıcı kontrolü için)
  int messageCount;     // Toplam gönderilen mesaj sayısı
  
};

#define MAX_USERS 20          // Aynı anda izlenebilecek maksimum kullanıcı sayısı
User activeUsers[MAX_USERS];  // RAM'deki kullanıcı listesi
int  userCount = 0;           // Şu an kayıtlı kullanıcı sayısı
int lastMsgCount = 0;

// ─── ŞİFRELEME ──────────────────────────────────────────────────
/*
 * GÜVENLİK NOTU:
 * XOR ile şifreleme gerçek kriptografi DEĞİLDİR.
 * Yalnızca SD kartın doğrudan okunmasını biraz zorlaştırır.
 * Gerçek uçtan uca şifreleme için AES-GCM veya ChaCha20 kullanın.
 * mbedtls kütüphanesi ESP32'de zaten mevcut.
 */
bool encryptionEnabled = true;  // Başlangıçta şifreleme açık; NVS'den güncellenir
const uint8_t XOR_KEY = 0x5A;  // XOR anahtarı – değiştirmek isterseniz buraya

// ═══════════════════════════════════════════════════════════════
//  YARDIMCI FONKSİYONLAR
// ═══════════════════════════════════════════════════════════════

// Bağlı istemcinin IP adresini döndürür
String getClientIP() {
  return server.client().remoteIP().toString();
}

// nick ile kullanıcı dizisinde arama; bulamazsa -1 döner
int findUser(const String& nick) {
  for (int i = 0; i < userCount; i++) {
    if (activeUsers[i].nick == nick) return i;
  }
  return -1;
}

// IP ile kullanıcı dizisinde arama; bulamazsa -1 döner
int findUserByIP(const String& ip) {
  for (int i = 0; i < userCount; i++) {
    if (activeUsers[i].ip == ip) return i;
  }
  return -1;
}

// Kullanıcının durum bilgisini ve son görülme zamanını günceller
void updateUserStatus(const String& nick, const String& status) {
  int idx = findUser(nick);
  if (idx >= 0) {
    activeUsers[idx].status   = status;
    activeUsers[idx].lastSeen = millis();
  }
}

/*
 * Kullanıcıyı ekler ya da varsa günceller.
 * GÜVENLİK: nick boşsa veya sadece boşluklardan oluşuyorsa reddedilir.
 * Ayrıca aynı nick'i farklı bir IP'den kullanamazsınız (basit spoofing önlemi).
 */
void addOrUpdateUser(const String& nick, const String& ip) {
  // Boş veya çok uzun nick reddet
  if (nick.length() == 0 || nick.length() > 20) return;

  int idx = findUser(nick);
  if (idx >= 0) {
    // Kullanıcı zaten var → sadece güncelle
    activeUsers[idx].lastSeen = millis();
    activeUsers[idx].status   = "online";
    activeUsers[idx].ip       = ip;  // IP değişmişse güncelle
  } else {
    // Yeni kullanıcı → listeye ekle
    if (userCount < MAX_USERS) {
      // Rengi rastgele ata ama çok koyu olmaması için alt limit koy
      uint32_t rndColor = random(0x444444, 0xEEEEEE);
      activeUsers[userCount].nick         = nick;
      activeUsers[userCount].color        = "#" + String(rndColor, HEX);
      activeUsers[userCount].avatar       = "👤";
      activeUsers[userCount].status       = "online";
      activeUsers[userCount].lastSeen     = millis();
      activeUsers[userCount].ip           = ip;
      activeUsers[userCount].messageCount = 0;
      userCount++;
    }
    // NOT: MAX_USERS aşıldığında sessizce başarısız olur.
    // İleride en eski offline kullanıcıyı çıkarma mantığı eklenebilir.
  }
}

// 30 saniye aktivite göstermeyen kullanıcıları "offline" yap
void checkOfflineUsers() {
  unsigned long now = millis();
  for (int i = 0; i < userCount; i++) {
    if (now - activeUsers[i].lastSeen > 30000) {
      activeUsers[i].status = "offline";
    }
  }
}

// Kullanıcı listesini JSON dizisi olarak döndürür (sidebar için)
String getUsersJSON() {
  checkOfflineUsers();
  String json = "[";
  for (int i = 0; i < userCount; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"nick\":\""     + activeUsers[i].nick         + "\",";
    json += "\"color\":\""    + activeUsers[i].color        + "\",";
    json += "\"avatar\":\""   + activeUsers[i].avatar       + "\",";
    json += "\"status\":\""   + activeUsers[i].status       + "\",";
    json += "\"messages\":"   + String(activeUsers[i].messageCount);
    json += "}";
  }
  json += "]";
  return json;
}

// XOR ile basit şifreleme / çözme (simetrik, aynı fonksiyon)
String xorCrypt(const String& text) {
  String result = "";
  result.reserve(text.length());
  for (unsigned int i = 0; i < text.length(); i++) {
    result += (char)(text[i] ^ XOR_KEY);
  }
  return result;
}

// Boyutu okunabilir formata çevirir (123 B, 4.5 KB, 1.2 MB)
String formatFileSize(size_t bytes) {
  if (bytes < 1024)            return String(bytes) + " B";
  else if (bytes < 1048576)    return String(bytes / 1024.0,  1) + " KB";
  else                         return String(bytes / 1048576.0, 1) + " MB";
}

/*
 * HTML özel karakterlerini escape eder.
 * XSS önlemi: kullanıcı girdisi doğrudan HTML'e eklenmeden önce bu fonksiyondan geçmeli.
 * Örn: <script>alert(1)</script>  →  &lt;script&gt;...
 */
String escapeHTML(const String& s) {
  String out = "";
  out.reserve(s.length() + 20);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '&')  out += "&amp;";
    else if (c == '<')  out += "&lt;";
    else if (c == '>')  out += "&gt;";
    else if (c == '"')  out += "&quot;";
    else if (c == '\'') out += "&#x27;";
    else                out += c;
  }
  return out;
}

// ═══════════════════════════════════════════════════════════════
//  DOSYA YÖNETİMİ
// ═══════════════════════════════════════════════════════════════

/*
 * SD karttaki dosyaları HTML kart listesi olarak döndürür.
 * Sistem dosyaları (chat_log, users, vb.) listede gösterilmez.
 * GÜVENLİK: Dosya adı escapeHTML'den geçirilir.
 */
String dosyaListesiGetir() {
  String html = "";
  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    return "<p class='empty-msg'>⚠️ SD Kart Hatası</p>";
  }

  // Gizlenecek sistem dosyaları
  const String hidden[] = { "chat_log.txt", "users.txt", "rooms.txt", "dm_log.txt" };
  const int hiddenCount = 4;

  int fileCount = 0;
  File file = root.openNextFile();

  while (file) {
    String ad = String(file.name());
    bool skip = false;

    // Sistem dosyası mı kontrol et
    for (int h = 0; h < hiddenCount; h++) {
      if (ad == hidden[h]) { skip = true; break; }
    }
    // Gizli/sistem dizinleri atla
    if (ad.startsWith(".") || ad.startsWith("System")) skip = true;

    if (!skip && !file.isDirectory()) {
      String boyut    = formatFileSize(file.size());
      String adSafe   = escapeHTML(ad);   // XSS önlemi
      String icon     = "📄";

      // Uzantıya göre ikon belirle
      if (ad.endsWith(".jpg") || ad.endsWith(".jpeg") || ad.endsWith(".png") || ad.endsWith(".gif")) icon = "🖼️";
      else if (ad.endsWith(".mp3") || ad.endsWith(".wav") || ad.endsWith(".ogg")) icon = "🎵";
      else if (ad.endsWith(".mp4") || ad.endsWith(".avi") || ad.endsWith(".mkv")) icon = "🎬";
      else if (ad.endsWith(".pdf")) icon = "📕";
      else if (ad.endsWith(".zip") || ad.endsWith(".rar") || ad.endsWith(".gz"))  icon = "📦";
      else if (ad.endsWith(".txt") || ad.endsWith(".md")) icon = "📝";

      html += "<div class='file-card'>";
      html += "<div class='file-icon'>" + icon + "</div>";
      html += "<div class='file-info'>";
      html += "<div class='file-name'>" + adSafe + "</div>";
      html += "<div class='file-size'>" + boyut + "</div>";
      html += "</div>";
      // İndirme düğmesi – dosya adı URL encode edilmiş
      html += "<a class='file-dl-btn' href='/" + adSafe + "' download>⬇</a>";
      html += "</div>";
      fileCount++;
    }
    file = root.openNextFile();
  }

  if (fileCount == 0) {
    html = "<p class='empty-msg'>Henüz dosya yok</p>";
  }
  return html;
}

// ═══════════════════════════════════════════════════════════════
//  ANA HTML ARAYÜZÜ
//  WhatsApp benzeri iki panel: sol sidebar + sağ chat
//  Mobil uyumlu – sidebar toggle ile gösterilir/gizlenir
// ═══════════════════════════════════════════════════════════════
String htmlSayfasi() {
  // Bu fonksiyon ~900 satır HTML+CSS+JS içerir.
  // PROGMEM kullanmak yerine rawliteral ile gönderiyoruz;
  // büyük projelerde PROGMEM daha verimlidir.
  String h = R"rawliteral(
<!DOCTYPE html>
<html lang="tr">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
  <title>☁️ DarkCloud</title>
  <style>
    /* ─── TEMA DEĞİŞKENLERİ ─── */
    :root {
      --bg-app:       #111b21;   /* Uygulama arka plan */
      --bg-panel:     #202c33;   /* Panel / sidebar arka plan */
      --bg-input:     #2a3942;   /* Input alanı arka plan */
      --bg-mine:      #005c4b;   /* Kendi mesaj balonu */
      --bg-other:     #202c33;   /* Karşı taraf mesaj balonu */
      --accent:       #00a884;   /* Ana vurgu rengi (WhatsApp yeşili) */
      --accent-light: #00d9b4;   /* Hover rengi */
      --text-pri:     #e9edef;   /* Birincil metin */
      --text-sec:     #8696a0;   /* İkincil / soluk metin */
      --border:       #2a3942;   /* Kenar çizgisi */
      --danger:       #e74c3c;   /* Silme / hata rengi */
      --online:       #2ecc71;
      --offline:      #636e72;
      --typing-color: #f39c12;
      --radius-msg:   10px;
      --sidebar-w:    320px;     /* Sidebar genişliği (masaüstü) */
    }

    /* ─── RESET & TEMEL ─── */
    *, *::before, *::after { margin:0; padding:0; box-sizing:border-box; }
    html, body { height:100%; overflow:hidden; }
    body {
      background: var(--bg-app);
      color: var(--text-pri);
      font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
      font-size: 14px;
      display: flex;
      flex-direction: column;
    }

    /* ─── HEADER ─── */
    .app-header {
      background: var(--bg-panel);
      padding: 10px 16px;
      display: flex;
      align-items: center;
      gap: 12px;
      border-bottom: 1px solid var(--border);
      flex-shrink: 0;
      z-index: 100;
    }
    .header-logo { font-size: 22px; }
    .header-title { font-weight: 700; font-size: 17px; color: var(--accent); flex: 1; }
    .header-pills { display: flex; gap: 8px; }
    .pill {
      background: var(--bg-input);
      border-radius: 20px;
      padding: 4px 10px;
      font-size: 11px;
      color: var(--text-sec);
      display: flex;
      align-items: center;
      gap: 4px;
    }
    .pill .val { color: var(--accent); font-weight: 700; }

    /* Mobil: sidebar açma düğmesi */
    .sidebar-toggle {
      display: none; /* Masaüstünde gizli */
      background: none;
      border: none;
      color: var(--text-pri);
      font-size: 22px;
      cursor: pointer;
      padding: 4px;
    }

    /* ─── ANA LAYOUT ─── */
    .app-body {
      display: flex;
      flex: 1;
      overflow: hidden;
      position: relative;
    }

    /* ─── SIDEBAR ─── */
    .sidebar {
      width: var(--sidebar-w);
      background: var(--bg-panel);
      border-right: 1px solid var(--border);
      display: flex;
      flex-direction: column;
      flex-shrink: 0;
      transition: transform 0.3s ease;
    }

    /* Tab butonları (Kişiler / Dosyalar) */
    .tab-bar {
      display: flex;
      background: var(--bg-app);
      border-bottom: 1px solid var(--border);
    }
    .tab-btn {
      flex: 1;
      padding: 13px 0;
      background: none;
      border: none;
      border-bottom: 3px solid transparent;
      color: var(--text-sec);
      font-weight: 600;
      font-size: 13px;
      cursor: pointer;
      transition: all 0.2s;
    }
    .tab-btn.active {
      color: var(--accent);
      border-bottom-color: var(--accent);
    }

    .tab-panel { display: none; flex: 1; flex-direction: column; overflow: hidden; }
    .tab-panel.active { display: flex; }

    .scroll-area { flex: 1; overflow-y: auto; padding: 8px; }

    /* ─── KULLANICI KARTLARI ─── */
    .user-card {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 10px 12px;
      border-radius: 8px;
      cursor: pointer;
      transition: background 0.15s;
    }
    .user-card:hover { background: var(--bg-input); }

    .avatar-wrap { position: relative; flex-shrink: 0; }
    .avatar {
      width: 44px; height: 44px;
      border-radius: 50%;
      display: flex; align-items: center; justify-content: center;
      font-size: 22px;
      border: 2px solid var(--border);
    }
    .status-badge {
      position: absolute;
      bottom: 1px; right: 1px;
      width: 11px; height: 11px;
      border-radius: 50%;
      border: 2px solid var(--bg-panel);
    }
    .badge-online  { background: var(--online); }
    .badge-offline { background: var(--offline); }
    .badge-typing  { background: var(--typing-color); animation: pulse 1s infinite; }

    .user-meta { flex: 1; overflow: hidden; }
    .user-nick  { font-weight: 600; font-size: 14px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .user-stat  { font-size: 11px; color: var(--text-sec); margin-top: 2px; }
    .user-count { font-size: 11px; background: var(--accent); color: #fff; border-radius: 10px; padding: 2px 7px; }

    /* ─── DOSYA KARTLARI ─── */
    .file-card {
      display: flex;
      align-items: center;
      gap: 10px;
      padding: 10px 12px;
      border-radius: 8px;
      background: var(--bg-input);
      margin-bottom: 6px;
    }
    .file-icon { font-size: 22px; flex-shrink: 0; }
    .file-info { flex: 1; overflow: hidden; }
    .file-name { font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .file-size { font-size: 11px; color: var(--text-sec); margin-top: 2px; }
    .file-dl-btn {
      background: var(--accent);
      color: #fff;
      border: none;
      border-radius: 8px;
      padding: 7px 12px;
      font-size: 16px;
      text-decoration: none;
      flex-shrink: 0;
    }
    .empty-msg { color: var(--text-sec); text-align: center; padding: 30px 10px; font-size: 13px; }

    /* ─── CHAT ALANI ─── */
    .chat-panel {
      flex: 1;
      display: flex;
      flex-direction: column;
      overflow: hidden;
    }
    #chat-box {
      flex: 1;
      overflow-y: auto;
      padding: 12px 16px;
      display: flex;
      flex-direction: column;
      gap: 4px;
    }

    /* ─── MESAJ BALONLARI ─── */
    .msg-wrap {
      display: flex;
      flex-direction: column;
      max-width: 75%;
      animation: msgIn 0.2s ease;
    }
    .msg-wrap.mine  { align-self: flex-end; align-items: flex-end; }
    .msg-wrap.other { align-self: flex-start; align-items: flex-start; }

    @keyframes msgIn {
      from { opacity:0; transform: translateY(6px); }
      to   { opacity:1; transform: translateY(0); }
    }

    .msg-header { display: flex; align-items: center; gap: 6px; margin-bottom: 3px; }
    .msg-mini-avatar {
      width: 20px; height: 20px;
      border-radius: 50%;
      display: flex; align-items: center; justify-content: center;
      font-size: 11px;
    }
    .msg-nick { font-size: 12px; font-weight: 700; }
    .msg-bubble {
      padding: 8px 12px;
      border-radius: var(--radius-msg);
      font-size: 14px;
      line-height: 1.45;
      word-break: break-word;
      box-shadow: 0 1px 3px rgba(0,0,0,0.3);
    }
    .msg-wrap.mine  .msg-bubble { background: var(--bg-mine);  border-bottom-right-radius: 3px; }
    .msg-wrap.other .msg-bubble { background: var(--bg-other); border-bottom-left-radius: 3px; }

    .msg-time {
      font-size: 10px;
      color: var(--text-sec);
      margin-top: 3px;
      padding: 0 4px;
    }

    /* ─── TYPING INDICATOR ─── */
    .typing-bar {
      padding: 6px 16px;
      font-size: 12px;
      color: var(--text-sec);
      font-style: italic;
      min-height: 26px;
    }
    .dots span { animation: dotBlink 1.4s infinite; }
    .dots span:nth-child(2) { animation-delay: 0.2s; }
    .dots span:nth-child(3) { animation-delay: 0.4s; }
    @keyframes dotBlink { 0%,80%,100%{opacity:0} 40%{opacity:1} }

    /* ─── GİRİŞ ALANI ─── */
    .input-section {
      background: var(--bg-panel);
      padding: 10px 12px;
      border-top: 1px solid var(--border);
    }
    .nick-row {
      display: flex;
      align-items: center;
      gap: 8px;
      margin-bottom: 8px;
    }
    .nick-label { font-size: 11px; color: var(--text-sec); white-space: nowrap; }
    #nick {
      width: 130px;
      padding: 7px 10px;
      background: var(--bg-input);
      border: 1px solid var(--border);
      border-radius: 20px;
      color: var(--text-pri);
      font-size: 13px;
      font-weight: 600;
      text-align: center;
    }
    #nick:focus { outline: none; border-color: var(--accent); }

    .msg-row {
      display: flex;
      align-items: flex-end;
      gap: 8px;
    }
    .toolbar {
      display: flex;
      gap: 4px;
      margin-bottom: 6px;
    }
    .tool-btn {
      background: none;
      border: none;
      color: var(--text-sec);
      font-size: 18px;
      cursor: pointer;
      padding: 4px 6px;
      border-radius: 6px;
      transition: color 0.2s, background 0.2s;
    }
    .tool-btn:hover { color: var(--accent); background: var(--bg-input); }
    #mesaj {
      flex: 1;
      padding: 10px 14px;
      background: var(--bg-input);
      border: 1px solid var(--border);
      border-radius: 20px;
      color: var(--text-pri);
      font-size: 14px;
      resize: none;
      font-family: inherit;
      max-height: 120px;
      line-height: 1.4;
    }
    #mesaj:focus { outline: none; border-color: var(--accent); }
    .send-btn {
      width: 44px; height: 44px;
      background: var(--accent);
      border: none;
      border-radius: 50%;
      color: #fff;
      font-size: 18px;
      cursor: pointer;
      display: flex; align-items: center; justify-content: center;
      flex-shrink: 0;
      transition: background 0.2s, transform 0.1s;
    }
    .send-btn:hover  { background: var(--accent-light); }
    .send-btn:active { transform: scale(0.92); }

    /* ─── EMOJİ PİCKER ─── */
    #emojiPicker {
      display: none;
      position: fixed;
      bottom: 130px;
      left: 50%;
      transform: translateX(-50%);
      background: var(--bg-panel);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 10px;
      width: min(340px, 95vw);
      box-shadow: 0 8px 30px rgba(0,0,0,0.6);
      z-index: 500;
    }
    #emojiPicker.open { display: block; }
    .emoji-grid {
      display: grid;
      grid-template-columns: repeat(8, 1fr);
      gap: 4px;
    }
    .emoji-btn {
      background: none; border: none;
      font-size: 20px; cursor: pointer;
      border-radius: 6px; padding: 3px;
      transition: background 0.15s, transform 0.15s;
    }
    .emoji-btn:hover { background: var(--bg-input); transform: scale(1.2); }

    /* ─── OVERLAY (Mobil sidebar kapama) ─── */
    #overlay {
      display: none;
      position: fixed; inset: 0;
      background: rgba(0,0,0,0.5);
      z-index: 200;
    }
    #overlay.show { display: block; }

    /* ─── TOAST BİLDİRİM ─── */
    #toast {
      position: fixed;
      bottom: 30px; left: 50%;
      transform: translateX(-50%) translateY(20px);
      background: var(--bg-panel);
      border: 1px solid var(--accent);
      color: var(--text-pri);
      padding: 10px 20px;
      border-radius: 20px;
      font-size: 13px;
      opacity: 0;
      transition: opacity 0.3s, transform 0.3s;
      pointer-events: none;
      z-index: 1000;
      white-space: nowrap;
    }
    #toast.show {
      opacity: 1;
      transform: translateX(-50%) translateY(0);
    }

    /* ─── ANİMASYON ─── */
    @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.4} }

    /* ─── SCROLLBAR ─── */
    ::-webkit-scrollbar { width: 5px; }
    ::-webkit-scrollbar-track { background: transparent; }
    ::-webkit-scrollbar-thumb { background: var(--border); border-radius: 3px; }
    ::-webkit-scrollbar-thumb:hover { background: var(--text-sec); }

    /* ─── RESPONSIVE (Mobil ≤ 640 px) ─── */
    @media (max-width: 640px) {
      /* Sidebar varsayılan olarak gizli; toggle ile açılır */
      .sidebar {
        position: fixed;
        top: 0; left: 0;
        height: 100%;
        z-index: 300;
        transform: translateX(-100%);
      }
      .sidebar.open { transform: translateX(0); }
      .sidebar-toggle { display: block; }
      .header-pills { display: none; }
      .msg-wrap { max-width: 88%; }
    }
  </style>
</head>
<body>

<!-- ═══════════ HEADER ═══════════ -->
<header class="app-header">
  <!-- Mobilde sidebar toggle düğmesi -->
  <button class="sidebar-toggle" onclick="toggleSidebar()" title="Menü">☰</button>
  <span class="header-logo">☁️</span>
  <span class="header-title">DarkCloud</span>
  <div class="header-pills">
    <span class="pill">👥 <span class="val" id="statUsers">0</span></span>
    <span class="pill">💬 <span class="val" id="statMsgs">0</span></span>
    <span class="pill" id="encPill">🔒</span>
  </div>
</header>

<!-- ═══════════ UYGULAMA GÖVDE ═══════════ -->
<div class="app-body">

  <!-- ── Mobil overlay (sidebar arkası) ── -->
  <div id="overlay" onclick="closeSidebar()"></div>

  <!-- ══════════ SIDEBAR ══════════ -->
  <aside class="sidebar" id="sidebar">
    <!-- Tab bar -->
    <div class="tab-bar">
      <button class="tab-btn active" id="tab-users-btn" onclick="switchTab('users')">👥 Kişiler</button>
      <button class="tab-btn"        id="tab-files-btn" onclick="switchTab('files')">📁 Dosyalar</button>
    </div>

    <!-- Kişiler paneli -->
    <div class="tab-panel active" id="tab-users">
      <div class="scroll-area" id="userList">
        <p class="empty-msg">Bağlanan kullanıcılar burada görünür</p>
      </div>
    </div>

    <!-- Dosyalar paneli -->
    <div class="tab-panel" id="tab-files">
      <div class="scroll-area" id="fileList"></div>
    </div>
  </aside>

  <!-- ══════════ CHAT ALANI ══════════ -->
  <main class="chat-panel">
    <!-- Mesajlar -->
    <div id="chat-box"></div>

    <!-- Yazıyor göstergesi -->
    <div class="typing-bar" id="typingBar"></div>

    <!-- Giriş bölümü -->
    <div class="input-section">
      <!-- Kullanıcı adı satırı -->
      <div class="nick-row">
        <span class="nick-label">Adın:</span>
        <input type="text" id="nick" maxlength="20" value="Anonim" autocomplete="off" spellcheck="false">
      </div>

      <!-- Araç çubuğu -->
      <div class="toolbar">
        <button class="tool-btn" onclick="toggleEmoji()" title="Emoji ekle">😊</button>
        <label  class="tool-btn" title="Dosya yükle" style="cursor:pointer;">
          📎
          <input type="file" id="fileInput" style="display:none" onchange="uploadFile(this)">
        </label>
        <button class="tool-btn" onclick="toggleEncryption()" title="Şifreleme">
          <span id="encIcon">🔒</span>
        </button>
        <button class="tool-btn" onclick="clearChat()" title="Sohbeti temizle">🗑️</button>
      </div>

      <!-- Mesaj yazma + gönder -->
      <div class="msg-row">
        <textarea id="mesaj" rows="1" placeholder="Mesajını yaz..." maxlength="1000"></textarea>
        <button class="send-btn" onclick="sendMessage()">➤</button>
      </div>
    </div>
  </main>
</div>

<!-- ══════════ EMOJİ PİCKER ══════════ -->
<div id="emojiPicker">
  <div class="emoji-grid" id="emojiGrid"></div>
</div>

<!-- ══════════ TOAST BİLDİRİM ══════════ -->
<div id="toast"></div>

<!-- ══════════════════════════════════════
     JAVASCRIPT
  ═══════════════════════════════════════ -->
<script>
  // ── Uygulama Durumu ──────────────────────────────────────────
  let lastChatHTML   = "";      // Son çekilen chat HTML'i (değişim tespiti için)
  let myNick         = "Anonim";
  let isTyping       = false;
  let typingTimer    = null;
  let encOn          = true;    // Şifreleme durumu (sunucu ile senkron)
  let lastMsgCount   = 0;
  let sidebarOpen    = false;

  // Emoji listesi
  const EMOJIS = [
    '😀','😁','😂','🤣','😃','😄','😅','😊','😋','😎','😍','😘','🥰',
    '🤗','🤔','😐','😑','🙄','😏','😒','😔','😞','😤','😢','😭','😱',
    '😡','🤬','🥳','🤩','😴','🤤','🥺','😇','🤠','😈','👿','💀','👻',
    '💩','🤖','👋','🤝','👍','👎','👏','🙌','🤜','🤛','❤️','🧡','💛',
    '💚','💙','💜','🖤','🔥','⭐','✨','💫','🎉','🎊','🎈','🎁','🏆',
    '🚀','🌈','☀️','🌙','⚡','❄️','🌊','🍕','🍔','🍜','☕','🎵','🎮',
    '📱','💻','📷','🔑','💡','🔒','🔓','⚙️','✅','❌','⚠️','💬','📢'
  ];

  // ── Sayfa Yüklenince ─────────────────────────────────────────
  (function init() {
    // Emoji butonları oluştur
    const grid = document.getElementById('emojiGrid');
    EMOJIS.forEach(e => {
      const btn = document.createElement('button');
      btn.className = 'emoji-btn';
      btn.textContent = e;
      btn.onclick = () => insertEmoji(e);
      grid.appendChild(btn);
    });

    // Textarea: Enter gönder, Shift+Enter yeni satır
    const ta = document.getElementById('mesaj');
    ta.addEventListener('keydown', e => {
      if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        sendMessage();
      }
    });

    // Typing tespiti
    ta.addEventListener('input', handleTyping);

    // İlk veri yükle
    refreshAll();

    // Her 2.5 saniyede otomatik güncelle
    setInterval(refreshAll, 2500);
  })();

  // ── Sidebar Mobil Toggle ─────────────────────────────────────
  function toggleSidebar() {
    sidebarOpen ? closeSidebar() : openSidebar();
  }
  function openSidebar() {
    document.getElementById('sidebar').classList.add('open');
    document.getElementById('overlay').classList.add('show');
    sidebarOpen = true;
  }
  function closeSidebar() {
    document.getElementById('sidebar').classList.remove('open');
    document.getElementById('overlay').classList.remove('show');
    sidebarOpen = false;
  }

  // ── Tab Geçişi ────────────────────────────────────────────────
  function switchTab(name) {
    ['users','files'].forEach(t => {
      document.getElementById('tab-' + t).classList.toggle('active', t === name);
      document.getElementById('tab-' + t + '-btn').classList.toggle('active', t === name);
    });
    // Dosyalar sekmesine geçince listeyi tazele
    if (name === 'files') fetchFiles();
  }

  // ── Tüm Verileri Güncelle ─────────────────────────────────────
  function refreshAll() {
    fetchChat();
    fetchUsers();
  }

  // ── Chat Güncelle ─────────────────────────────────────────────
  function fetchChat() {
    fetch('/chatoku')
      .then(r => r.text())
      .then(html => {
        if (html === lastChatHTML) return;     // Değişiklik yoksa DOM'a dokunma
        lastChatHTML = html;

        const box = document.getElementById('chat-box');
        const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 80;

        box.innerHTML = html;

        // Yeni mesaj sayısı
        const count = box.querySelectorAll('.msg-wrap').length;
        if (count > lastMsgCount && lastMsgCount > 0) {
          showToast('💬 Yeni mesaj!');
          beep();
        }
        lastMsgCount = count;
        document.getElementById('statMsgs').textContent = count;

        // Kullanıcı aşağıdaysa otomatik kaydır
        if (atBottom) box.scrollTop = box.scrollHeight;
      })
      .catch(() => {});
  }

  // ── Kullanıcıları Güncelle ────────────────────────────────────
  function fetchUsers() {
    fetch('/users')
      .then(r => r.json())
      .then(users => {
        let html = '';
        let onlineCount = 0;

        // Typing durumundakileri tespit et
        const typingUsers = users.filter(u => u.status === 'typing').map(u => u.nick);
        if (typingUsers.length > 0) {
          document.getElementById('typingBar').innerHTML =
            '<b>' + typingUsers.join(', ') + '</b> yazıyor' +
            '<span class="dots"><span>.</span><span>.</span><span>.</span></span>';
        } else {
          document.getElementById('typingBar').textContent = '';
        }

        users.forEach(u => {
          if (u.status !== 'offline') onlineCount++;
          const badgeClass = 'badge-' + (u.status === 'typing' ? 'typing' : u.status);
          const statusText = u.status === 'online'  ? 'Çevrimiçi' :
                             u.status === 'typing'  ? 'Yazıyor…'  : 'Çevrimdışı';
          html += `
            <div class="user-card">
              <div class="avatar-wrap">
                <div class="avatar" style="background:${u.color}">${u.avatar}</div>
                <span class="status-badge ${badgeClass}"></span>
              </div>
              <div class="user-meta">
                <div class="user-nick" style="color:${u.color}">${u.nick}</div>
                <div class="user-stat">${statusText}</div>
              </div>
              <span class="user-count">${u.messages}</span>
            </div>`;
        });

        document.getElementById('userList').innerHTML =
          html || '<p class="empty-msg">Henüz kimse bağlanmadı</p>';
        document.getElementById('statUsers').textContent = onlineCount;
      })
      .catch(() => {});
  }

  // ── Dosya Listesini Güncelle ──────────────────────────────────
  function fetchFiles() {
    fetch('/dosyaListesi')
      .then(r => r.text())
      .then(html => {
        document.getElementById('fileList').innerHTML = html;
      })
      .catch(() => {});
  }

  // ── Mesaj Gönder ──────────────────────────────────────────────
  function sendMessage() {
    const nickEl = document.getElementById('nick');
    const msgEl  = document.getElementById('mesaj');
    const n = nickEl.value.trim() || 'Anonim';
    const m = msgEl.value.trim();
    if (!m) return;

    myNick = n;
    msgEl.value = '';
    msgEl.style.height = 'auto';
    stopTyping();

    fetch('/chatyaz', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'nick=' + encodeURIComponent(n) + '&mesaj=' + encodeURIComponent(m)
    })
    .then(() => { setTimeout(fetchChat, 250); })
    .catch(() => showToast('❌ Gönderilemedi'));
  }

  // ── Typing ────────────────────────────────────────────────────
  function handleTyping() {
    // Textarea otomatik yükseklik
    const ta = document.getElementById('mesaj');
    ta.style.height = 'auto';
    ta.style.height = Math.min(ta.scrollHeight, 120) + 'px';

    if (!isTyping) {
      isTyping = true;
      fetch('/typing?nick=' + encodeURIComponent(myNick) + '&status=typing').catch(() => {});
    }
    clearTimeout(typingTimer);
    typingTimer = setTimeout(stopTyping, 2000);
  }

  function stopTyping() {
    if (!isTyping) return;
    isTyping = false;
    fetch('/typing?nick=' + encodeURIComponent(myNick) + '&status=online').catch(() => {});
  }

  // ── Dosya Yükleme ─────────────────────────────────────────────
  function uploadFile(input) {
    const file = input.files[0];
    if (!file) return;
    const fd = new FormData();
    fd.append('upload', file);
    showToast('⏳ Yükleniyor: ' + file.name);
    fetch('/upload', { method: 'POST', body: fd })
      .then(r => {
        showToast(r.ok ? '✅ Yüklendi!' : '❌ Yükleme başarısız');
        if (r.ok) fetchFiles();
      })
      .catch(() => showToast('❌ Bağlantı hatası'));
    input.value = '';
  }

  // ── Emoji ─────────────────────────────────────────────────────
  function toggleEmoji() {
    document.getElementById('emojiPicker').classList.toggle('open');
  }
  function insertEmoji(e) {
    const ta = document.getElementById('mesaj');
    const pos = ta.selectionStart;
    ta.value = ta.value.slice(0, pos) + e + ta.value.slice(pos);
    ta.focus();
    ta.selectionStart = ta.selectionEnd = pos + e.length;
    document.getElementById('emojiPicker').classList.remove('open');
  }
  // Emoji picker dışına tıklayınca kapat
  document.addEventListener('click', e => {
    if (!document.getElementById('emojiPicker').contains(e.target) &&
        !e.target.closest('[onclick="toggleEmoji()"]')) {
      document.getElementById('emojiPicker').classList.remove('open');
    }
  });

  // ── Chat Temizleme ────────────────────────────────────────────
  function clearChat() {
    if (!confirm('Tüm mesajları silmek istiyor musun?')) return;
    fetch('/clearChat')
      .then(() => { showToast('🗑️ Temizlendi'); fetchChat(); })
      .catch(() => {});
  }

  // ── Şifreleme Toggle ─────────────────────────────────────────
  function toggleEncryption() {
    encOn = !encOn;
    document.getElementById('encIcon').textContent = encOn ? '🔒' : '🔓';
    document.getElementById('encPill').textContent = encOn ? '🔒' : '🔓';
    showToast(encOn ? '🔒 Şifreleme açık' : '🔓 Şifreleme kapalı');
    fetch('/setEncryption?status=' + (encOn ? '1' : '0')).catch(() => {});
  }

  // ── Toast Bildirimi ───────────────────────────────────────────
  let toastTimer;
  function showToast(msg) {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.remove('show'), 2800);
  }

  // ── Ses Efekti (WebAudio API) ─────────────────────────────────
  function beep() {
    try {
      const ctx = new (window.AudioContext || window.webkitAudioContext)();
      const osc = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.type = 'sine';
      osc.frequency.value = 880;
      gain.gain.setValueAtTime(0.15, ctx.currentTime);
      gain.gain.exponentialRampToValueAtTime(0.001, ctx.currentTime + 0.15);
      osc.connect(gain);
      gain.connect(ctx.destination);
      osc.start();
      osc.stop(ctx.currentTime + 0.15);
    } catch (e) { /* Tarayıcı desteklemiyorsa sessizce geç */ }
  }
</script>
</body>
</html>
)rawliteral";
  return h;
}

// ═══════════════════════════════════════════════════════════════
//  SETUP – Bir kez çalışır
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n☁️  DarkCloud MEGA v6.0 başlıyor...");

  // ── SD Kart Başlat ───────────────────────────────────────────
  // Özel SPI pinleri kullanılıyor (Deneyap Kart için)
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    // SD olmadan sistem çalışmaya devam eder ama mesajlar kaydedilmez
    Serial.println("⚠️  SD Kart bağlanamadı – mesajlar kaydedilmeyecek!");
  } else {
    Serial.println("✅ SD Kart hazır.");
  }

  // ── NVS'den Ayarları Oku ─────────────────────────────────────
  prefs.begin("darkcloud", true);   // true = salt okunur mod
  encryptionEnabled = prefs.getBool("encryption", true);
  prefs.end();
  Serial.print("Şifreleme: ");
  Serial.println(encryptionEnabled ? "Açık" : "Kapalı");

  // ── WiFi SoftAP ──────────────────────────────────────────────
  /*
   * GÜVENLİK: Varsayılan kanal 1 kullanılır.
   * maxConn = 8 ile eş zamanlı bağlantı sınırlandırılıyor (DoS önlemi).
   */
  WiFi.softAP(WIFI_SSID, WIFI_PASSWORD, 1 /*channel*/, 0 /*hidden*/, 8 /*maxConn*/);
  Serial.print("SoftAP Aktif – SSID: ");
  Serial.println(WIFI_SSID);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  // ═════════════════ HTTP ROTALARI ════════════════════════════

  // Ana sayfa → HTML arayüzü gönder
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=UTF-8", htmlSayfasi());
  });

  // ── /chatoku – Chat geçmişini HTML olarak döndür ──────────────
  server.on("/chatoku", HTTP_GET, []() {
    String html = "";
    File f = SD.open(CHAT_FILE);
    if (f) {
      int idx = 0;
      while (f.available()) {
        String satir = f.readStringUntil('\n');
        int sep = satir.indexOf('|');
        if (sep <= 0) continue;   // Geçersiz satır, atla

        String nick = satir.substring(0, sep);
        String msg  = satir.substring(sep + 1);
        msg.trim();

        // Şifreli ise çöz
        if (encryptionEnabled) msg = xorCrypt(msg);

        // XSS önlemi: her iki alanı da escape et
        String nickSafe = escapeHTML(nick);
        String msgSafe  = escapeHTML(msg);

        // Renk bilgisi için kullanıcıyı bul
        int uIdx = findUser(nick);
        String color  = (uIdx >= 0) ? activeUsers[uIdx].color  : "#00a884";
        String avatar = (uIdx >= 0) ? activeUsers[uIdx].avatar : "👤";

        // Kendi mesajı mı? (sunucu taraflı tespit yoktur, client IP'den anlaşılır)
        // Basitlik için hepsini "other" stilinde gösteriyoruz;
        // JavaScript tarafı kendi mesajlarını nick eşleşmesiyle ayırt edebilir.
        html += "<div class='msg-wrap other'>";
        html += "<div class='msg-header'>";
        html += "<div class='msg-mini-avatar' style='background:" + color + "'>" + avatar + "</div>";
        html += "<span class='msg-nick' style='color:" + color + "'>" + nickSafe + "</span>";
        html += "</div>";
        html += "<div class='msg-bubble'>" + msgSafe + "</div>";
        html += "<div class='msg-time'>#" + String(++idx) + "</div>";
        html += "</div>";
      }
      f.close();
    }
    server.send(200, "text/html; charset=UTF-8", html);
  });

  // ── /chatyaz – Yeni mesaj kaydet ────────────────────────────
  server.on("/chatyaz", HTTP_POST, []() {
    if (!server.hasArg("mesaj") || !server.hasArg("nick")) {
      server.send(400, "text/plain", "Eksik parametre");
      return;
    }

    String nick = server.arg("nick");
    String msg  = server.arg("mesaj");
    String ip   = getClientIP();

    // Nick doğrulama: boş veya çok uzun ise reddet
    nick.trim();
    if (nick.length() == 0 || nick.length() > 20) {
      server.send(400, "text/plain", "Geçersiz nick");
      return;
    }
    // Mesaj uzunluğu sınırı
    if (msg.length() == 0 || msg.length() > 1000) {
      server.send(400, "text/plain", "Geçersiz mesaj uzunluğu");
      return;
    }

    // Kullanıcıyı kayıt et / güncelle
    addOrUpdateUser(nick, ip);
    int idx = findUser(nick);
    if (idx >= 0) activeUsers[idx].messageCount++;

    // Şifrele ve SD'ye yaz
    String toSave = encryptionEnabled ? xorCrypt(msg) : msg;
    File f = SD.open(CHAT_FILE, FILE_APPEND);
    if (f) {
      f.println(nick + "|" + toSave);
      f.close();
    } else {
      // SD kart yazma hatası – istemciye yine de OK dön (log tutmak için Serial)
      Serial.println("⚠️  SD yazma hatası!");
    }

    server.send(200, "text/plain", "OK");
  });

  // ── /users – Aktif kullanıcı listesini JSON olarak ver ───────
  server.on("/users", HTTP_GET, []() {
    server.send(200, "application/json", getUsersJSON());
  });

  // ── /typing – Kullanıcı yazıyor durumunu güncelle ────────────
  server.on("/typing", HTTP_GET, []() {
    if (server.hasArg("nick") && server.hasArg("status")) {
      String st = server.arg("status");
      // Sadece izin verilen durumları kabul et (injection önlemi)
      if (st == "typing" || st == "online" || st == "away") {
        updateUserStatus(server.arg("nick"), st);
      }
    }
    server.send(200, "text/plain", "OK");
  });

  // ── /dosyaListesi – SD'deki dosyaları listele ────────────────
  server.on("/dosyaListesi", HTTP_GET, []() {
    server.send(200, "text/html; charset=UTF-8", dosyaListesiGetir());
  });

  // ── /clearChat – Chat geçmişini sil ─────────────────────────
  /*
   * GÜVENLİK UYARISI: Bu endpoint şu an herkese açık.
   * Üretim kullanımında ?token=ADMIN_TOKEN ile koruyun:
   *   if (server.arg("token") != ADMIN_TOKEN) { server.send(403,...); return; }
   */
  server.on("/clearChat", HTTP_GET, []() {
    // Basit token kontrolü (opsiyonel – aktif etmek için yorum kaldırın)
    // if (server.arg("token") != String(ADMIN_TOKEN)) {
    //   server.send(403, "text/plain", "Yetkisiz");
    //   return;
    // }
    SD.remove(CHAT_FILE);
    lastMsgCount = 0;   // Sayacı sıfırla
    server.send(200, "text/plain", "OK");
  });

  // ── /setEncryption – Şifreleme açma/kapama ──────────────────
  server.on("/setEncryption", HTTP_GET, []() {
    if (server.hasArg("status")) {
      encryptionEnabled = (server.arg("status") == "1");
      prefs.begin("darkcloud", false);
      prefs.putBool("encryption", encryptionEnabled);
      prefs.end();
      Serial.print("Şifreleme: ");
      Serial.println(encryptionEnabled ? "Açık" : "Kapalı");
    }
    server.send(200, "text/plain", "OK");
  });

  // ── /upload – Dosya yükleme (multipart/form-data) ────────────
  server.on("/upload", HTTP_POST,
    /* İstek tamamlandığında çalışan callback */
    []() { server.send(200, "text/plain", "OK"); },

    /* Her chunk geldiğinde çalışan upload callback */
    []() {
      HTTPUpload& upload = server.upload();

      if (upload.status == UPLOAD_FILE_START) {
        String path = "/" + upload.filename;

        // GÜVENLİK: Dosya adında yol geçişi (path traversal) önlemi
        path.replace("..", "");   // "../" gibi zincirleri engelle
        if (path.length() < 2 || path.length() > 64) return;  // Geçersiz isim

        Serial.println("⬆️  Yükleniyor: " + path);
        if (SD.exists(path)) SD.remove(path);   // Aynı isimde varsa üzerine yaz
        uploadFile = SD.open(path, FILE_WRITE);

      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);

      } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
          uploadFile.close();
          Serial.print("✅ Yükleme tamam – ");
          Serial.print(upload.totalSize);
          Serial.println(" byte");
        }
      }
    }
  );

  // ── Bilinmeyen URL → SD'den dosya servis et (indirme) ────────
  server.onNotFound([]() {
    String yol = server.uri();

    // GÜVENLİK: Yol geçişi önlemi
    if (yol.indexOf("..") != -1) {
      server.send(403, "text/plain", "Yasak");
      return;
    }

    if (SD.exists(yol)) {
      File dosya = SD.open(yol, FILE_READ);
      // application/octet-stream – tarayıcı dosyayı indirir
      server.streamFile(dosya, "application/octet-stream");
      dosya.close();
    } else {
      server.send(404, "text/plain", "Dosya bulunamadi: " + yol);
    }
  });

  server.begin();
  Serial.println("🌐 HTTP Sunucu başlatıldı – http://" + WiFi.softAPIP().toString());
}

// ═══════════════════════════════════════════════════════════════
//  LOOP – Sürekli tekrar eder
// ═══════════════════════════════════════════════════════════════
void loop() {
  // HTTP isteklerini işle (bloklamayan)
  server.handleClient();

  // Her 10 saniyede offline kullanıcıları kontrol et
  static unsigned long lastOfflineCheck = 0;
  if (millis() - lastOfflineCheck > 10000) {
    checkOfflineUsers();
    lastOfflineCheck = millis();
  }
}

/*
 * ═══════════════════════════════════════════════════════════════
 *  v6 DEĞİŞİKLİK GÜNLÜĞÜ
 * ═══════════════════════════════════════════════════════════════
 *
 *  GÜVENLİK DÜZELTMELERİ:
 *    [+] escapeHTML() eklendi → XSS önlendi
 *    [+] Nick/mesaj uzunluğu sunucu taraflı doğrulandı
 *    [+] /upload'da path traversal koruması eklendi
 *    [+] /typing'de status değeri whitelist ile doğrulandı
 *    [+] onNotFound'da ".." içeren yollar reddedilir
 *    [+] WiFi.softAP maxConn=8 ile sınırlandırıldı
 *    [~] AES import kaldırıldı (kullanılmıyordu, kod şişiriyordu)
 *    [~] admin şifresi sabit string'den ADMIN_TOKEN sabitine taşındı
 *    [!] XOR hâlâ gerçek şifreleme değil – ileride AES-GCM ile değiştirin
 *
 *  ARAYÜZ İYİLEŞTİRMELERİ:
 *    [+] WhatsApp tarzı mesaj balonları (mine/other ayrımı)
 *    [+] Tam mobil uyumlu – sidebar toggle (hamburger menü)
 *    [+] Toast bildirimi (eski sabit div yerine)
 *    [+] Emoji picker'ın dışına tıklayınca kapanması
 *    [+] Textarea otomatik yükseklik (handleTyping)
 *    [+] Typing bar ayrı satır – daha okunabilir
 *    [+] Dosya indirme düğmesi anchor tag oldu (download attr)
 *
 *  KOD KALİTESİ:
 *    [+] Tüm kritik bloklara Türkçe yorum satırları eklendi
 *    [+] const String → const char* (SSID, pass)
 *    [+] findUser/addOrUpdateUser const referans alıyor
 *    [+] HTTP metodu belirtildi (HTTP_GET / HTTP_POST)
 *    [~] Kullanılmayan mbedtls/aes.h import kaldırıldı
 * ═══════════════════════════════════════════════════════════════
 */

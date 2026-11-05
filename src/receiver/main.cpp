// ============================================================
//  受信機：M5Stack Core2 / CoreS3（受付に設置）
//
//  ・ボタンからの非常信号を受けて、画面を赤く点滅させ警報音を鳴らす
//  ・LINE WORKS の Incoming Webhook でトークルームに通知する
//  ・画面タップで「対応中」、2秒長押しで「解除」
//  ・各ボタンの生存確認を監視し、途絶えたら知らせる
//  ・Bボタン（画面下中央）を2秒長押しで LINE WORKS にテスト送信
// ============================================================
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <ArduinoJson.h>
#include <time.h>

#include "config.h"
#include "protocol.h"

namespace {

constexpr int NUM_ROOMS = static_cast<int>(sizeof(ROOM_NAMES) / sizeof(ROOM_NAMES[0]));
static_assert(NUM_ROOMS <= proto::MAX_ROOMS, "ROOM_NAMES は最大16部屋までです");

const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---------- 部屋ごとの状態 ----------
struct Room {
  bool     seen      = false;  // 起動後に一度でも通信があったか
  uint32_t lastSeen  = 0;
  bool     offline   = false;  // 「応答なし」通知済み
  bool     alarm     = false;  // 非常事態中
  bool     handling  = false;  // 受付で「対応中」を押した
  bool     hasSeq    = false;
  uint32_t alarmSeq  = 0;
  uint32_t alarmAt   = 0;
};
Room g_rooms[proto::MAX_ROOMS];

String roomName(uint8_t id) {
  if (id < NUM_ROOMS) return ROOM_NAMES[id];
  return String("部屋") + id;
}

// ---------- 受信キュー（ESP-NOW コールバック → loop） ----------
QueueHandle_t g_rxQueue;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
#else
void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  if (len != sizeof(proto::Packet)) return;
  proto::Packet p;
  memcpy(&p, data, sizeof(p));
  xQueueSend(g_rxQueue, &p, 0);
}

void sendAck(const proto::Packet& in) {
  proto::Packet ack = proto::make(proto::ACK, in.room, in.seq, CLINIC_NET_ID, in.type);
  esp_now_send(BCAST, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
}

// ---------- 時刻 ----------
String nowStr() {
  struct tm tm;
  if (!getLocalTime(&tm, 0)) return "時刻未取得";
  char buf[24];
  strftime(buf, sizeof(buf), "%m/%d %H:%M:%S", &tm);
  return buf;
}

// ---------- LINE WORKS 通知（別タスクで送信。送信中も警報は止まらない） ----------
struct NotifyMsg {
  char text[480];
};
QueueHandle_t g_notifyQueue;

void notify(const String& text) {
  Serial.println("[notify] " + text);
  if (strlen(LINEWORKS_WEBHOOK_URL) == 0) return;
  NotifyMsg m;
  strlcpy(m.text, text.c_str(), sizeof(m.text));
  if (xQueueSend(g_notifyQueue, &m, 0) != pdTRUE) {
    Serial.println("[notify] 送信待ちがいっぱいです");
  }
}

bool postLineWorks(const char* text) {
  WiFiClientSecure client;
  if (strlen(LINEWORKS_ROOT_CA) > 0) {
    client.setCACert(LINEWORKS_ROOT_CA);
  } else {
    client.setInsecure();
  }
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, LINEWORKS_WEBHOOK_URL)) return false;
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["body"]["text"] = text;
  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  if (code < 200 || code >= 300) {
    Serial.printf("[notify] 失敗 HTTP %d %s\n", code, http.getString().c_str());
  }
  http.end();
  return code >= 200 && code < 300;
}

void notifyTask(void*) {
  NotifyMsg m;
  for (;;) {
    if (xQueueReceive(g_notifyQueue, &m, portMAX_DELAY) != pdTRUE) continue;
    uint32_t backoff = 1000;
    for (int attempt = 0; attempt < 20; attempt++) {
      if (WiFi.status() == WL_CONNECTED && postLineWorks(m.text)) {
        Serial.println("[notify] LINE WORKS に送信しました");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(backoff));
      backoff = (backoff * 2 > 15000) ? 15000 : backoff * 2;
    }
  }
}

// ---------- 受信処理 ----------
void handlePacket(const proto::Packet& p) {
  if (!proto::valid(p, CLINIC_NET_ID)) return;
  if (p.type != proto::ALARM && p.type != proto::HEARTBEAT) return;
  sendAck(p);  // まず受領確認を返す（送信機の再送を止める）

  Room& r = g_rooms[p.room];
  r.lastSeen = millis();
  r.seen     = true;
  if (r.offline) {
    r.offline = false;
    notify("ℹ️ " + roomName(p.room) + " の非常ボタンとの通信が復旧しました（" + nowStr() + "）");
  }

  if (p.type == proto::ALARM) {
    if (r.hasSeq && r.alarmSeq == p.seq) return;  // 同じ発報の再送
    bool again = r.alarm;
    r.hasSeq   = true;
    r.alarmSeq = p.seq;
    r.alarm    = true;
    r.handling = false;
    if (!again) r.alarmAt = millis();
    notify(String("🚨【非常ボタン】") + roomName(p.room) + " で非常ボタンが" +
           (again ? "再度" : "") + "押されました。至急向かってください。（" + nowStr() + "）");
  }
}

void checkOffline() {
  uint32_t now = millis();
  for (int i = 0; i < NUM_ROOMS; i++) {
    Room& r = g_rooms[i];
    if (r.offline) continue;
    bool timedOut = r.seen ? (now - r.lastSeen > OFFLINE_TIMEOUT_MS) : (now > OFFLINE_TIMEOUT_MS);
    if (timedOut) {
      r.offline = true;
      notify("⚠️ " + roomName(i) + " の非常ボタンから応答がありません。電源とケーブルを確認してください（" +
             nowStr() + "）");
    }
  }
}

// ---------- 操作 ----------
bool anyAlarm(bool unhandledOnly) {
  for (int i = 0; i < proto::MAX_ROOMS; i++) {
    if (g_rooms[i].alarm && (!unhandledOnly || !g_rooms[i].handling)) return true;
  }
  return false;
}

void acknowledgeAll() {
  for (int i = 0; i < proto::MAX_ROOMS; i++) {
    Room& r = g_rooms[i];
    if (r.alarm && !r.handling) {
      r.handling = true;
      notify("🏃 " + roomName(i) + " の非常ボタン：受付で対応を開始しました（" + nowStr() + "）");
    }
  }
  M5.Speaker.stop();
}

void resolveAll() {
  for (int i = 0; i < proto::MAX_ROOMS; i++) {
    Room& r = g_rooms[i];
    if (r.alarm && r.handling) {
      r.alarm    = false;
      r.handling = false;
      notify("✅ " + roomName(i) + " の非常事態は解除されました（" + nowStr() + "）");
    }
  }
}

void handleInput() {
  auto t = M5.Touch.getDetail();
  bool tap  = t.wasClicked() || M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || M5.BtnC.wasClicked();
  bool hold = t.wasHold() || M5.BtnA.wasHold() || M5.BtnB.wasHold() || M5.BtnC.wasHold();

  if (anyAlarm(true)) {
    if (tap || hold) acknowledgeAll();
  } else if (anyAlarm(false)) {
    if (hold) resolveAll();
  } else if (M5.BtnB.wasHold()) {
    notify(String("🔔 テスト送信：") + CLINIC_NAME + " 非常ボタン受信機は正常に動作しています（" +
           nowStr() + "）");
  }
}

// ---------- 音 ----------
void updateSound() {
#if ALARM_SOUND
  static uint32_t lastBeep = 0;
  if (anyAlarm(true) && millis() - lastBeep >= 600) {
    lastBeep = millis();
    M5.Speaker.tone(2200, 300);
  }
#endif
}

// ---------- 画面 ----------
M5Canvas g_canvas(&M5.Display);

String elapsed(uint32_t since) {
  uint32_t s = (millis() - since) / 1000;
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  return buf;
}

void draw() {
  M5Canvas& c = g_canvas;
  const int w = c.width();
  const int h = c.height();
  bool blink = (millis() / 400) % 2;

  if (anyAlarm(false)) {
    bool unhandled = anyAlarm(true);
    uint32_t bg = unhandled ? (blink ? TFT_RED : 0x6000) : 0xC300;  // 赤点滅 / オレンジ
    c.fillScreen(bg);
    c.setTextColor(TFT_WHITE);
    c.setTextDatum(top_center);
    c.setFont(&fonts::lgfxJapanGothicP_40);
    c.drawString(unhandled ? "非常ボタン" : "対応中", w / 2, 8);

    c.setFont(&fonts::lgfxJapanGothicP_32);
    c.setTextDatum(top_left);
    int y = 64;
    for (int i = 0; i < proto::MAX_ROOMS && y < h - 70; i++) {
      const Room& r = g_rooms[i];
      if (!r.alarm) continue;
      c.drawString(roomName(i), 12, y);
      c.setTextDatum(top_right);
      c.drawString(elapsed(r.alarmAt), w - 12, y);
      c.setTextDatum(top_left);
      y += 40;
    }
    c.setFont(&fonts::lgfxJapanGothicP_20);
    c.setTextDatum(bottom_center);
    c.drawString(unhandled ? "画面タップで「対応中」にする" : "画面を2秒長押しで解除", w / 2, h - 8);
  } else {
    c.fillScreen(0x0841);
    c.setTextColor(TFT_GREEN);
    c.setTextDatum(top_left);
    c.setFont(&fonts::lgfxJapanGothicP_32);
    c.drawString("待機中", 12, 6);

    c.setFont(&fonts::lgfxJapanGothicP_16);
    c.setTextDatum(top_right);
    bool wifi = WiFi.status() == WL_CONNECTED;
    c.setTextColor(wifi ? TFT_GREEN : TFT_YELLOW);
    c.drawString(strlen(WIFI_SSID) == 0 ? "Wi-Fi未設定" : (wifi ? "Wi-Fi接続中" : "Wi-Fi未接続"), w - 8, 8);
    c.setTextColor(TFT_LIGHTGREY);
    c.drawString(nowStr(), w - 8, 28);

    c.setFont(&fonts::lgfxJapanGothicP_24);
    int y = 56;
    for (int i = 0; i < NUM_ROOMS && y < h - 40; i++) {
      const Room& r = g_rooms[i];
      c.setTextDatum(top_left);
      c.setTextColor(TFT_WHITE);
      c.drawString(roomName(i), 16, y);
      c.setTextDatum(top_right);
      if (r.offline) {
        c.setTextColor(TFT_YELLOW);
        c.drawString(r.seen ? "応答なし" : "未接続", w - 16, y);
      } else if (r.seen) {
        c.setTextColor(TFT_GREEN);
        c.drawString("正常", w - 16, y);
      } else {
        c.setTextColor(TFT_LIGHTGREY);
        c.drawString("確認中…", w - 16, y);
      }
      y += 30;
    }
    c.setFont(&fonts::lgfxJapanGothicP_16);
    c.setTextColor(TFT_DARKGREY);
    c.setTextDatum(bottom_center);
    c.drawString("Bボタン2秒長押し：LINE WORKSにテスト送信", w / 2, h - 4);
  }
  c.pushSprite(0, 0);
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setRotation(1);
  M5.Speaker.setVolume(SPEAKER_VOLUME);
  M5.BtnA.setHoldThresh(2000);
  M5.BtnB.setHoldThresh(2000);
  M5.BtnC.setHoldThresh(2000);
  M5.Touch.setHoldThresh(2000);

  g_canvas.setColorDepth(16);
  g_canvas.setPsram(true);
  g_canvas.createSprite(M5.Display.width(), M5.Display.height());

  g_rxQueue     = xQueueCreate(32, sizeof(proto::Packet));
  g_notifyQueue = xQueueCreate(16, sizeof(NotifyMsg));
  xTaskCreatePinnedToCore(notifyTask, "notify", 12288, nullptr, 1, nullptr, 0);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // 省電力モードだと ESP-NOW を取りこぼすため無効
  if (strlen(WIFI_SSID) > 0) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    configTzTime("JST-9", "ntp.nict.jp", "pool.ntp.org");
  } else {
    // Wi-Fi なしで使う場合は固定チャンネル（送信機が自動で探します）
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW の初期化に失敗。再起動します");
    delay(1000);
    ESP.restart();
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.println("\n[clinic-alert 受信機] 起動しました");
}

void loop() {
  M5.update();

  proto::Packet p;
  while (xQueueReceive(g_rxQueue, &p, 0) == pdTRUE) handlePacket(p);

  static bool startupNotified = false;
  if (!startupNotified && WiFi.status() == WL_CONNECTED) {
    startupNotified = true;
    notify(String("ℹ️ ") + CLINIC_NAME + " 非常ボタン受信機が起動しました");
  }

  static uint32_t lastCheck = 0;
  if (millis() - lastCheck >= 5000) {
    lastCheck = millis();
    checkOffline();
  }

  handleInput();
  updateSound();

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 200) {
    lastDraw = millis();
    draw();
  }
  delay(5);
}

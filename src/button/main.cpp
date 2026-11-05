// ============================================================
//  送信機：M5Stack ATOM Lite（診察室の机の下に設置）
//
//  ・ボタンを LONG_PRESS_MS 押し続けると、受付へ非常信号を送る
//  ・受付から受領確認（ACK）が返るまで再送を続ける
//  ・定期的に生存確認を送り、受付側で故障や電源断を検知できるようにする
//  ・診察室側では音を出さない（相手に気づかれないため）
//  ・受信機の Wi-Fi チャンネルを自動で探して合わせる
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <Preferences.h>

#include "config.h"
#include "protocol.h"

namespace {

constexpr int PIN_BTN_INTERNAL = 39;  // ATOM Lite 本体ボタン
constexpr int PIN_LED          = 27;  // ATOM Lite 本体 RGB LED (SK6812)
constexpr uint8_t MIN_CHANNEL  = 1;
constexpr uint8_t MAX_CHANNEL  = 13;

const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

portMUX_TYPE g_ackMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t g_ackSeq = 0;
volatile uint8_t  g_ackOf  = 0;

Preferences g_prefs;
uint8_t  g_room    = BUTTON_DEFAULT_ROOM_ID;
uint8_t  g_channel = MIN_CHANNEL;
uint32_t g_seq     = 0;
bool     g_linkOk  = false;
uint32_t g_lastHeartbeat = 0;
uint32_t g_lastFaultBlink = 0;

// ---------- LED ----------
void led(uint8_t r, uint8_t g, uint8_t b) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(PIN_LED, r, g, b);
#else
  neopixelWrite(PIN_LED, r, g, b);
#endif
}
void ledBlink(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
  led(r, g, b);
  delay(ms);
  led(0, 0, 0);
}

// ---------- ボタン ----------
bool buttonDown() {
  bool down = false;
#if EXT_BUTTON_PIN >= 0
  down |= digitalRead(EXT_BUTTON_PIN) == LOW;
#endif
#if USE_INTERNAL_BUTTON
  down |= digitalRead(PIN_BTN_INTERNAL) == LOW;
#endif
  return down;
}

// ---------- ESP-NOW ----------
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
#else
void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  if (len != sizeof(proto::Packet)) return;
  proto::Packet p;
  memcpy(&p, data, sizeof(p));
  if (!proto::valid(p, CLINIC_NET_ID)) return;
  if (p.type != proto::ACK || p.room != g_room) return;
  portENTER_CRITICAL(&g_ackMux);
  g_ackSeq = p.seq;
  g_ackOf  = p.ack_of;
  portEXIT_CRITICAL(&g_ackMux);
}

bool ackReceived(uint8_t type, uint32_t seq) {
  portENTER_CRITICAL(&g_ackMux);
  bool ok = (g_ackSeq == seq && g_ackOf == type);
  portEXIT_CRITICAL(&g_ackMux);
  return ok;
}

void setChannel(uint8_t ch) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  g_channel = ch;
}

bool sendAndWait(proto::Type type, uint32_t seq, uint32_t timeoutMs) {
  proto::Packet p = proto::make(type, g_room, seq, CLINIC_NET_ID);
  esp_now_send(BCAST, reinterpret_cast<const uint8_t*>(&p), sizeof(p));
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (ackReceived(type, seq)) return true;
    delay(5);
  }
  return false;
}

// 全チャンネルを順に試して受信機を探す。見つかったチャンネルは保存する。
bool sweepChannels(proto::Type type, uint32_t seq) {
  for (uint8_t ch = MIN_CHANNEL; ch <= MAX_CHANNEL; ch++) {
    setChannel(ch);
    if (sendAndWait(type, seq, 120)) {
      g_prefs.putUChar("channel", ch);
      Serial.printf("[link] 受信機をチャンネル %u で発見\n", ch);
      return true;
    }
  }
  return false;
}

// 現在のチャンネルで数回試し、だめなら全チャンネルを探す
bool deliver(proto::Type type, uint32_t seq, int tries) {
  for (int i = 0; i < tries; i++) {
    if (sendAndWait(type, seq, 200)) return true;
  }
  return sweepChannels(type, seq);
}

// ---------- 非常発報 ----------
void runAlarm() {
  uint32_t seq = ++g_seq;
  Serial.printf("[alarm] 発報 room=%u seq=%lu\n", g_room, (unsigned long)seq);
  uint32_t start = millis();
  while (millis() - start < ALARM_RETRY_TIMEOUT_MS) {
    if (deliver(proto::ALARM, seq, 5)) {
      g_linkOk = true;
      Serial.println("[alarm] 受付に届きました");
#if LED_ACK_FEEDBACK
      ledBlink(0, 24, 0, 300);
#endif
      return;
    }
    Serial.println("[alarm] 受付から応答なし。再送します");
  }
  g_linkOk = false;
  Serial.println("[alarm] 再送時間切れ。受付に届いていない可能性があります");
}

void heartbeat() {
  uint32_t seq = ++g_seq;
  bool ok = deliver(proto::HEARTBEAT, seq, 3);
  if (ok != g_linkOk) {
    Serial.println(ok ? "[link] 受付と通信できています" : "[link] 受付と通信できません");
  }
  g_linkOk = ok;
}

// ---------- シリアルからの設定 ----------
// 「room 2」と入力すると部屋IDを 2 に設定（電源を切っても保持）
void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.startsWith("room")) {
    String arg = line.substring(4);
    arg.trim();
    int id = arg.toInt();
    if (arg.length() == 0 || id < 0 || id >= proto::MAX_ROOMS) {
      Serial.printf("部屋IDは 0〜%u で指定してください（例: room 1）\n", proto::MAX_ROOMS - 1);
      return;
    }
    g_room = static_cast<uint8_t>(id);
    g_prefs.putUChar("room", g_room);
    Serial.printf("部屋IDを %u に設定しました\n", g_room);
    g_lastHeartbeat = millis() - HEARTBEAT_INTERVAL_MS;  // すぐに生存確認を送る
  } else if (line == "status") {
    Serial.printf("room=%u channel=%u link=%s\n", g_room, g_channel, g_linkOk ? "OK" : "NG");
  } else if (line.length() > 0) {
    Serial.println("コマンド: room <番号> / status");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);
  pinMode(PIN_BTN_INTERNAL, INPUT);  // G39 は外部プルアップ付き
#if EXT_BUTTON_PIN >= 0
  pinMode(EXT_BUTTON_PIN, INPUT_PULLUP);
#endif
  led(0, 0, 16);  // 起動中: 薄い青

  g_prefs.begin("clinic-alert", false);
  g_room    = g_prefs.getUChar("room", BUTTON_DEFAULT_ROOM_ID);
  g_channel = g_prefs.getUChar("channel", MIN_CHANNEL);
  if (g_room >= proto::MAX_ROOMS) g_room = 0;
  if (g_channel < MIN_CHANNEL || g_channel > MAX_CHANNEL) g_channel = MIN_CHANNEL;
  g_seq = esp_random();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW の初期化に失敗。再起動します");
    delay(1000);
    ESP.restart();
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 0;  // 現在のチャンネルを使う
  peer.encrypt = false;
  esp_now_add_peer(&peer);
  setChannel(g_channel);

  Serial.printf("\n[clinic-alert 送信機] room=%u\n", g_room);
  Serial.println("シリアルで「room <番号>」と入力すると部屋IDを変更できます");

  heartbeat();
  g_lastHeartbeat = millis();
  ledBlink(g_linkOk ? 0 : 24, g_linkOk ? 24 : 0, 0, 500);  // 緑=接続OK / 赤=受付が見つからない
}

void loop() {
  static uint32_t pressStart = 0;
  static bool     pressing   = false;
  static bool     fired      = false;
  static uint32_t releaseAt  = 0;

  uint32_t now = millis();
  if (buttonDown()) {
    releaseAt = 0;
    if (!pressing) {
      pressing   = true;
      pressStart = now;
    }
    if (!fired && now - pressStart >= LONG_PRESS_MS) {
      fired = true;
      runAlarm();
      g_lastHeartbeat = millis();
    }
  } else if (pressing) {
    // 一瞬のチャタリングで押下判定が途切れないよう 50ms 待つ
    if (releaseAt == 0) releaseAt = now;
    if (now - releaseAt >= 50) {
      pressing = false;
      fired    = false;  // 離したら次の発報を受け付ける
    }
  }

  if (!pressing) {
    uint32_t interval = g_linkOk ? HEARTBEAT_INTERVAL_MS : 5000;  // 不通時は5秒ごとに再接続
    if (now - g_lastHeartbeat >= interval) {
      g_lastHeartbeat = now;
      heartbeat();
    }
#if LED_FAULT_INDICATOR
    if (!g_linkOk && now - g_lastFaultBlink >= 5000) {
      g_lastFaultBlink = now;
      ledBlink(16, 0, 0, 40);
    }
#endif
    handleSerial();
  }
  delay(10);
}

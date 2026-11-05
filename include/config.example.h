#pragma once
// ============================================================
//  設定ファイルの見本
//  このファイルを include/config.h にコピーしてから編集してください。
//  （config.h は .gitignore 済み。Wi-Fiパスワードや Webhook URL を
//    GitHub に上げないためです）
// ============================================================

// ---------- 共通（送信機・受信機で同じ値にする） ----------
// 院内で固有の値に変えてください（適当な16進8桁でOK）。
// 近くに同じシステムがあっても混線しなくなります。
#define CLINIC_NET_ID 0x5A3C9E71UL


// ---------- 送信機（ATOM Lite / 机の下のボタン） ----------
// 部屋IDの初期値。各ボタンにはシリアルモニタで「room 1」のように
// 入力して個別に設定できます（再書き込み不要・電源を切っても保持）。
#define BUTTON_DEFAULT_ROOM_ID 0

// 外付けボタンをつなぐピン（ATOM Lite の Grove 端子 = G26）。
// ボタンの片側を G26、もう片側を GND につなぎます。使わない場合は -1。
#define EXT_BUTTON_PIN 26

// ATOM Lite 本体のボタン（画面側の大きいボタン）でも発報するか。1=する / 0=しない
#define USE_INTERNAL_BUTTON 1

// 何ミリ秒押し続けたら発報するか（誤報防止）
#define LONG_PRESS_MS 1000

// 生存確認の送信間隔（ミリ秒）
#define HEARTBEAT_INTERVAL_MS 30000

// 受付に届いたとき、本体LEDを一瞬だけ緑に光らせる。1=する / 0=しない
#define LED_ACK_FEEDBACK 1

// 受付と通信できないとき、5秒ごとに本体LEDを赤く点滅させる。1=する / 0=しない
#define LED_FAULT_INDICATOR 1

// 受付に届くまで再送を続ける最大時間（ミリ秒）
#define ALARM_RETRY_TIMEOUT_MS 180000


// ---------- 受信機（Core2 / CoreS3 / 受付） ----------
#define CLINIC_NAME "〇〇クリニック"

// 院内 Wi-Fi（2.4GHz のみ。5GHz には接続できません）
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// LINE WORKS の Incoming Webhook で発行された URL
// 例: "https://webhook.worksmobile.com/message/xxxxxxxx..."
// 空のままなら LINE WORKS には送らず、受付の画面と音だけで知らせます。
#define LINEWORKS_WEBHOOK_URL ""

// HTTPS のルート証明書（PEM形式）。README の手順で取得して貼り付けると
// 通信相手が本物の LINE WORKS かどうかを確認するようになります（推奨）。
// 空のままでも送信はできますが、相手の確認をしません。
static const char LINEWORKS_ROOT_CA[] = "";

// 部屋の名前。配列の順番が部屋ID（0, 1, 2, ...）になります。最大16部屋。
static const char* const ROOM_NAMES[] = {
  "第1診察室",
  "第2診察室",
  "処置室",
};

// この時間ボタンから生存確認が来なければ「応答なし」として知らせる（ミリ秒）
#define OFFLINE_TIMEOUT_MS 120000

// 受付の警報音。1=鳴らす / 0=鳴らさない（画面と LINE WORKS のみ）
#define ALARM_SOUND 1
// 音量 0〜255
#define SPEAKER_VOLUME 200

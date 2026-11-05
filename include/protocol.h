#pragma once
// 送信機（ボタン）と受信機のあいだで ESP-NOW でやり取りするパケット
#include <stdint.h>

namespace proto {

constexpr uint32_t MAGIC   = 0x434C4E41;  // "CLNA"
constexpr uint8_t  VERSION = 1;
constexpr uint8_t  MAX_ROOMS = 16;        // 部屋IDは 0〜15

enum Type : uint8_t {
  ALARM     = 1,  // 非常ボタンが押された
  HEARTBEAT = 2,  // 生存確認（定期送信）
  ACK       = 3,  // 受信機からの受領確認
};

struct __attribute__((packed)) Packet {
  uint32_t magic;
  uint8_t  version;
  uint8_t  type;     // Type
  uint8_t  room;     // 部屋ID
  uint8_t  ack_of;   // ACK のとき、どの種類への応答か（ALARM / HEARTBEAT）
  uint32_t seq;      // 送信機ごとの連番（ACK は元パケットの seq をそのまま返す）
  uint32_t net_id;   // 院内ネットワークID（他のシステムとの混線防止）
};

inline Packet make(Type type, uint8_t room, uint32_t seq, uint32_t net_id, uint8_t ack_of = 0) {
  Packet p{};
  p.magic   = MAGIC;
  p.version = VERSION;
  p.type    = type;
  p.room    = room;
  p.ack_of  = ack_of;
  p.seq     = seq;
  p.net_id  = net_id;
  return p;
}

inline bool valid(const Packet& p, uint32_t net_id) {
  return p.magic == MAGIC && p.version == VERSION && p.net_id == net_id &&
         p.room < MAX_ROOMS && p.type >= ALARM && p.type <= ACK;
}

}  // namespace proto

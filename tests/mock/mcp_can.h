// Fake MCP2515 driver: frames the test puts in g_rx are "received",
// frames the sketch sends are collected in g_tx.
#pragma once
#include "Arduino.h"
#include <deque>
#include <vector>

#define MCP_ANY      3
#define MCP_16MHZ    1
#define MCP_8MHZ     2
#define CAN_125KBPS  10
#define MCP_NORMAL   0
#define CAN_OK       0
#define CAN_MSGAVAIL 3
#define CAN_NOMSG    4

struct Frame {
  unsigned long id;
  std::vector<uint8_t> d;
  unsigned long t;   // time sent (ms)
};
extern std::deque<Frame> g_rx;
extern std::vector<Frame> g_tx;

struct MCP_CAN {
  explicit MCP_CAN(int) {}
  uint8_t begin(uint8_t, uint8_t, uint8_t) { return CAN_OK; }
  uint8_t setMode(uint8_t) { return CAN_OK; }
  uint8_t checkReceive() { return g_rx.empty() ? CAN_NOMSG : CAN_MSGAVAIL; }
  uint8_t readMsgBuf(unsigned long* id, uint8_t* ext, uint8_t* len, uint8_t* buf) {
    Frame f = g_rx.front();
    g_rx.pop_front();
    *id = f.id; *ext = 1; *len = (uint8_t)f.d.size();
    memcpy(buf, f.d.data(), f.d.size());
    return CAN_OK;
  }
  uint8_t sendMsgBuf(unsigned long id, uint8_t ext, uint8_t len, uint8_t* buf) {
    (void)ext;
    g_tx.push_back({id, std::vector<uint8_t>(buf, buf + len), g_ms});
    return CAN_OK;
  }
};

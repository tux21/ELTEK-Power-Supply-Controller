// PC test for Flatpack2_Charger.ino
//
// Runs the real sketch against a simulated Flatpack2 (fake CAN driver and a
// fake clock) and checks the CAN frames it sends and the charge stages.
// Build and run with:  ./run_tests.sh      (add -v to see the serial output)

#include <algorithm>
#include "Arduino.h"
#include "mcp_can.h"

unsigned long      g_ms = 0;
int                g_led = 0;
std::string        g_log;
bool               g_verbose = false;
SerialT            Serial;
std::deque<Frame>  g_rx;
std::vector<Frame> g_tx;

#include "../Flatpack2_Charger/Flatpack2_Charger.ino"

// ---------------------------------------------------------------- test helpers
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                   \
  do {                                                                     \
    if (cond) { g_pass++; printf("  ok    %s\n", msg); }                   \
    else      { g_fail++; printf("  FAIL  %s   (line %d)\n", msg, __LINE__); } \
  } while (0)

static void section(const char* name) { printf("\n%s\n", name); }

// Serial number from the label of a real Flatpack2 48V 1800 W: 060371019754
static const uint8_t SN1[6] = {0x06, 0x03, 0x71, 0x01, 0x97, 0x54};
static const uint8_t SN2[6] = {0x12, 0x02, 0x71, 0x10, 0x10, 0x60};
static const uint8_t SN3[6] = {0x14, 0x14, 0x71, 0x11, 0x40, 0x04};  // hello ID 0x05004004

static void hello(const uint8_t* sn) {
  unsigned long id = 0x05000000UL | ((unsigned long)sn[4] << 8) | sn[5];
  g_rx.push_back({id, {0x1B, sn[0], sn[1], sn[2], sn[3], sn[4], sn[5], 0x00}, g_ms});
}
static void loginRequest(int presetId, const uint8_t* sn) {
  g_rx.push_back({0x05004400UL | ((unsigned long)presetId << 16),
                  {sn[0], sn[1], sn[2], sn[3], sn[4], sn[5], 0x00, 0x00}, g_ms});
}
static void status(int id, uint8_t st, float v, float a) {
  uint16_t cv = (uint16_t)(v * 100 + 0.5), da = (uint16_t)(a * 10 + 0.5), vin = 230;
  g_rx.push_back({0x05004000UL | ((unsigned long)id << 16) | st,
                  {25, (uint8_t)(da & 255), (uint8_t)(da >> 8), (uint8_t)(cv & 255),
                   (uint8_t)(cv >> 8), (uint8_t)(vin & 255), (uint8_t)(vin >> 8), 38}, g_ms});
}

// simulated Flatpacks with ID 1 and 2: send status every 200 ms while "on"
struct Sim { bool on; uint8_t st; float v, a; };
static Sim sim[3];

static void step(unsigned long ms) {
  unsigned long end = g_ms + ms;
  while (g_ms < end) {
    if (g_ms % 200 == 0)
      for (int id = 1; id <= 2; id++)
        if (sim[id].on) status(id, sim[id].st, sim[id].v, sim[id].a);
    loop();
    g_ms += 10;
  }
}

static const Frame* lastTx(unsigned long id) {
  for (auto it = g_tx.rbegin(); it != g_tx.rend(); ++it)
    if (it->id == id) return &*it;
  return nullptr;
}
static uint16_t le16(const Frame* f, int i) { return f->d[i] | (f->d[i + 1] << 8); }
static bool logHas(const char* s) { return g_log.find(s) != std::string::npos; }
static bool payloadIs(const Frame* f, std::vector<uint8_t> want) { return f && f->d == want; }

// ---------------------------------------------------------------- tests
int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "-v") g_verbose = true;

  section("1. Start-up");
  setup();
  CHECK(logHas("v" FW_VERSION), "banner shows the version");
  step(11000);
  CHECK(g_tx.empty(), "sends nothing before a Flatpack is seen");
  CHECK(logHas("Nothing received on CAN yet"), "prints the wiring hint when the bus is silent");

  section("2. Flatpack announces itself (hello 0x0500xxxx)");
  g_tx.clear();
  hello(SN1);
  step(10);
  CHECK(logHas("serial 060371019754 -> FP1"), "reads the serial number and assigns ID 1");
  const Frame* login = lastTx(0x05004804UL);
  CHECK(payloadIs(login, {0x06, 0x03, 0x71, 0x01, 0x97, 0x54, 0x00, 0x00}),
        "logs in at once: 0x05004804 = serial + 00 00");
  const Frame* sp = lastTx(0x05FF4004UL);
  CHECK(sp && sp->d.size() == 8, "sends setpoint 0x05FF4004 (broadcast, 5 s walk-in)");
  CHECK(sp && le16(sp, 0) == 200, "current limit 20.0 A (200 dA)");
  CHECK(sp && le16(sp, 2) == 5520 && le16(sp, 4) == 5520, "voltage 55.20 V (5520 cV) in both fields");
  CHECK(sp && le16(sp, 6) == 5750, "over-voltage limit 57.50 V");

  section("3. Charging starts and the login is kept alive");
  sim[1] = {true, 0x08, 50.0f, 20.0f};
  step(3000);
  CHECK(stage == CHARGING, "stage = CHARGING once status arrives");
  g_tx.clear();
  step(20000);
  int logins = 0; unsigned long prev = 0, maxGap = 0;
  for (auto& f : g_tx)
    if (f.id == 0x05004804UL) {
      if (logins) maxGap = std::max(maxGap, f.t - prev);
      prev = f.t; logins++;
    }
  CHECK(logins >= 9, "logs in again every 2 s");
  CHECK(maxGap <= 2100, "never more than 2.1 s between logins (Flatpack drops out after ~13 s)");
  CHECK(logHas("FP1: 50.00 V 20.0 A CC AC 230 V 25/38 C"), "status line decodes V, A, mode, AC and temps");

  section("4. CV phase: current tapers, battery full after 10 min below 2 A");
  sim[1] = {true, 0x04, 55.2f, 1.5f};
  step(9UL * 60000);
  CHECK(stage == CHARGING, "still CHARGING after 9 min");
  step(90000);
  CHECK(stage == FULL, "FULL after 10 min below the tail current");
  step(2100);
  sp = lastTx(0x05FF4004UL);
  CHECK(sp && le16(sp, 2) == 5360 && le16(sp, 4) == 5360, "setpoint drops to float 53.60 V");
  { bool solid = true; for (int k = 0; k < 20; k++) { step(100); if (!g_led) solid = false; }
    CHECK(solid, "LED solid when full"); }

  section("5. Float: battery takes current again -> new charge");
  sim[1] = {true, 0x08, 53.0f, 10.0f};
  step(30000);
  CHECK(stage == FULL, "short current draw (30 s) does not restart");
  step(40000);
  CHECK(stage == CHARGING, "more than 5 A for 60 s restarts the charge");
  step(2100);
  sp = lastTx(0x05FF4004UL);
  CHECK(sp && le16(sp, 2) == 5520, "setpoint back to 55.20 V");

  section("6. Tail timer restarts if the current rises again");
  sim[1] = {true, 0x04, 55.2f, 1.5f};
  step(8UL * 60000);
  sim[1].a = 3.0f;  step(10000);
  sim[1].a = 1.5f;  step(8UL * 60000);
  CHECK(stage == CHARGING, "8 + 8 min with a 3 A bump in between is not 'full'");
  step(3UL * 60000);
  CHECK(stage == FULL, "full after a clean 10 min");
  sim[1] = {true, 0x08, 52.0f, 10.0f};
  step(65000);
  CHECK(stage == CHARGING, "(back to charging for the next tests)");

  section("7. Alarm handling");
  sim[1].st = 0x0C;
  g_tx.clear();
  step(1000);
  CHECK(payloadIs(lastTx(0x0501BFFCUL), {0x08, 0x08, 0x00}), "asks for alarm bits (0x0501BFFC 08 08 00)");
  g_rx.push_back({0x0501BFFCUL, {0x0E, 0x08, 0x00, 0x08, 0x08, 0x00, 0x00}, g_ms});
  step(10);
  CHECK(logHas("FP1 ALARM: High mains; Fan 1 speed low;"), "decodes and prints the alarm names");
  sim[1].st = 0x08;
  step(1000);

  section("8. Second Flatpack (login request 0x05xx4400)");
  loginRequest(2, SN2);
  step(10);
  CHECK(logHas("serial 120271101060 -> FP2"), "second unit found and given ID 2");
  sim[2] = {true, 0x08, 52.0f, 10.0f};
  g_tx.clear();
  step(6000);
  CHECK(payloadIs(lastTx(0x05004808UL), {0x12, 0x02, 0x71, 0x10, 0x10, 0x60, 0x00, 0x00}),
        "logs in unit 2 with 0x05004808");
  sp = lastTx(0x05FF4004UL);
  CHECK(sp && le16(sp, 0) == 100, "20 A total split: 10.0 A per Flatpack");
  sim[2].on = false;
  step(8000);
  sp = lastTx(0x05FF4004UL);
  CHECK(sp && le16(sp, 0) == 200, "unit 2 stops: unit 1 gets the full 20.0 A again");

  section("9. Grid off, then back on");
  sim[1].on = false;
  step(35000);
  CHECK(stage == WAITING, "WAITING after 30 s with no Flatpack");
  g_tx.clear();
  hello(SN1);
  step(10);
  CHECK(payloadIs(lastTx(0x05004804UL), {0x06, 0x03, 0x71, 0x01, 0x97, 0x54, 0x00, 0x00}),
        "known unit says hello -> logged in immediately");
  sim[1] = {true, 0x10, 48.0f, 0.0f};
  step(3000);
  CHECK(stage == CHARGING, "new charge starts");
  step(5UL * 60000);
  CHECK(logHas("FP2 silent for 5 min - forgotten"), "unit missing for 5 min is forgotten");

  section("10. Safety timer: never reaches tail current (constant load)");
  sim[1] = {true, 0x08, 52.0f, 20.0f};
  unsigned long start = g_ms;
  while (!logHas("Max charge time reached") && g_ms - start < 13UL * 3600000UL) step(1000);
  CHECK(logHas("Max charge time reached"), "max charge time triggers");
  CHECK(g_ms - start >= 11UL * 3600000UL + 50UL * 60000UL, "only after ~12 h");
  CHECK(stage == FULL, "goes to FULL / float");
  step(5UL * 60000);
  CHECK(stage == FULL, "stays FULL although 20 A still flows (no endless re-charge loop)");
  sp = lastTx(0x05FF4004UL);
  CHECK(sp && le16(sp, 2) == 5360, "holds float 53.60 V");

  section("11. Odd hello ID that looks like a status frame");
  hello(SN3);   // CAN ID 0x05004004 with byte0 = 0x1B
  step(10);
  CHECK(logHas("serial 141471114004 -> FP2"), "treated as hello, not as status");

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}

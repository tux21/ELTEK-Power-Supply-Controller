/*
  ============================================================================
   Eltek Flatpack2 48V  ->  16S LiFePO4 charger controller        v1.0.0
   https://github.com/tux21/ELTEK-Power-Supply-Controller
  ============================================================================
   Hardware : Arduino Nano or Arduino Pro Mini 5 V / 16 MHz (ATmega328P or ATmega168)
              + MCP2515 CAN module (the blue board with MCP2515 + TJA1050)
   Library  : "mcp_can" by coryjfowler  (Arduino IDE -> Library Manager)

   What it does:
   - Listens on the Flatpack CAN bus (125 kbit/s) and finds every Flatpack2
     by its serial number automatically (no need to type the serial in).
   - Logs in to each Flatpack every 2 s and sends: charge voltage, current
     limit and over-voltage limit. The Flatpack then does CC/CV by itself:
     constant current until the battery reaches CHARGE_VOLTAGE, then
     constant voltage while the current tapers off.
   - When the current has stayed below TAIL_CURRENT for TAIL_MINUTES the
     battery is full -> drops to FLOAT_VOLTAGE (can be switched off).
   - Prints status to the Serial Monitor (115200 baud) and can drive an
     optional status LED.

   If this Arduino stops talking, the Flatpack falls back to its own default
   (factory 53.5 V, full current) about 15 s later.
  ============================================================================
*/

#include <SPI.h>
#include <mcp_can.h>

// =================== SETTINGS - change only these numbers ===================

constexpr float CHARGE_VOLTAGE   = 55.20;  // V  CC/CV charge voltage (55.2 V = 3.45 V/cell)
constexpr float MAX_CURRENT      = 20.0;   // A  TOTAL charge current into the battery.
                                           //    With several Flatpacks it is split between them.
constexpr bool  ENABLE_FLOAT     = true;   // true  = after full, drop to FLOAT_VOLTAGE
                                           // false = stay at CHARGE_VOLTAGE forever
constexpr float FLOAT_VOLTAGE    = 53.60;  // V  float voltage (53.6 V = 3.35 V/cell)
constexpr float TAIL_CURRENT     = 2.0;    // A  battery is "full" when current stays below this...
constexpr unsigned int TAIL_MINUTES = 10;  //    ...for this many minutes at charge voltage
constexpr float REBULK_CURRENT   = 5.0;    // A  in float: if battery takes more than this for
                                           //    1 minute, start a new full charge
constexpr unsigned int MAX_CHARGE_HOURS = 12; // h  safety: go to float after this long anyway (0 = off)
constexpr float OVP_VOLTAGE      = 57.50;  // V  Flatpack shuts itself down above this
constexpr bool  LONG_WALK_IN     = false;  // false = 5 s soft start, true = 60 s soft start

// Optional, normally leave false. true = permanently store DEFAULT_VOLTAGE inside
// the Flatpack, so it starts at that voltage even with no Arduino connected
// (but then with NO current limit). Survives power-off. Set back to false after use.
// NOTE: only works on newer Flatpacks (revision 3.0 and up). Older units such as
// the 1800 W "FLATPACK2 48V" version 1.4 ignore it - leave false for those.
constexpr bool  WRITE_DEFAULT_VOLTAGE = false;
constexpr float DEFAULT_VOLTAGE  = 55.20;  // V

// Hardware
constexpr byte  CAN_CS_PIN = 10;           // MCP2515 CS pin
#define CAN_CRYSTAL MCP_8MHZ               // crystal on MCP2515 board: "8.000" -> MCP_8MHZ
                                           //                          "16.000" -> MCP_16MHZ
constexpr byte  LED_PIN    = 4;            // optional LED (+ 1k resistor to GND). Not 13 (used by SPI).

// ============================================================================
//                    Nothing below needs to be changed
// ============================================================================

#define FW_VERSION "1.0.0"

// Flatpack2 48V limits
constexpr float FP_MIN_VOLTAGE = 43.5;
constexpr float FP_MAX_VOLTAGE = 57.6;
constexpr float FP_MAX_CURRENT = 33.6;     // per module: 33.6 A = 1800 W model (safe for all).
                                           // Only 2000 W HE units can use 41.7.

static_assert(CHARGE_VOLTAGE >= FP_MIN_VOLTAGE && CHARGE_VOLTAGE <= FP_MAX_VOLTAGE,
              "CHARGE_VOLTAGE must be between 43.5 and 57.6 V");
static_assert(FLOAT_VOLTAGE >= FP_MIN_VOLTAGE && FLOAT_VOLTAGE <= CHARGE_VOLTAGE,
              "FLOAT_VOLTAGE must be between 43.5 V and CHARGE_VOLTAGE");
static_assert(DEFAULT_VOLTAGE >= FP_MIN_VOLTAGE && DEFAULT_VOLTAGE <= FP_MAX_VOLTAGE,
              "DEFAULT_VOLTAGE must be between 43.5 and 57.6 V");
static_assert(OVP_VOLTAGE >= CHARGE_VOLTAGE + 1.0 && OVP_VOLTAGE <= 59.5,
              "OVP_VOLTAGE must be at least 1 V above CHARGE_VOLTAGE and max 59.5 V");
static_assert(MAX_CURRENT > 0.5, "MAX_CURRENT too low");

// Timing
const unsigned long SEND_INTERVAL_MS   = 2000;   // login + setpoint (Flatpack logs out after ~13 s)
const unsigned long REPORT_INTERVAL_MS = 2000;   // serial status line
const unsigned long ACTIVE_TIMEOUT_MS  = 5000;   // no status for 5 s -> module not active
const unsigned long GONE_TIMEOUT_MS    = 30000;  // no module for 30 s -> new charge next time
const unsigned long FORGET_TIMEOUT_MS  = 300000; // module silent 5 min -> free its slot
const unsigned long REBULK_TIME_MS     = 60000;

MCP_CAN CAN(CAN_CS_PIN);

const byte MAX_UNITS = 4;   // up to 4 Flatpacks on one bus, IDs 1..4

struct Unit {
  bool          used;
  byte          serial[6];
  unsigned long lastSeen;     // any message from it
  unsigned long lastStatus;   // last status message
  unsigned long lastAlarmReq;
  float         vout;         // V
  float         iout;         // A
  uint16_t      vin;          // V AC
  byte          tIn, tOut;    // deg C
  byte          state;        // 0x04 CV, 0x08 CC/warning, 0x0C alarm, 0x10 walk-in
  byte          alarm1, alarm2;
  bool          defaultWritten;
};
Unit units[MAX_UNITS];

enum Stage : byte { WAITING, CHARGING, FULL };
Stage stage = WAITING;

unsigned long stageStart     = 0;
unsigned long lastAnyStatus  = 0;
unsigned long lastSend       = 0;
unsigned long lastReport     = 0;
unsigned long lastTick       = 0;
unsigned long lastRxAny      = 0;
unsigned long lastNoRxHint   = 0;
bool          tailTiming     = false;
unsigned long tailSince      = 0;
bool          rebulkTiming   = false;
unsigned long rebulkSince    = 0;
bool          rebulkAllowed  = true;   // false after the max-time cut-off
bool          anyRx          = false;
unsigned int  txFailCount    = 0;

// ---------------------------------------------------------------- alarm text
const char al10[] PROGMEM = "OVS lock-out";
const char al11[] PROGMEM = "Module fail primary";
const char al12[] PROGMEM = "Module fail secondary";
const char al13[] PROGMEM = "High mains";
const char al14[] PROGMEM = "Low mains";
const char al15[] PROGMEM = "High temp";
const char al16[] PROGMEM = "Low temp";
const char al17[] PROGMEM = "Current limit";
const char al20[] PROGMEM = "Internal voltage";
const char al21[] PROGMEM = "Module fail";
const char al22[] PROGMEM = "Module fail secondary";
const char al23[] PROGMEM = "Fan 1 speed low";
const char al24[] PROGMEM = "Fan 2 speed low";
const char al25[] PROGMEM = "Sub module 1 fail";
const char al26[] PROGMEM = "Fan 3 speed low";
const char al27[] PROGMEM = "Inner voltage";
const char* const ALARM_TEXT[16] PROGMEM = {
  al10, al11, al12, al13, al14, al15, al16, al17,
  al20, al21, al22, al23, al24, al25, al26, al27
};

void printP(const char* p) {
  char c;
  while ((c = pgm_read_byte(p++))) Serial.write(c);
}

// ---------------------------------------------------------------- helpers
void printSerial(const byte* s) {
  for (byte i = 0; i < 6; i++) {
    if (s[i] < 0x10) Serial.print('0');
    Serial.print(s[i], HEX);
  }
}

byte activeUnits(unsigned long now) {
  byte n = 0;
  for (byte i = 0; i < MAX_UNITS; i++)
    if (units[i].used && units[i].lastStatus && now - units[i].lastStatus < ACTIVE_TIMEOUT_MS) n++;
  return n;
}

float targetVoltage() {
  if (stage == FULL && ENABLE_FLOAT) return FLOAT_VOLTAGE;
  return CHARGE_VOLTAGE;
}

float currentPerUnit(unsigned long now) {
  byte n = activeUnits(now);
  if (n == 0) {
    for (byte i = 0; i < MAX_UNITS; i++) if (units[i].used) n++;
  }
  if (n == 0) n = 1;
  float a = MAX_CURRENT / n;
  if (a > FP_MAX_CURRENT) a = FP_MAX_CURRENT;
  if (a < 0.5) a = 0.5;
  return a;
}

bool canSend(unsigned long id, byte len, byte* data) {
  byte r = CAN.sendMsgBuf(id, 1, len, data);   // 1 = extended 29-bit ID
  if (r != CAN_OK) {
    txFailCount++;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- CAN commands
void sendLogin(byte idx) {
  byte d[8];
  memcpy(d, units[idx].serial, 6);
  d[6] = 0x00;
  d[7] = 0x00;
  canSend(0x05004800UL | (unsigned long)((idx + 1) * 4), 8, d);   // ID = idx+1
}

void sendSetpoint(float volts, float amps) {
  uint16_t dA  = (uint16_t)(amps * 10.0 + 0.5);
  uint16_t cV  = (uint16_t)(volts * 100.0 + 0.5);
  uint16_t ovp = (uint16_t)(OVP_VOLTAGE * 100.0 + 0.5);
  byte d[8] = {
    (byte)(dA & 0xFF),  (byte)(dA >> 8),    // current limit
    (byte)(cV & 0xFF),  (byte)(cV >> 8),    // "measured" voltage = same as wanted
    (byte)(cV & 0xFF),  (byte)(cV >> 8),    // wanted voltage
    (byte)(ovp & 0xFF), (byte)(ovp >> 8)    // over-voltage protection
  };
  // 0x05FF.... = broadcast to all logged-in Flatpacks
  canSend(LONG_WALK_IN ? 0x05FF4005UL : 0x05FF4004UL, 8, d);
}

void sendDefaultVoltage(byte idx) {
  uint16_t cV = (uint16_t)(DEFAULT_VOLTAGE * 100.0 + 0.5);
  byte d[5] = { 0x29, 0x15, 0x00, (byte)(cV & 0xFF), (byte)(cV >> 8) };
  unsigned long id = 0x05009C00UL | ((unsigned long)(idx + 1) << 16);
  canSend(id, 5, d);            // documented form, addressed to this module's ID
  canSend(0x05009C00UL, 5, d);  // form used by other working projects
  Serial.print(F(">> FP"));
  Serial.print(idx + 1);
  Serial.print(F(": default voltage "));
  Serial.print(DEFAULT_VOLTAGE, 2);
  Serial.println(F(" V written (active after this Arduino disconnects)"));
}

void requestAlarms(byte idx) {
  byte d[3] = { 0x08, 0x08, 0x00 };   // 0x08 = ask for alarms
  unsigned long id = 0x0500BFFCUL | ((unsigned long)(idx + 1) << 16);
  canSend(id, 3, d);
}

// ---------------------------------------------------------------- receive
void registerUnit(const byte* serial, unsigned long now) {
  // already known?
  for (byte i = 0; i < MAX_UNITS; i++) {
    if (units[i].used && memcmp(units[i].serial, serial, 6) == 0) {
      units[i].lastSeen = now;
      // it is announcing itself = it is logged out -> log in right away
      sendLogin(i);
      sendSetpoint(targetVoltage(), currentPerUnit(now));
      return;
    }
  }
  // new one -> free slot
  for (byte i = 0; i < MAX_UNITS; i++) {
    if (!units[i].used) {
      memset(&units[i], 0, sizeof(Unit));
      units[i].used = true;
      memcpy(units[i].serial, serial, 6);
      units[i].lastSeen = now;
      Serial.print(F(">> Found Flatpack, serial "));
      printSerial(serial);
      Serial.print(F(" -> FP"));
      Serial.println(i + 1);
      sendLogin(i);
      sendSetpoint(targetVoltage(), currentPerUnit(now));
      return;
    }
  }
  Serial.println(F(">> More than 4 Flatpacks found - extra one ignored"));
}

void handleFrame(unsigned long id, byte len, const byte* d, unsigned long now) {
  // 1) "hello" from a logged-out Flatpack: 0x0500xxxx, byte0 = 0x1B, serial in bytes 1..6
  if ((id & 0xFFFF0000UL) == 0x05000000UL && len == 8 && d[0] == 0x1B) {
    registerUnit(d + 1, now);
    return;
  }
  // 2) "login request": 0x05xx4400, serial in bytes 0..5
  if ((id & 0xFF00FFFFUL) == 0x05004400UL && len >= 6) {
    registerUnit(d, now);
    return;
  }

  byte uid = (id >> 16) & 0xFF;              // module ID inside the CAN ID
  if (uid < 1 || uid > MAX_UNITS) return;
  Unit& u = units[uid - 1];
  if (!u.used) return;

  // 3) status: 0x05ii40ss
  if ((id & 0xFF00FF00UL) == 0x05004000UL && len == 8) {
    u.tIn   = d[0];
    u.iout  = (uint16_t)(d[1] | (d[2] << 8)) / 10.0;
    u.vout  = (uint16_t)(d[3] | (d[4] << 8)) / 100.0;
    u.vin   = (uint16_t)(d[5] | (d[6] << 8));
    u.tOut  = d[7];
    u.state = id & 0xFF;
    u.lastStatus = now;
    u.lastSeen   = now;
    lastAnyStatus = now;

    if (u.state == 0x0C) {
      if (now - u.lastAlarmReq > 5000) {
        u.lastAlarmReq = now;
        requestAlarms(uid - 1);
      }
    } else {
      u.alarm1 = u.alarm2 = 0;
    }

    if (WRITE_DEFAULT_VOLTAGE && !u.defaultWritten) {
      u.defaultWritten = true;
      sendDefaultVoltage(uid - 1);
    }
    return;
  }

  // 4) alarm answer: 0x05iiBFFC, byte0 = 0x0E
  if ((id & 0xFF00FFFFUL) == 0x0500BFFCUL && len >= 5 && d[0] == 0x0E) {
    bool changed = (u.alarm1 != d[3] || u.alarm2 != d[4]);
    u.alarm1 = d[3];
    u.alarm2 = d[4];
    if (changed) {
      Serial.print(F("!! FP"));
      Serial.print(uid);
      Serial.print(F(" ALARM:"));
      for (byte b = 0; b < 16; b++) {
        byte bits = (b < 8) ? u.alarm1 : u.alarm2;
        if (bits & (1 << (b & 7))) {
          Serial.print(' ');
          printP((const char*)pgm_read_word(&ALARM_TEXT[b]));
          Serial.print(';');
        }
      }
      Serial.println();
    }
  }
}

// ---------------------------------------------------------------- charge logic
void setStage(Stage s, unsigned long now, const __FlashStringHelper* why) {
  stage = s;
  stageStart = now;
  tailTiming = false;
  rebulkTiming = false;
  Serial.print(F("== "));
  Serial.println(why);
}

void chargeTick(unsigned long now) {
  byte n = activeUnits(now);

  if (n == 0) {
    if (stage != WAITING && now - lastAnyStatus > GONE_TIMEOUT_MS)
      setStage(WAITING, now, F("No Flatpack running - waiting (next start = new charge)"));
    return;
  }

  float v = 0, i = 0;
  for (byte k = 0; k < MAX_UNITS; k++) {
    Unit& u = units[k];
    if (u.used && u.lastStatus && now - u.lastStatus < ACTIVE_TIMEOUT_MS) {
      v += u.vout;
      i += u.iout;
    }
  }
  v /= n;

  if (stage == WAITING) {
    rebulkAllowed = true;
    setStage(CHARGING, now, F("Flatpack running - CHARGING started"));
    return;
  }

  if (stage == CHARGING) {
    if (i < TAIL_CURRENT && v >= CHARGE_VOLTAGE - 0.3) {
      if (!tailTiming) { tailTiming = true; tailSince = now; }
      if (now - tailSince >= (unsigned long)TAIL_MINUTES * 60000UL) {
        rebulkAllowed = true;
        setStage(FULL, now, ENABLE_FLOAT ? F("Battery FULL - switching to float")
                                         : F("Battery FULL - holding charge voltage"));
        return;
      }
    } else {
      tailTiming = false;
    }
    if (MAX_CHARGE_HOURS > 0 && now - stageStart >= (unsigned long)MAX_CHARGE_HOURS * 3600000UL) {
      // Battery never reached the tail current (big load or a problem).
      // Stay in FULL until the Flatpack restarts - no automatic re-charge.
      rebulkAllowed = false;
      setStage(FULL, now, F("Max charge time reached - FULL until the Flatpack restarts"));
    }
    return;
  }

  if (stage == FULL) {
    if (rebulkAllowed && i > REBULK_CURRENT) {
      if (!rebulkTiming) { rebulkTiming = true; rebulkSince = now; }
      if (now - rebulkSince >= REBULK_TIME_MS)
        setStage(CHARGING, now, F("Battery taking current again - new CHARGE started"));
    } else {
      rebulkTiming = false;
    }
  }
}

// ---------------------------------------------------------------- status output
void printState(byte s) {
  switch (s) {
    case 0x04: Serial.print(F("CV"));      break;
    case 0x08: Serial.print(F("CC"));      break;
    case 0x0C: Serial.print(F("ALARM"));   break;
    case 0x10: Serial.print(F("walk-in")); break;
    default:   Serial.print(F("0x")); Serial.print(s, HEX);
  }
}

void report(unsigned long now) {
  switch (stage) {
    case WAITING:  Serial.print(F("WAITING  ")); break;
    case CHARGING: Serial.print(F("CHARGING ")); break;
    case FULL:     Serial.print(ENABLE_FLOAT ? F("FLOAT    ") : F("FULL     ")); break;
  }
  Serial.print(F("set "));
  Serial.print(targetVoltage(), 2);
  Serial.print(F(" V / "));
  Serial.print(MAX_CURRENT, 1);
  Serial.print(F(" A"));

  if (stage == CHARGING && tailTiming) {
    Serial.print(F("  (tail "));
    Serial.print((now - tailSince) / 60000UL);
    Serial.print('/');
    Serial.print(TAIL_MINUTES);
    Serial.print(F(" min)"));
  }

  bool any = false;
  for (byte k = 0; k < MAX_UNITS; k++) {
    Unit& u = units[k];
    if (!u.used) continue;
    any = true;
    Serial.print(F(" | FP"));
    Serial.print(k + 1);
    Serial.print(F(": "));
    if (!u.lastStatus || now - u.lastStatus >= ACTIVE_TIMEOUT_MS) {
      Serial.print(F("no data"));
      continue;
    }
    Serial.print(u.vout, 2);
    Serial.print(F(" V "));
    Serial.print(u.iout, 1);
    Serial.print(F(" A "));
    printState(u.state);
    Serial.print(F(" AC "));
    Serial.print(u.vin);
    Serial.print(F(" V "));
    Serial.print(u.tIn);
    Serial.print('/');
    Serial.print(u.tOut);
    Serial.print(F(" C"));
  }
  if (!any) Serial.print(F(" | no Flatpack found yet"));
  Serial.println();

  if (txFailCount) {
    Serial.print(F("!! CAN send errors: "));
    Serial.print(txFailCount);
    Serial.println(F(" (check CAN-H/CAN-L and GND)"));
    txFailCount = 0;
  }
}

// ---------------------------------------------------------------- LED
void updateLed(unsigned long now) {
  bool alarm = false;
  for (byte k = 0; k < MAX_UNITS; k++)
    if (units[k].used && units[k].state == 0x0C && now - units[k].lastStatus < ACTIVE_TIMEOUT_MS) alarm = true;

  bool on;
  if (alarm)                 on = (now / 100) % 2;          // fast flicker = alarm
  else if (stage == WAITING) on = (now % 2000) < 60;        // short blip  = waiting
  else if (stage == CHARGING) on = (now / 500) % 2;         // 1 Hz blink  = charging
  else                        on = true;                    // solid       = full / float
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("=== Flatpack2 LiFePO4 charger v" FW_VERSION " ==="));
  Serial.print(F("Charge ")); Serial.print(CHARGE_VOLTAGE, 2);
  Serial.print(F(" V, max ")); Serial.print(MAX_CURRENT, 1);
  Serial.print(F(" A, float "));
  if (ENABLE_FLOAT) { Serial.print(FLOAT_VOLTAGE, 2); Serial.print(F(" V")); }
  else Serial.print(F("off"));
  Serial.print(F(", OVP ")); Serial.print(OVP_VOLTAGE, 2); Serial.println(F(" V"));

  while (CAN.begin(MCP_ANY, CAN_125KBPS, CAN_CRYSTAL) != CAN_OK) {
    Serial.println(F("!! MCP2515 not responding - check wiring (CS=D10, SI=D11, SO=D12, SCK=D13, VCC=5V)"));
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(1000);
  }
  CAN.setMode(MCP_NORMAL);
  Serial.println(F("CAN ready (125 kbit/s). Waiting for Flatpack..."));
  lastNoRxHint = millis();
}

void loop() {
  unsigned long now = millis();

  // read everything that arrived
  while (CAN.checkReceive() == CAN_MSGAVAIL) {
    unsigned long id;
    byte ext, len, buf[8];
    if (CAN.readMsgBuf(&id, &ext, &len, buf) == CAN_OK && ext) {
      anyRx = true;
      lastRxAny = now;
      handleFrame(id & 0x1FFFFFFFUL, len, buf, now);
    }
  }

  // login + setpoint every 2 s
  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;
    bool any = false;
    for (byte i = 0; i < MAX_UNITS; i++) {
      if (!units[i].used) continue;
      if (now - units[i].lastSeen > FORGET_TIMEOUT_MS) {
        Serial.print(F(">> FP")); Serial.print(i + 1);
        Serial.println(F(" silent for 5 min - forgotten"));
        units[i].used = false;
        continue;
      }
      sendLogin(i);
      any = true;
    }
    if (any) sendSetpoint(targetVoltage(), currentPerUnit(now));
  }

  // charge logic every 1 s
  if (now - lastTick >= 1000) {
    lastTick = now;
    chargeTick(now);
  }

  // status line
  if (now - lastReport >= REPORT_INTERVAL_MS) {
    lastReport = now;
    report(now);
    if (!anyRx && now - lastNoRxHint >= 10000) {
      lastNoRxHint = now;
      Serial.println(F("!! Nothing received on CAN yet. Check: Flatpack on mains, CAN-H/CAN-L (try swapping),"));
      Serial.println(F("   Arduino GND to Flatpack DC minus, CAN_CRYSTAL setting (8 vs 16 MHz), 120R jumper."));
    }
  }

  updateLed(now);
}

// Minimal Arduino stand-in so the sketch can run on a PC for testing.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

typedef uint8_t byte;

#define PROGMEM
class __FlashStringHelper;
#define F(s) ((const __FlashStringHelper*)(s))
inline uint8_t pgm_read_byte(const void* p) { return *(const uint8_t*)p; }
inline const void* pgm_read_ptr_(const void* p) { return *(const void* const*)p; }
#define pgm_read_word(p) (pgm_read_ptr_(p))

#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define HEX 16

// simulated clock
extern unsigned long g_ms;
inline unsigned long millis() { return g_ms; }
inline void delay(unsigned long d) { g_ms += d; }

extern int g_led;
inline void pinMode(int, int) {}
inline void digitalWrite(int, int v) { g_led = v; }
inline int digitalRead(int) { return g_led; }

// Serial: collects every printed line in g_log (and prints it when g_verbose)
extern std::string g_log;
extern bool g_verbose;
struct SerialT {
  std::string line;
  void begin(long) {}
  void write(char c) { line += c; }
  void print(const __FlashStringHelper* s) { line += (const char*)s; }
  void print(const char* s) { line += s; }
  void print(char c) { line += c; }
  void print(double v, int d = 2) { char b[32]; snprintf(b, 32, "%.*f", d, v); line += b; }
  void print(float v, int d = 2) { print((double)v, d); }
  void print(int v, int base = 10) { char b[32]; snprintf(b, 32, base == 16 ? "%X" : "%d", v); line += b; }
  void print(unsigned int v, int base = 10) { print((int)v, base); }
  void print(unsigned char v, int base = 10) { print((int)v, base); }
  void print(long v, int base = 10) { char b[32]; snprintf(b, 32, base == 16 ? "%lX" : "%ld", v); line += b; }
  void print(unsigned long v, int base = 10) { print((long)v, base); }
  template <class T> void println(T v) { print(v); println(); }
  void println() {
    g_log += line + "\n";
    if (g_verbose) printf("  [%8.1fs] %s\n", g_ms / 1000.0, line.c_str());
    line.clear();
  }
};
extern SerialT Serial;

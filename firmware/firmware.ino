// Firmware für das Schwammhirn-Badge: ESP32-C3-Zero auf der Platine aus diesem Repo.
//
// Aufbau:
//   * Ein Timer-Interrupt scannt die 110 Charlieplex-LEDs mit 4 Bit Helligkeit.
//   * loop() rendert Animationen in einen 110-Byte-Framebuffer (0..255 pro LED).
//     Flächige Animationen zeichnen auf eine kleine virtuelle Leinwand, aus der
//     jede LED an ihrer Position auf der Platine abtastet (siehe led_map.h).
//   * Kommandos kommen per Serial oder BLE (Nordic-UART-Service), gleiche Syntax.
//   * Badges in Reichweite synchronisieren Modus, Tempo, Helligkeit und Phase
//     über BLE-Advertising. Ein Tastendruck löst eine Welle auf allen aus.
//
// Kommandos (Serial 115200 oder BLE RX, eine Zeile pro Kommando):
//   mode <0-4|neurons|pulse|text|test|remote>   Modus wählen
//                      test: Löt-Test, füllt jede Zeile LED für LED, dann aus, nächste Zeile;
//                      danach dasselbe mit den Spalten. So läuft jeder GPIO als Kathode und Anode.
//   text <Text>        Laufschrift setzen (ASCII, Umlaute werden ersetzt)
//   name <Name>        BLE-Anzeigename (max. 20 Zeichen), Standard Schwammhirn-<ID>
//   tsize <1-5>        Schriftgröße der Laufschrift (Leinwand-Pixel pro Font-Pixel)
//   ty <0-30>          Abstand der Laufschrift vom oberen Rand in Leinwand-Pixeln
//   speed <0-100>      Tempo
//   bright <1-15>      Helligkeit
//   pulse              Welle auslösen (auch auf Badges in Reichweite); im Löt-Test: Test neu starten
//   frame <220 Hex>    Rohbild, ein Byte pro LED, schaltet auf Modus remote
//   sync <on|off>      Sync mit anderen Badges
//   mirror <on|off>    Framebuffer als "F<hex>" per BLE-Notify streamen (~10 Hz)
//   status             Zustand ausgeben

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <soc/gpio_reg.h>
#include <driver/gpio.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <math.h>

#include "led_map.h"
#include "font5x7.h"

constexpr int MATRIX_PINS[PIN_COUNT] = {5, 6, 4, 7, 3, 8, 2, 9, 1, 0, 20}; // Col0..Col10
constexpr int BUTTON_PIN = 21; // alle drei Taster parallel, aktiv HIGH

// WS2812-Status-LED des C3-Zero an GPIO10. Bewusst nicht RGB_BUILTIN: die generische
// C3-Variante legt das auf GPIO8, und das ist Zeile 5 der LED-Matrix.
constexpr int STATUS_LED = SOC_GPIO_PIN_COUNT + 10;

// Typen stehen vor der ersten Funktion, weil der Arduino-Präprozessor dort seine Prototypen einfügt.
enum Mode : uint8_t { NEURONS, PULSE, TEXT, TEST, REMOTE, MODE_COUNT }; // Taster rotiert bis TEST; ab TEST kein Sync, kein Speichern

struct __attribute__((packed)) SyncPacket
{
  uint8_t company[2]; // 0xFFFF = Testkennung
  uint8_t magic[2];   // 'S','H'
  uint8_t version;
  uint16_t id;
  uint16_t generation;
  uint8_t mode, speed, brightness;
  uint16_t phase;     // animTime() & 0xFFFF
  uint8_t pulseSeq, pulseOrigin;
  uint8_t textScale, textY;
};

constexpr bool pinInMatrix(int gpio)
{
  for (int p : MATRIX_PINS)
    if (p == gpio) return true;
  return false;
}
static_assert(!pinInMatrix(STATUS_LED - SOC_GPIO_PIN_COUNT), "Status-LED liegt auf einem Matrix-Pin");
static_assert(!pinInMatrix(BUTTON_PIN), "Taster liegt auf einem Matrix-Pin");

// ---------------------------------------------------------------------------
// Charlieplex-Scan (Timer-ISR). 4 Bitebenen pro Spalte, Bitebene b dauert 2^b Ticks.
// ---------------------------------------------------------------------------
constexpr int PLANES = 4;
constexpr uint32_t TICK_US = 30; // 11 Spalten * 15 Ticks * 30 us = 5 ms => ~200 Hz Bildwiederholung

uint32_t pinMask[PIN_COUNT];
uint32_t allPinsMask = 0;
volatile uint32_t rowMask[2][PIN_COUNT][PLANES]; // [Puffer][Spalte][Bitebene] -> Kathoden-Pins
volatile uint8_t activeBuf = 0;
hw_timer_t* scanTimer = nullptr;

void ARDUINO_ISR_ATTR onScanTick()
{
  static uint8_t col = 0, plane = 0, remaining = 1;
  if (--remaining) return;

  REG_WRITE(GPIO_ENABLE_W1TC_REG, allPinsMask); // alles hochohmig
  uint32_t rows = rowMask[activeBuf][col][plane];
  if (rows)
  {
    REG_WRITE(GPIO_OUT_W1TS_REG, pinMask[col]);
    REG_WRITE(GPIO_OUT_W1TC_REG, rows);
    REG_WRITE(GPIO_ENABLE_W1TS_REG, pinMask[col] | rows);
  }

  remaining = 1 << plane;
  if (++plane == PLANES)
  {
    plane = 0;
    if (++col == PIN_COUNT) col = 0;
  }
}

// ---------------------------------------------------------------------------
// Zustand
// ---------------------------------------------------------------------------
const char* const MODE_NAMES[MODE_COUNT] = {"neurons", "pulse", "text", "test", "remote"};

struct State
{
  Mode mode = NEURONS;
  uint8_t speed = 4;      // 0..100
  uint8_t brightness = 15; // 1..15
  char text[64] = "SCHWAMMHIRN";
  char name[21] = "";     // BLE-Anzeigename, leer = Standard aus myId
  uint8_t textScale = 3;  // 1..5
  uint8_t textY = 6;      // 0..30
  bool syncEnabled = true;
  bool mirror = false;
} st;

uint8_t fb[LED_COUNT];      // Framebuffer 0..255
uint32_t epoch = 0;         // Animationszeit t = millis() - epoch, per Sync verschiebbar
uint16_t generation = 0;    // Lamport-Zähler für den Sync: höchste Generation gewinnt
uint16_t myId;
uint8_t pulseSeq = 0;       // zählt eigene Pulse, damit Nachbarn neue erkennen
uint8_t pulseOrigin = 0;
uint32_t dirtySince = 0;    // Zeitpunkt der letzten Änderung, für verzögertes Speichern
bool advertDirty = true;

Preferences prefs;

inline uint32_t animTime() { return millis() - epoch; }

// ---------------------------------------------------------------------------
// Framebuffer -> Scan-Puffer
// ---------------------------------------------------------------------------
void show()
{
  uint8_t next = activeBuf ^ 1;
  for (int c = 0; c < PIN_COUNT; ++c)
    for (int p = 0; p < PLANES; ++p) rowMask[next][c][p] = 0;

  for (int i = 0; i < LED_COUNT; ++i)
  {
    uint8_t v = (fb[i] * (st.brightness + 1)) >> 8; // 0..brightness
    for (int p = 0; p < PLANES; ++p)
      if (v & (1 << p)) rowMask[next][LEDS[i].col][p] |= pinMask[LEDS[i].row];
  }
  activeBuf = next;
}

// ---------------------------------------------------------------------------
// Virtuelle Leinwand, ca. 2 mm pro Pixel
// ---------------------------------------------------------------------------
constexpr int CW = 48;
constexpr int CH = 40;
uint8_t canvas[CW * CH];

void canvasClear() { memset(canvas, 0, sizeof(canvas)); }

inline void canvasSet(int x, int y, uint8_t v)
{
  if (x < 0 || y < 0 || x >= CW || y >= CH) return;
  uint8_t& px = canvas[y * CW + x];
  if (v > px) px = v;
}


// Jede LED nimmt das Maximum ihrer Nachbarschaft (radius 1 = 3x3), damit dünne Linien nicht durchfallen.
void sampleCanvas(int radius)
{
  for (int i = 0; i < LED_COUNT; ++i)
  {
    int cx = LEDS[i].x * CW / 256, cy = LEDS[i].y * CH / 256;
    uint8_t m = 0;
    for (int dy = -radius; dy <= radius; ++dy)
      for (int dx = -radius; dx <= radius; ++dx)
      {
        int x = cx + dx, y = cy + dy;
        if (x >= 0 && y >= 0 && x < CW && y < CH) m = max(m, canvas[y * CW + x]);
      }
    fb[i] = m;
  }
}

// ---------------------------------------------------------------------------
// Wellen (Overlay in jedem Modus)
// ---------------------------------------------------------------------------
struct Wave { uint32_t start; uint8_t origin; bool active; };
Wave waves[4];

void triggerWave(uint8_t origin)
{
  for (Wave& w : waves)
    if (!w.active)
    {
      w = {millis(), origin, true};
      return;
    }
  waves[0] = {millis(), origin, true};
}

void renderWaves()
{
  for (Wave& w : waves)
  {
    if (!w.active) continue;
    uint32_t age = millis() - w.start;
    if (age > 1800) { w.active = false; continue; }
    float r = age * 0.25f;                     // Radius in LED-Koordinaten (0..255), ~1 Sekunde bis zum Rand
    float fade = 1.0f - age / 1800.0f;
    int ox = LEDS[w.origin].x, oy = LEDS[w.origin].y;
    for (int i = 0; i < LED_COUNT; ++i)
    {
      float d = hypotf(LEDS[i].x - ox, LEDS[i].y - oy);
      float k = 1.0f - fabsf(d - r) / 30.0f;   // Ringbreite 30
      if (k <= 0) continue;
      uint8_t v = (uint8_t)(255 * k * fade);
      if (v > fb[i]) fb[i] = v;
    }
  }
}

// ---------------------------------------------------------------------------
// Animationen
// ---------------------------------------------------------------------------
void renderNeurons(uint32_t dt)
{
  uint8_t decay = 2 + dt / 4;
  for (int i = 0; i < LED_COUNT; ++i) fb[i] = fb[i] > decay ? fb[i] - decay : 0;
  int flashes = 1 + st.speed / 25;
  for (int n = 0; n < flashes; ++n)
    if (random(100) < 20 + st.speed / 2) fb[random(LED_COUNT)] = 255;
}

void renderPulseMode(uint32_t t)
{
  static uint32_t nextAt = 0;
  memset(fb, 0, sizeof(fb));
  if (millis() >= nextAt)
  {
    // ponytail: Ursprung aus t abgeleitet, damit synchronisierte Badges denselben wählen
    triggerWave((t / 100) % LED_COUNT);
    nextAt = millis() + 2500 - st.speed * 20;
  }
}


void renderText(uint32_t t)
{
  canvasClear();
  const int SCALE = st.textScale, GAP = SCALE, ADV = FONT_W * SCALE + GAP;
  int n = strlen(st.text);
  if (n == 0) { sampleCanvas(0); return; }
  int total = n * ADV + CW;
  int offset = (int)((t * (10 + st.speed) / 400) % total);
  int y0 = st.textY;
  for (int ci = 0; ci < n; ++ci)
  {
    int x0 = CW + ci * ADV - offset;
    if (x0 + FONT_W * SCALE < 0 || x0 >= CW) continue;
    uint8_t ch = st.text[ci];
    if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
    const uint8_t* glyph = FONT5X7[ch - FONT_FIRST];
    for (int gx = 0; gx < FONT_W; ++gx)
      for (int gy = 0; gy < FONT_H; ++gy)
        if (glyph[gx] & (1 << gy))
          for (int dx = 0; dx < SCALE; ++dx)
            for (int dy = 0; dy < SCALE; ++dy) canvasSet(x0 + gx * SCALE + dx, y0 + gy * SCALE + dy, 255);
  }
  sampleCanvas(SCALE >= 3 ? 0 : 1); // breite Striche ohne Nachbarschaft, sonst bleiben Buchstaben Blobs
}

// Löt-Test: Gruppe 0..10 = Zeilen, 11..21 = Spalten. Je Gruppe 10 Schritte zum Auffüllen,
// dann ein Schritt dunkel. Meldet jede neu hinzukommende LED.
int testLast = -1; // -1 = Test nicht aktiv, sonst zuletzt gemeldeter Schritt
constexpr int TEST_GROUPS = 2 * PIN_COUNT, TEST_STEPS = PIN_COUNT; // 10 LEDs + 1 Pause
void renderTest()
{
  static uint32_t startedAt = 0;
  if (testLast < 0) startedAt = millis();
  uint32_t stepMs = 150 - st.speed; // 146 ms bei Tempo 4, 50 ms bei Tempo 100
  int step = ((millis() - startedAt) / stepMs) % (TEST_GROUPS * TEST_STEPS);
  int group = step / TEST_STEPS, k = step % TEST_STEPS;
  bool byRow = group < PIN_COUNT;
  int pin = byRow ? group : group - PIN_COUNT;
  memset(fb, 0, sizeof(fb));
  int added = -1;
  if (k < TEST_STEPS - 1)
  {
    int n = 0;
    for (int other = 0; other < PIN_COUNT && n <= k; ++other) // Partner-Pins aufsteigend
      for (int i = 0; i < LED_COUNT; ++i)
        if ((byRow ? LEDS[i].row == pin && LEDS[i].col == other : LEDS[i].col == pin && LEDS[i].row == other))
        {
          fb[i] = 255;
          if (n == k) added = i;
          ++n;
        }
  }
  if (step != testLast)
  {
    testLast = step;
    String where = String(byRow ? "row" : "col") + pin + " (GPIO" + MATRIX_PINS[pin] + (byRow ? " low)" : " high)");
    if (added < 0) reply("test " + where + " fertig");
    else reply("test " + where + " +D" + (added + 1) + " " + (byRow ? "col" : "row") + (byRow ? LEDS[added].col : LEDS[added].row));
  }
}

void renderFrame()
{
  static uint32_t last = 0;
  uint32_t now = millis(), dt = now - last;
  last = now;
  uint32_t t = animTime();
  switch (st.mode)
  {
    case NEURONS: renderNeurons(dt); break;
    case PULSE: renderPulseMode(t); break;
    case TEXT: renderText(t); break;
    case REMOTE: break; // fb kommt per "frame"-Kommando
    case TEST: renderTest(); break;
    default: break;
  }
  if (st.mode != TEST) testLast = -1;
  if (st.mode != TEST) renderWaves();
  show();
}

// ---------------------------------------------------------------------------
// Sync über BLE-Advertising (Manufacturer Data)
// ---------------------------------------------------------------------------

struct Peer { uint16_t id; uint8_t pulseSeq; uint32_t lastSeen; };
Peer peers[16];

NimBLECharacteristic* txChar = nullptr;
NimBLEAdvertising* adv = nullptr;

void bumpGeneration() { ++generation; advertDirty = true; }

void markDirty()
{
  dirtySince = millis();
  bumpGeneration();
}

void updateAdvert()
{
  SyncPacket p = {{0xFF, 0xFF}, {'S', 'H'}, 1, myId, generation, st.mode, st.speed, st.brightness,
                  (uint16_t)animTime(), pulseSeq, pulseOrigin, st.textScale, st.textY};
  adv->setManufacturerData((const uint8_t*)&p, sizeof(p));
  if (adv->isAdvertising()) adv->refreshAdvertisingData();
  advertDirty = false;
}

int peerCount()
{
  int n = 0;
  for (Peer& p : peers) if (p.id && millis() - p.lastSeen < 10000) ++n;
  return n;
}

void onSyncPacket(const SyncPacket& p)
{
  Peer* peer = nullptr;
  Peer* slot = nullptr;
  for (Peer& q : peers)
  {
    if (q.id == p.id) peer = &q;
    if (!slot && (q.id == 0 || millis() - q.lastSeen > 10000)) slot = &q;
  }
  bool known = peer != nullptr;
  if (!peer) peer = slot ? slot : &peers[0];
  if (known && peer->pulseSeq != p.pulseSeq) triggerWave(p.pulseOrigin < LED_COUNT ? p.pulseOrigin : 0);
  *peer = {p.id, p.pulseSeq, millis()};

  if (!st.syncEnabled || p.mode >= TEST || st.mode >= TEST) return;
  bool newer = p.generation > generation || (p.generation == generation && p.id < myId);
  if (!newer) return;
  int16_t drift = (int16_t)(p.phase - (uint16_t)animTime());
  if (abs(drift) > 40) epoch -= drift;
  if (p.generation != generation || p.mode != st.mode || p.speed != st.speed || p.brightness != st.brightness ||
      p.textScale != st.textScale || p.textY != st.textY)
  {
    generation = p.generation;
    st.mode = (Mode)min<uint8_t>(p.mode, TEXT);
    st.speed = min<uint8_t>(p.speed, 100);
    st.brightness = constrain(p.brightness, 1, 15);
    st.textScale = constrain(p.textScale, 1, 5);
    st.textY = min<uint8_t>(p.textY, 30);
    dirtySince = millis();
    advertDirty = true;
  }
}

class ScanCallbacks : public NimBLEScanCallbacks
{
  void onResult(const NimBLEAdvertisedDevice* dev) override
  {
    if (!dev->haveManufacturerData()) return;
    std::string d = dev->getManufacturerData();
    if (d.size() != sizeof(SyncPacket)) return;
    SyncPacket p;
    memcpy(&p, d.data(), sizeof(p));
    if (p.company[0] != 0xFF || p.company[1] != 0xFF || p.magic[0] != 'S' || p.magic[1] != 'H' || p.id == myId) return;
    onSyncPacket(p);
  }
} scanCallbacks;

// ---------------------------------------------------------------------------
// Kommandos
// ---------------------------------------------------------------------------
void reply(const String& s)
{
  Serial.println(s);
  if (txChar && NimBLEDevice::getServer()->getConnectedCount()) txChar->notify((const uint8_t*)s.c_str(), s.length());
}

void setMode(Mode m)
{
  st.mode = m;
  markDirty();
}

void applyName() // nach setupBle() aufrufen
{
  if (!st.name[0]) snprintf(st.name, sizeof(st.name), "Schwammhirn-%04X", myId);
  NimBLEDevice::setDeviceName(st.name);
  adv->setName(st.name);
  if (adv->isAdvertising()) adv->refreshAdvertisingData();
}

void setText(const char* src)
{
  // UTF-8-Umlaute auf ASCII abbilden, Kleinbuchstaben hochsetzen
  int o = 0;
  for (const uint8_t* s = (const uint8_t*)src; *s && o < (int)sizeof(st.text) - 1; ++s)
  {
    uint8_t c = *s;
    if (c == 0xC3 && s[1])
    {
      uint8_t u = *++s;
      c = (u == 0x84 || u == 0xA4) ? 'A' : (u == 0x96 || u == 0xB6) ? 'O' : (u == 0x9C || u == 0xBC) ? 'U' : (u == 0x9F) ? 'S' : '?';
      if (u == 0x9F && o < (int)sizeof(st.text) - 2) st.text[o++] = 'S';
    }
    else if (c >= 0x80) c = '?';
    st.text[o++] = toupper(c);
  }
  st.text[o] = 0;
}

int hexVal(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  c = tolower(c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

void handleCommand(String line)
{
  line.trim();
  if (line.isEmpty()) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  cmd.toLowerCase();

  if (cmd == "mode")
  {
    int m = -1;
    for (int i = 0; i < MODE_COUNT; ++i) if (arg.equalsIgnoreCase(MODE_NAMES[i])) m = i;
    if (m < 0 && arg.length() && isDigit(arg[0])) m = arg.toInt();
    if (m < 0 || m >= MODE_COUNT) { reply("err mode"); return; }
    setMode((Mode)m);
    reply(String("ok mode ") + MODE_NAMES[m]);
  }
  else if (cmd == "text")
  {
    setText(arg.c_str());
    if (st.mode != TEXT) st.mode = TEXT;
    markDirty();
    reply(String("ok text ") + st.text);
  }
  else if (cmd == "name")
  {
    arg.trim();
    int o = 0;
    for (unsigned i = 0; i < arg.length() && o < (int)sizeof(st.name) - 1; ++i)
      if (arg[i] >= 0x20 && arg[i] < 0x7F) st.name[o++] = arg[i];
    st.name[o] = 0;
    applyName();
    dirtySince = millis();
    reply(String("ok name ") + st.name);
  }
  else if (cmd == "speed")
  {
    st.speed = constrain(arg.toInt(), 0, 100);
    markDirty();
    reply(String("ok speed ") + st.speed);
  }
  else if (cmd == "bright")
  {
    st.brightness = constrain(arg.toInt(), 1, 15);
    markDirty();
    reply(String("ok bright ") + st.brightness);
  }
  else if (cmd == "tsize")
  {
    st.textScale = constrain(arg.toInt(), 1, 5);
    st.mode = TEXT;
    markDirty();
    reply(String("ok tsize ") + st.textScale);
  }
  else if (cmd == "ty")
  {
    st.textY = constrain(arg.toInt(), 0, 30);
    st.mode = TEXT;
    markDirty();
    reply(String("ok ty ") + st.textY);
  }
  else if (cmd == "pulse")
  {
    if (st.mode == TEST) // im Löt-Test startet ein Puls (Taster kurz, Web-Button) den Durchlauf neu
    {
      testLast = -1;
      reply("ok test restart");
      return;
    }
    pulseOrigin = arg.length() ? constrain(arg.toInt(), 0, LED_COUNT - 1) : random(LED_COUNT);
    ++pulseSeq;
    advertDirty = true;
    triggerWave(pulseOrigin);
    reply("ok pulse");
  }
  else if (cmd == "frame")
  {
    if ((int)arg.length() != LED_COUNT * 2) { reply("err frame needs 220 hex chars"); return; }
    for (int i = 0; i < LED_COUNT; ++i)
    {
      int hi = hexVal(arg[2 * i]), lo = hexVal(arg[2 * i + 1]);
      if (hi < 0 || lo < 0) { reply("err frame hex"); return; }
      fb[i] = hi * 16 + lo;
    }
    st.mode = REMOTE;
    // kein markDirty: Rohbilder werden weder gespeichert noch an Nachbarn gesendet
  }
  else if (cmd == "sync")
  {
    st.syncEnabled = arg != "off" && arg != "0";
    reply(String("ok sync ") + (st.syncEnabled ? "on" : "off"));
  }
  else if (cmd == "mirror")
  {
    st.mirror = arg != "off" && arg != "0";
    reply(String("ok mirror ") + (st.mirror ? "on" : "off"));
  }
  else if (cmd == "status")
  {
    reply(String("mode=") + MODE_NAMES[st.mode] + " speed=" + st.speed + " bright=" + st.brightness + " tsize=" + st.textScale +
          " ty=" + st.textY + " text=" + st.text + " sync=" + (st.syncEnabled ? "on" : "off") + " gen=" + generation + " id=" + String(myId, HEX) + " peers=" + peerCount() + " name=" + st.name);
  }
  else reply("err unknown: " + cmd);
}

class RxCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override
  {
    std::string v = c->getValue();
    handleCommand(String(v.c_str()));
  }
} rxCallbacks;

class ServerCallbacks : public NimBLEServerCallbacks
{
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override { rgbLedWrite(STATUS_LED, 0, 0, 16); }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override
  {
    st.mirror = false;
    rgbLedWrite(STATUS_LED, 0, 0, 0);
  }
} serverCallbacks;

void sendMirror()
{
  static uint32_t last = 0;
  if (!st.mirror || millis() - last < 100 || !NimBLEDevice::getServer()->getConnectedCount()) return;
  last = millis();
  static char buf[1 + LED_COUNT * 2];
  buf[0] = 'F';
  for (int i = 0; i < LED_COUNT; ++i)
  {
    buf[1 + 2 * i] = "0123456789abcdef"[fb[i] >> 4];
    buf[2 + 2 * i] = "0123456789abcdef"[fb[i] & 15];
  }
  txChar->notify((const uint8_t*)buf, sizeof(buf));
}

// ---------------------------------------------------------------------------
// Taster: kurz = Welle (lokal und an Nachbarn), lang = nächster Modus
// ---------------------------------------------------------------------------
void handleButton()
{
  static bool last = false, longFired = false;
  static uint32_t pressedAt = 0, releasedAt = 0;
  bool now = digitalRead(BUTTON_PIN);
  if (now && !last && millis() - releasedAt > 30)
  {
    pressedAt = millis();
    longFired = false;
  }
  if (now && !longFired && millis() - pressedAt > 700)
  {
    longFired = true;
    setMode((Mode)((st.mode + 1) % REMOTE)); // remote nur per Kommando, test ist Teil der Rotation
    reply(String("button: mode ") + MODE_NAMES[st.mode]);
  }
  if (!now && last)
  {
    releasedAt = millis();
    if (!longFired) handleCommand("pulse");
  }
  last = now;
}

// ---------------------------------------------------------------------------
// Persistenz
// ---------------------------------------------------------------------------
void loadSettings()
{
  prefs.begin("hirn");
  st.mode = (Mode)prefs.getUChar("mode", NEURONS);
  if (st.mode >= TEST) st.mode = NEURONS;
  st.speed = prefs.getUChar("speed", 4);
  st.brightness = constrain(prefs.getUChar("bright", 15), 1, 15);
  prefs.getString("text", st.text, sizeof(st.text));
  st.textScale = constrain(prefs.getUChar("tsize", 3), 1, 5);
  st.textY = min<uint8_t>(prefs.getUChar("ty", 6), 30);
  prefs.getString("name", st.name, sizeof(st.name));
}

void saveSettingsIfDue()
{
  if (!dirtySince || millis() - dirtySince < 2000) return;
  dirtySince = 0;
  prefs.putUChar("mode", st.mode >= TEST ? NEURONS : st.mode);
  prefs.putUChar("speed", st.speed);
  prefs.putUChar("bright", st.brightness);
  prefs.putString("text", st.text);
  prefs.putUChar("tsize", st.textScale);
  prefs.putUChar("ty", st.textY);
  prefs.putString("name", st.name);
}

// ---------------------------------------------------------------------------
void setupBle()
{
  if (!st.name[0]) snprintf(st.name, sizeof(st.name), "Schwammhirn-%04X", myId);
  NimBLEDevice::init(st.name);
  NimBLEDevice::setPower(9); // dBm
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks);
  server->advertiseOnDisconnect(true);

  NimBLEService* svc = server->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"); // Nordic UART Service
  NimBLECharacteristic* rx = svc->createCharacteristic("6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
                                                       NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(&rxCallbacks);
  txChar = svc->createCharacteristic("6E400003-B5A3-F393-E0A9-E50E24DCCA9E", NIMBLE_PROPERTY::NOTIFY);
  svc->start(); // ponytail: in NimBLE 2.x wirkungslos, bleibt für ältere Versionen (Arduino IDE) stehen

  adv = NimBLEDevice::getAdvertising();
  adv->enableScanResponse(true);
  adv->setName(st.name);
  adv->setMinInterval(320); // 200 ms
  adv->setMaxInterval(480); // 300 ms
  updateAdvert();
  adv->start();

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks);
  scan->setActiveScan(false);
  scan->setDuplicateFilter(0);
  scan->setMaxResults(0);
  scan->setInterval(100);
  scan->setWindow(40);
  scan->start(0);
}

void setup()
{
  Serial.begin(115200);
  for (int i = 0; i < PIN_COUNT; ++i)
  {
    pinMode(MATRIX_PINS[i], INPUT);
    gpio_set_drive_capability((gpio_num_t)MATRIX_PINS[i], GPIO_DRIVE_CAP_3);
    pinMask[i] = 1u << MATRIX_PINS[i];
    allPinsMask |= pinMask[i];
  }
  pinMode(BUTTON_PIN, INPUT_PULLDOWN); // auf dem nackten Dev-Board fehlt R12, sonst floatet der Eingang

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  myId = (mac[4] << 8) | mac[5];
  if (myId == 0) myId = 1;
  randomSeed(esp_random());

  loadSettings();
  memset(fb, 0, sizeof(fb));
  show();

  scanTimer = timerBegin(1000000);
  timerAttachInterrupt(scanTimer, &onScanTick);
  timerAlarm(scanTimer, TICK_US, true, 0);

  setupBle();
  rgbLedWrite(STATUS_LED, 0, 16, 0);
  delay(200);
  rgbLedWrite(STATUS_LED, 0, 0, 0);
  handleCommand("status");
}

void loop()
{
  static String serialLine;
  while (Serial.available())
  {
    char c = Serial.read();
    if (c == '\n' || c == '\r')
    {
      handleCommand(serialLine);
      serialLine = "";
    }
    else if (serialLine.length() < 300) serialLine += c;
  }

  handleButton();
  renderFrame();
  sendMirror();
  saveSettingsIfDue();

  static uint32_t lastAdvert = 0;
  if (advertDirty || millis() - lastAdvert > 500)
  {
    lastAdvert = millis();
    updateAdvert();
  }
  delay(16);
}

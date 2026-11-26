// Smart EV charging / V2G scheduler demo (3 EVs, 12 one-hour slots, 18:00-05:00)
#include <Arduino.h>

// ---- Pins (NodeMCU ESP8266 labels; all five are safe at boot) ----
constexpr uint8_t GREEN_LED_PIN = D1;  // ON while any EV is charging (G2V)
constexpr uint8_t RED_LED_PIN = D2;   // ON while any EV is discharging (V2G)
constexpr uint8_t SW_V1_PIN = D5;      // switches pull to GND: LOW = plugged in
constexpr uint8_t SW_V2_PIN = D6;
constexpr uint8_t SW_V3_PIN = D7;

// ---- Simulation settings ----
constexpr uint32_t SLOT_INTERVAL_MS = 5000;  // one simulated hour
constexpr uint32_t ARRIVE_MS = 6000;         // show starting batteries
constexpr uint32_t RESULT_MS = 12000;        // hold the morning result
constexpr int TOTAL_SLOTS = 12;
constexpr int PEAK_SLOTS = 5;  // slots 0-4 (18:00-22:00)
constexpr float PEAK_PRICE = 10.0f;
constexpr float OFF_PEAK_PRICE = 4.0f;
constexpr int CHARGE_STEP = 10;    // % SoC per slot
constexpr int DISCHARGE_STEP = 8;  // % SoC per slot
constexpr int GRID_MAX_CHARGING = 2;
constexpr int NUM_EVS = 3;
constexpr uint32_t DEBOUNCE_MS = 50;

enum Mode { IDLE, CHARGE, DISCHARGE };
const char* const MODE_NAME[] = {"IDLE", "CHARGE", "DISCHARGE"};

struct EvConfig {
  const char* name;
  uint8_t switchPin;
  int initialSoc;
  int targetSoc;
  int reserveSoc;
  int departSlot;  // EV leaves at the start of this slot (12 = end of cycle)
  bool v2gAllowed;
};

// Edit these to try other scenarios.
const EvConfig CONFIG[NUM_EVS] = {
    // name  pin        soc tgt res depart v2g
    {"V1", SW_V1_PIN, 90, 80, 60, 12, true},  // full battery: feeds peak load
    {"V2", SW_V2_PIN, 20, 90, 60, 7, true},   // leaves 01:00: urgent at once
    {"V3", SW_V3_PIN, 30, 80, 60, 12, true},  // can wait for cheap hours
};

struct EvState {
  int soc;
  Mode mode;
  int modeChanges;
};

EvState ev[NUM_EVS];
// Each night: cars arrive, 12 hours run, then the morning result is held.
enum Phase { ARRIVING, RUNNING, MORNING };
const char* const PHASE_NAME[] = {"arrive", "night", "morning"};
Phase phase = ARRIVING;
int change[NUM_EVS];  // battery change in the current hour

int currentSlot = 0;
uint32_t slotStartMs = 0;
float smartCost = 0;
float peakOnlyCost = 0;

// Debounced switch state. All logic reads this, not the pins.
bool connected[NUM_EVS];
bool rawState[NUM_EVS];
uint32_t rawChangedMs[NUM_EVS];
bool deferred[NUM_EVS];
bool urgent[NUM_EVS];
char lastEvent[NUM_EVS][48];  // latest plug / unplug message per EV

// Result of the previous cycle, kept on screen during the next one.
struct {
  bool valid;
  int soc[NUM_EVS];
  int changes[NUM_EVS];
  float smart, peakOnly;
} lastCycle;

bool readSwitch(int i) { return digitalRead(CONFIG[i].switchPin) == LOW; }

// Number of charging slots needed to get from soc to target (rounded up).
int slotsNeeded(int soc, int target) {
  return soc >= target ? 0 : (target - soc + CHARGE_STEP - 1) / CHARGE_STEP;
}

void setMode(int i, Mode mode) {
  if (ev[i].mode != mode) {
    ev[i].mode = mode;
    ev[i].modeChanges++;
  }
}

void updateLeds() {
  bool charging = false;
  bool discharging = false;
  for (int i = 0; i < NUM_EVS; i++) {
    charging |= ev[i].mode == CHARGE;
    discharging |= ev[i].mode == DISCHARGE;
  }
  digitalWrite(GREEN_LED_PIN, charging ? HIGH : LOW);
  digitalWrite(RED_LED_PIN, discharging ? HIGH : LOW);
}

// Why an EV is in its current state.
const char* noteFor(int i) {
  if (!connected[i]) return "unplugged";
  if (currentSlot >= CONFIG[i].departSlot) return "departed";
  if (deferred[i]) return "deferred: grid limit";
  if (urgent[i] && ev[i].mode == CHARGE) return "urgent";
  return "";
}

uint32_t phaseLength() {
  return phase == ARRIVING ? ARRIVE_MS
         : phase == MORNING ? RESULT_MS
                            : SLOT_INTERVAL_MS;
}

// One-line JSON snapshot of everything, prefixed with '@' for the dashboard.
void sendSnapshot(const char* type) {
  Serial.printf(
      "@{\"type\":\"%s\",\"phase\":\"%s\",\"wait\":%lu,\"slot\":%d,\"hour\":%d,\"peak\":%d,\"price\":%.1f,"
      "\"smart\":%.1f,\"peakOnly\":%.1f,\"green\":%d,\"red\":%d,\"ev\":[",
      type, PHASE_NAME[phase], (unsigned long)phaseLength(), currentSlot, (18 + currentSlot) % 24, currentSlot < PEAK_SLOTS,
      currentSlot < PEAK_SLOTS ? PEAK_PRICE : OFF_PEAK_PRICE, smartCost,
      peakOnlyCost, digitalRead(GREEN_LED_PIN), digitalRead(RED_LED_PIN));
  for (int i = 0; i < NUM_EVS; i++) {
    Serial.printf(
        "%s{\"name\":\"%s\",\"plugged\":%d,\"mode\":\"%s\",\"soc\":%d,\"change\":%d,"
        "\"target\":%d,\"reserve\":%d,\"depart\":%d,\"changes\":%d,"
        "\"note\":\"%s\"}",
        i ? "," : "", CONFIG[i].name, connected[i], MODE_NAME[ev[i].mode],
        ev[i].soc, change[i], CONFIG[i].targetSoc, CONFIG[i].reserveSoc,
        CONFIG[i].departSlot, ev[i].modeChanges, noteFor(i));
  }
  Serial.println("]}");
}

// Redraw the whole serial screen in place (ANSI: clear screen, cursor home).
// Printed after the '@' snapshot so that the long data line is wiped from view.
void drawScreen() {
  const bool peak = currentSlot < PEAK_SLOTS;
  int chargers = 0;
  for (int i = 0; i < NUM_EVS; i++) chargers += ev[i].mode == CHARGE;

  Serial.print("\033[2J\033[3J\033[H");
  Serial.println("===== EV CHARGING STATION =====");
  if (phase == ARRIVING) {
    Serial.println("18:00  Cars have arrived. These are their starting batteries.");
  } else if (phase == MORNING) {
    Serial.println("MORNING  Night over. Next: a new evening, batteries start again.");
  } else {
    Serial.printf("%02d:00  Electricity is %s (Rs %.0f per unit)\n",
                  (18 + currentSlot) % 24, peak ? "EXPENSIVE" : "CHEAP",
                  peak ? PEAK_PRICE : OFF_PEAK_PRICE);
  }
  Serial.printf("Chargers in use %d of %d   Green LED %s   Red LED %s\n\n",
                chargers, GRID_MAX_CHARGING,
                digitalRead(GREEN_LED_PIN) ? "ON" : "OFF",
                digitalRead(RED_LED_PIN) ? "ON" : "OFF");
  Serial.println("Car  Plug       Doing     Battery  This hour  Goal");
  for (int i = 0; i < NUM_EVS; i++) {
    const char* doing = ev[i].mode == CHARGE      ? "CHARGING"
                        : ev[i].mode == DISCHARGE ? "SELLING"
                        : currentSlot >= CONFIG[i].departSlot ? "LEFT"
                                                              : "WAITING";
    Serial.printf("%s   %-9s  %-8s  %5d%%  %+8d%%  %3d%%\n", CONFIG[i].name,
                  connected[i] ? "PLUGGED" : "UNPLUGGED", doing, ev[i].soc,
                  change[i], CONFIG[i].targetSoc);
  }
  if (phase != ARRIVING)
    Serial.printf("\nSaved so far: Rs %.0f\n", peakOnlyCost - smartCost);
  Serial.println("\nLast plug event:");
  for (int i = 0; i < NUM_EVS; i++)
    Serial.printf("  %s  %s\n", CONFIG[i].name, lastEvent[i]);

  if (!lastCycle.valid) return;
  Serial.println("\nLast completed cycle:");
  for (int i = 0; i < NUM_EVS; i++) {
    Serial.printf("  %s  SoC %3d%% (target %d%%, %s)  mode changes: %d\n",
                  CONFIG[i].name, lastCycle.soc[i], CONFIG[i].targetSoc,
                  lastCycle.soc[i] >= CONFIG[i].targetSoc ? "met" : "NOT met",
                  lastCycle.changes[i]);
  }
  Serial.printf("  Smart Rs %.1f   Peak-only Rs %.1f   Saved Rs %.1f\n",
                lastCycle.smart, lastCycle.peakOnly,
                lastCycle.peakOnly - lastCycle.smart);
}

// Apply the priority rules to one EV. `urgent` is set for must-charge requests.
Mode decide(int i, int slot, bool& mustCharge) {
  const EvConfig& c = CONFIG[i];
  const int soc = ev[i].soc;
  const int remaining = c.departSlot - slot;  // includes this slot
  mustCharge = false;

  // 1. Unplugged, or already driven away
  if (!connected[i] || remaining <= 0) return IDLE;

  // 2. Must charge now to reach the target before departure
  if (soc < c.targetSoc && remaining <= slotsNeeded(soc, c.targetSoc)) {
    mustCharge = true;
    return CHARGE;
  }

  const bool peak = slot < PEAK_SLOTS;

  // 3. V2G: discharge at peak only if there is still time to recharge later
  if (peak && c.v2gAllowed && soc > c.reserveSoc) {
    const int socAfter = max(soc - DISCHARGE_STEP, c.reserveSoc);
    if (slotsNeeded(socAfter, c.targetSoc) <= remaining - 1) return DISCHARGE;
  }

  // 4. Cheap off-peak charging
  if (!peak && soc < c.targetSoc) return CHARGE;

  // 5. Wait / full
  return IDLE;
}

// True if EV a should get a charger before EV b.
bool moreUrgent(int a, int b) {
  if (urgent[a] != urgent[b]) return urgent[a];
  if (CONFIG[a].departSlot != CONFIG[b].departSlot)
    return CONFIG[a].departSlot < CONFIG[b].departSlot;
  return ev[a].soc < ev[b].soc;
}

void runSlot(int slot) {
  const bool peak = slot < PEAK_SLOTS;
  const float price = peak ? PEAK_PRICE : OFF_PEAK_PRICE;

  Mode next[NUM_EVS];
  for (int i = 0; i < NUM_EVS; i++) next[i] = decide(i, slot, urgent[i]);

  // Grid limit: defer any request ranked below the first GRID_MAX_CHARGING.
  for (int i = 0; i < NUM_EVS; i++) {
    deferred[i] = false;
    if (next[i] != CHARGE) continue;
    int ahead = 0;
    for (int j = 0; j < NUM_EVS; j++) {
      if (j == i || next[j] != CHARGE) continue;
      // Equal ranking falls back to the lower EV number.
      if (moreUrgent(j, i) || (!moreUrgent(i, j) && j < i))
        ahead++;
    }
    deferred[i] = ahead >= GRID_MAX_CHARGING;
  }

  for (int i = 0; i < NUM_EVS; i++) {
    const Mode mode = deferred[i] ? IDLE : next[i];
    setMode(i, mode);
    const int before = ev[i].soc;

    if (mode == CHARGE) {
      ev[i].soc = min(ev[i].soc + CHARGE_STEP, 100);
      smartCost += price;
      peakOnlyCost += PEAK_PRICE;
    } else if (mode == DISCHARGE) {
      ev[i].soc = max(ev[i].soc - DISCHARGE_STEP, CONFIG[i].reserveSoc);
      smartCost -= PEAK_PRICE;  // V2G credit
    }
    change[i] = ev[i].soc - before;

  }
  updateLeds();
  sendSnapshot("slot");
  drawScreen();
}

void resetCycle() {
  for (int i = 0; i < NUM_EVS; i++) {
    ev[i] = {CONFIG[i].initialSoc, IDLE, 0};
    deferred[i] = urgent[i] = false;
    change[i] = 0;
  }
  currentSlot = 0;
  smartCost = 0;
  peakOnlyCost = 0;
}

void saveCycle() {
  lastCycle.valid = true;
  for (int i = 0; i < NUM_EVS; i++) {
    lastCycle.soc[i] = ev[i].soc;
    lastCycle.changes[i] = ev[i].modeChanges;
  }
  lastCycle.smart = smartCost;
  lastCycle.peakOnly = peakOnlyCost;
}

// Cars arrive: reset batteries and show them before anything happens.
void startNight() {
  resetCycle();
  phase = ARRIVING;
  updateLeds();
  sendSnapshot("arrive");
  drawScreen();
}

// Stop all power flow and keep the final batteries on screen.
void endNight() {
  saveCycle();
  phase = MORNING;
  for (int i = 0; i < NUM_EVS; i++) {
    ev[i].mode = IDLE;
    change[i] = 0;
  }
  updateLeds();
  sendSnapshot("summary");
  drawScreen();
}

// Report every plug / unplug as soon as the switch has been stable.
void pollSwitches() {
  for (int i = 0; i < NUM_EVS; i++) {
    const bool raw = readSwitch(i);
    if (raw != rawState[i]) {
      rawState[i] = raw;
      rawChangedMs[i] = millis();
    }
    if (raw == connected[i] || millis() - rawChangedMs[i] < DEBOUNCE_MS)
      continue;

    connected[i] = raw;
    deferred[i] = urgent[i] = false;
    const bool departed = currentSlot >= CONFIG[i].departSlot;
    if (!raw) {
      setMode(i, IDLE);  // stops charging / discharging at once
      updateLeds();
    }
    snprintf(lastEvent[i], sizeof(lastEvent[i]), "%s at %02d:00 - %s",
             raw ? "PLUGGED IN" : "UNPLUGGED", (18 + currentSlot) % 24,
             !raw       ? "now IDLE"
             : departed ? "already departed"
                        : "joins next slot");
    sendSnapshot(raw ? "plug" : "unplug");
    drawScreen();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  for (int i = 0; i < NUM_EVS; i++) pinMode(CONFIG[i].switchPin, INPUT_PULLUP);
  delay(100);  // let the pull-ups settle
  for (int i = 0; i < NUM_EVS; i++) {
    connected[i] = rawState[i] = readSwitch(i);
    strcpy(lastEvent[i], "none yet");
  }

  startNight();
  slotStartMs = millis();
}

void loop() {
  // Unplugging takes effect immediately; plugging in waits for the next slot.
  pollSwitches();

  if (millis() - slotStartMs < phaseLength()) return;
  slotStartMs = millis();

  if (phase == MORNING) {
    startNight();
  } else if (phase == RUNNING && currentSlot == TOTAL_SLOTS - 1) {
    endNight();
  } else {
    currentSlot = phase == ARRIVING ? 0 : currentSlot + 1;
    phase = RUNNING;
    runSlot(currentSlot);
  }
}

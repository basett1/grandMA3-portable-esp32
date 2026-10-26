/*
 * ============================================================================
 *  PORTABLE MAesp32 - UNITA' GIALLA (CONTROLLER TOUCH WIRELESS CYD)
 *  Hardware: ESP32-2432S028 (Cheap Yellow Display 2.8" 320x240 + XPT2046)
 * ============================================================================
 *  Caratteristiche principali V3 PRO:
 *  - Interfaccia grafica a 6 Pagine per controllo completo GrandMA3
 *  - Gestione Touch edge-triggered non bloccante (zero tocchi doppi involontari)
 *  - Doppio click rapido e preciso su '+' (At % +10), 'At' (Normal), '.' (At 0)
 *  - Auto-repeat progressivo sui tasti +/- di configurazione IP/Porta (Pagina 5)
 *  - Ridisegno parziale flicker-free su Pagina 5 (IP/Porta) e Pagina 6 (Backlight)
 *  - Callback ESP-NOW ultra-veloce senza delay() o String dinamiche (40 gruppi)
 *  - Salvataggio luminosita' isolato dal riavvio del Bridge Ethernet
 *  - Standby automatico dopo 30s di inattivita' con risveglio al primo tocco
 * ============================================================================
 */

#include <FS.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <esp_now.h>
#include <WiFi.h>
#include <Preferences.h>

// --- PIN HARDWARE CYD (ESP32-2432S028) ---
#define LED_R 4
#define LED_G 16
#define LED_B 17
#define BACKLIGHT_PIN 21
#define TOUCH_CS_PIN  33
#define TOUCH_IRQ_PIN 36

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);
Preferences preferences;

// --- CONFIGURAZIONE ESP-NOW ---
// Indirizzo MAC dell'unita' VIOLA (Bridge Ethernet)
uint8_t bridgeAddress[] = {0x90, 0xE5, 0xB1, 0x6A, 0xB7, 0x74};

typedef struct struct_message {
    char tipo;          // 'C' = Cmd, 'P' = Ping, 'A' = Ack, 'S' = Settings, 'F' = Feedback
    char cmd[80];       // Comando OSC o Feedback da MA3
    int targetIp[4];    // IP Console GrandMA3
    int localIp[4];     // IP Locale Bridge
    int porta;          // Porta OSC
} struct_message;

struct_message myData;
struct_message inData;

// --- DEFAULT RETE ---
const int DEFAULT_TARGET_IP[4] = {192, 168, 1, 223};
const int DEFAULT_LOCAL_IP[4]  = {192, 168, 1, 100};
const int DEFAULT_PORT         = 8000;

// --- STATO CONNESSIONE RADIO ---
volatile bool isBridgeConnected = false;
volatile unsigned long lastBridgeMessage = 0;
const unsigned long BRIDGE_TIMEOUT = 8000;
unsigned long lastPingTime = 0;

// --- DATABASE GRUPPI (ZERO HEAP FRAGMENTATION) ---
char groupNames[40][20];
int groupOffset = 0;
volatile bool needsRedraw = false;

// --- STATO INTERFACCIA E LUMINOSITA' ---
int currentPage = 1;
int currentBrightnessLevel = 10;             // Livello da 0 a 10 (10 = 100%)
bool isStandby = false;
unsigned long lastTouchTime = 0;
const unsigned long STANDBY_TIMEOUT = 30000; // 30 secondi

int ipParts[4], lipParts[4], portMA3;
int activeRow = 0, activeCol = 0;
String cmdBuffer = "";
bool highlightState = false, soloState = false;

// --- GESTIONE TOUCH NON BLOCCANTE E DOPPIO CLICK ---
bool wasTouched = false;
unsigned long lastReleaseTime = 0;
unsigned long touchPressStartTime = 0;
unsigned long lastRepeatStepTime = 0;
unsigned long lastAtClick = 0, lastDotClick = 0, lastPlusClick = 0;
const unsigned long doubleClickDelay = 420;

// --- GESTIONE LED RGB NON BLOCCANTE ---
bool ledActive = false;
unsigned long ledOffTime = 0;

// --- FUNZIONE GESTIONE BACKLIGHT (10 LIVELLI DA 0 A 100%) ---
void applyBrightnessLevel(int level) {
  currentBrightnessLevel = constrain(level, 0, 10);
  int pwmVal = map(currentBrightnessLevel, 0, 10, 15, 255);
  if (currentBrightnessLevel == 0) pwmVal = 15; // Minimo di sicurezza visibile

  if (!isStandby) {
    analogWrite(BACKLIGHT_PIN, pwmVal);
  }
}

// --- FUNZIONI LED RGB (ACTIVE LOW SUL CYD) ---
void setLED(int r, int g, int b) {
  analogWrite(LED_R, 255 - r);
  analogWrite(LED_G, 255 - g);
  analogWrite(LED_B, 255 - b);
}

void triggerLed(int r, int g, int b, unsigned long durationMs = 45) {
  setLED(r, g, b);
  ledActive = true;
  ledOffTime = millis() + durationMs;
}

void ledFeedback(uint16_t color) {
  if (color == TFT_GREEN || color == 0x07E0) triggerLed(0, 255, 0, 45);
  else if (color == TFT_RED)                 triggerLed(255, 0, 0, 45);
  else if (color == 0xFD20)                  triggerLed(255, 100, 0, 45);
  else if (color == TFT_GOLD)                triggerLed(255, 200, 0, 45);
  else if (color == TFT_SKYBLUE)             triggerLed(0, 180, 255, 45);
  else                                       triggerLed(200, 200, 200, 40);
}

// --- PRIMITIVE GRAFICHE ---
void drawKey(int x, int y, int w, int h, const char* label, uint16_t color, uint16_t bgColor = TFT_BLACK, int textSize = 2) {
  tft.fillRoundRect(x + 1, y + 1, w - 2, h - 2, 4, bgColor);
  tft.drawRoundRect(x, y, w, h, 4, color);
  tft.setTextSize(textSize);
  int16_t textW = tft.textWidth(label);
  tft.setTextColor((bgColor == TFT_BLACK || bgColor == 0x2104 || bgColor == 0x4000 || bgColor == 0x03E0) ? color : TFT_BLACK);
  int16_t textH = 7 * textSize;
  tft.setCursor(x + (w - textW) / 2, y + (h - textH) / 2);
  tft.print(label);
}

void drawHeader() {
  uint16_t headerColor = isBridgeConnected ? TFT_DARKGREEN : TFT_RED;
  tft.fillRect(0, 0, 320, 35, headerColor);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print("MA3 BRIDGE | P");
  tft.print(currentPage);
  drawKey(230, 3, 40, 28, "<", TFT_WHITE, headerColor);
  drawKey(275, 3, 40, 28, ">", TFT_WHITE, headerColor);
}

void drawBuffer() {
  tft.fillRect(5, 36, 310, 28, 0x1082);
  tft.drawRect(5, 36, 310, 28, TFT_DARKGREY);
  tft.setCursor(12, 42);
  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  String toDisplay = cmdBuffer;
  if (toDisplay.length() > 24) {
    toDisplay = "..." + toDisplay.substring(toDisplay.length() - 21);
  }
  tft.print(toDisplay);
}

// --- DISEGNO DELLE 6 PAGINE ---
void drawPage1() {
  tft.fillScreen(TFT_BLACK); drawHeader(); drawBuffer();
  drawKey(5, 70, 72, 38, "7", TFT_WHITE); drawKey(83, 70, 72, 38, "8", TFT_WHITE); drawKey(161, 70, 72, 38, "9", TFT_WHITE); drawKey(239, 70, 76, 38, "+", 0xFD20);
  drawKey(5, 112, 72, 38, "4", TFT_WHITE); drawKey(83, 112, 72, 38, "5", TFT_WHITE); drawKey(161, 112, 72, 38, "6", TFT_WHITE); drawKey(239, 112, 76, 38, "Thru", 0xFD20);
  drawKey(5, 154, 72, 38, "1", TFT_WHITE); drawKey(83, 154, 72, 38, "2", TFT_WHITE); drawKey(161, 154, 72, 38, "3", TFT_WHITE); drawKey(239, 154, 76, 38, "At", 0xFD20);
  drawKey(5, 196, 72, 38, "0", TFT_WHITE); drawKey(83, 196, 72, 38, ".", TFT_WHITE); drawKey(161, 196, 72, 38, "CLR", TFT_RED); drawKey(239, 196, 76, 38, "PLEASE", 0x07E0, 0x07E0, 2);
}

void updatePage2Toggles() {
  uint16_t hCol = highlightState ? 0xFD20 : 0x2104;
  drawKey(10, 110, 145, 55, "HIGHLIGHT", 0xFD20, hCol);
  uint16_t sCol = soloState ? 0x001F : 0x2104;
  drawKey(165, 110, 145, 55, "SOLO", 0x001F, sCol);
}

void drawPage2() {
  tft.fillScreen(TFT_BLACK); drawHeader();
  drawKey(10, 45, 145, 55, "NEXT", TFT_WHITE); drawKey(165, 45, 145, 55, "PREV", TFT_WHITE);
  updatePage2Toggles();
  drawKey(10, 175, 300, 55, "CLEAR", TFT_RED, TFT_BLACK);
}

void drawPage3() {
  tft.fillScreen(TFT_BLACK); drawHeader(); drawBuffer();
  drawKey(5, 70, 72, 38, "7", TFT_WHITE); drawKey(83, 70, 72, 38, "8", TFT_WHITE); drawKey(161, 70, 72, 38, "9", TFT_WHITE); drawKey(239, 70, 76, 38, "GOTO", TFT_CYAN);
  drawKey(5, 112, 72, 38, "4", TFT_WHITE); drawKey(83, 112, 72, 38, "5", TFT_WHITE); drawKey(161, 112, 72, 38, "6", TFT_WHITE); drawKey(239, 112, 76, 38, "CUE", TFT_CYAN);
  drawKey(5, 154, 72, 38, "1", TFT_WHITE); drawKey(83, 154, 72, 38, "2", TFT_WHITE); drawKey(161, 154, 72, 38, "3", TFT_WHITE); drawKey(239, 154, 76, 38, "SEQ", TFT_CYAN);
  drawKey(5, 196, 72, 38, "0", TFT_WHITE); drawKey(83, 196, 72, 38, ".", TFT_WHITE); drawKey(161, 196, 72, 38, "CLR", TFT_RED); drawKey(239, 196, 76, 38, "PLEASE", 0x07E0, 0x07E0, 2);
}

// Ridisegna solo i 6 pulsanti gruppo della Pagina 4 (zero flickering durante il sync!)
void updatePage4GroupKeys() {
  for (int i = 0; i < 6; i++) {
    int actualIdx = i + groupOffset;
    int x = (i % 2 == 0) ? 10 : 165;
    int y = 70 + ((i / 2) * 46);
    if (actualIdx >= 40) {
      drawKey(x, y, 145, 42, "-", 0x4208, 0x2104, 2);
      continue;
    }
    bool isEmpty = (groupNames[actualIdx][0] == '\0' || strcmp(groupNames[actualIdx], "Empty") == 0);
    char labelBuf[20];
    if (isEmpty) {
      snprintf(labelBuf, sizeof(labelBuf), "G%d", actualIdx + 1);
    } else {
      strncpy(labelBuf, groupNames[actualIdx], sizeof(labelBuf) - 1);
      labelBuf[sizeof(labelBuf) - 1] = '\0';
    }
    uint16_t color = isEmpty ? 0x4208 : 0xFD20;
    drawKey(x, y, 145, 42, labelBuf, color, 0x2104, 2);
  }
}

void drawPage4() {
  tft.fillScreen(TFT_BLACK); drawHeader(); drawBuffer();
  updatePage4GroupKeys();
  drawKey(5, 210, 65, 25, "<<", TFT_SKYBLUE, TFT_BLACK, 2);
  drawKey(75, 210, 170, 25, "SYNC NAMES", 0x07E0, 0x07E0, 2);
  drawKey(250, 210, 65, 25, ">>", TFT_SKYBLUE, TFT_BLACK, 2);
}

// Aggiornamento parziale flicker-free dei soli valori numerici in Pagina 5
void updatePage5Values() {
  for (int i = 0; i < 4; i++) {
    uint16_t color = (activeRow == 1 && activeCol == i) ? TFT_YELLOW : 0x4208;
    tft.fillRect(11 + (i * 78), 56, 63, 28, TFT_BLACK);
    tft.drawRect(10 + (i * 78), 55, 65, 30, color);
    tft.setCursor(20 + (i * 78), 63);
    tft.setTextSize(2);
    tft.setTextColor(color == TFT_YELLOW ? TFT_YELLOW : TFT_WHITE);
    tft.print(lipParts[i]);
  }
  for (int i = 0; i < 4; i++) {
    uint16_t color = (activeRow == 0 && activeCol == i) ? TFT_YELLOW : 0x4208;
    tft.fillRect(11 + (i * 78), 111, 63, 28, TFT_BLACK);
    tft.drawRect(10 + (i * 78), 110, 65, 30, color);
    tft.setCursor(20 + (i * 78), 118);
    tft.setTextSize(2);
    tft.setTextColor(color == TFT_YELLOW ? TFT_YELLOW : TFT_WHITE);
    tft.print(ipParts[i]);
  }
  uint16_t pColor = (activeRow == 2) ? TFT_YELLOW : 0x4208;
  tft.fillRect(11, 161, 98, 28, TFT_BLACK);
  tft.drawRect(10, 160, 100, 30, pColor);
  tft.setCursor(20, 168);
  tft.setTextSize(2);
  tft.setTextColor(pColor == TFT_YELLOW ? TFT_YELLOW : TFT_WHITE);
  tft.print(portMA3);
}

void drawPage5() {
  tft.fillScreen(TFT_BLACK); drawHeader();
  tft.setTextSize(1); tft.setTextColor(TFT_WHITE);
  tft.setCursor(10, 42);  tft.print("LOCAL IP (BRIDGE):");
  tft.setCursor(10, 95);  tft.print("TARGET IP (MA3):");
  tft.setCursor(10, 150); tft.print("OSC PORT:");
  updatePage5Values();
  drawKey(125, 155, 85, 40, "+", 0x07E0);
  drawKey(220, 155, 85, 40, "-", TFT_RED);
  drawKey(10, 200, 300, 35, "SAVE SETTINGS", 0x001F, 0x001F, 2);
}

void updatePage6BrightnessText() {
  tft.fillRect(20, 160, 280, 30, TFT_BLACK);
  int percent = currentBrightnessLevel * 10;
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(50, 165);
  tft.print("Luminosita: ");
  tft.print(percent);
  tft.print("%   ");
}

void drawPage6() {
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 42);
  tft.print("BACKLIGHT");
  tft.drawFastHLine(10, 60, 300, TFT_DARKGREY);
  drawKey(20, 80, 120, 60, "-", TFT_RED, 0x4000, 4);
  drawKey(180, 80, 120, 60, "+", TFT_GREEN, 0x03E0, 4);
  updatePage6BrightnessText();
}

void drawCurrentPage() {
  if (currentPage == 1)      drawPage1();
  else if (currentPage == 2) drawPage2();
  else if (currentPage == 3) drawPage3();
  else if (currentPage == 4) drawPage4();
  else if (currentPage == 5) drawPage5();
  else if (currentPage == 6) drawPage6();
}

// --- COMUNICAZIONE & CALLBACK ESP-NOW ---
void sendToBridge() {
  esp_now_send(bridgeAddress, (uint8_t *)&myData, sizeof(myData));
}

void sendOSC(const String &command) {
  myData.tipo = 'C';
  memset(myData.cmd, 0, sizeof(myData.cmd));
  strncpy(myData.cmd, command.c_str(), sizeof(myData.cmd) - 1);
  sendToBridge();
}

// Richiede al Bridge Viola di interrogare i 40 gruppi su MA3 via Inline Lua (Zero-Plugin!)
void requestZeroPluginGroupSync() {
  myData.tipo = 'G';
  strcpy(myData.cmd, "SYNC_GROUPS");
  sendToBridge();
}

// Callback veloce e thread-safe: zero delay() e zero String nell'interrupt Wi-Fi
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingBytes, int len) {
  if (len != sizeof(inData)) return;
  memcpy(&inData, incomingBytes, sizeof(inData));
  inData.cmd[sizeof(inData.cmd) - 1] = '\0';

  isBridgeConnected = true;
  lastBridgeMessage = millis();

  if (inData.tipo == 'F') {
    int startPos = -1;
    for (int i = 0; i < 75; i++) {
      if (inData.cmd[i] == 'G' && inData.cmd[i + 1] == 'N') {
        startPos = i;
        break;
      }
    }
    if (startPos != -1) {
      int idx = 0;
      bool hasDigits = false;
      int nameStart = -1;
      for (int i = startPos + 2; i < 79; i++) {
        char c = inData.cmd[i];
        if (c == ':') {
          nameStart = i + 1;
          break;
        }
        if (c >= '0' && c <= '9') {
          idx = (idx * 10) + (c - '0');
          hasDigits = true;
        }
      }
      if (hasDigits && nameStart != -1 && idx >= 0 && idx < 40) {
        char parsedName[20];
        int pos = 0;
        for (int k = nameStart; k < 79 && pos < 19; k++) {
          char c = inData.cmd[k];
          if (c >= 32 && c <= 126) {
            parsedName[pos++] = c;
          } else {
            break;
          }
        }
        parsedName[pos] = '\0';
        if (strcmp(parsedName, "Empty") == 0) {
          groupNames[idx][0] = '\0';
        } else {
          strncpy(groupNames[idx], parsedName, sizeof(groupNames[idx]) - 1);
          groupNames[idx][sizeof(groupNames[idx]) - 1] = '\0';
        }
        needsRedraw = true;
      }
    }
    triggerLed(0, 0, 255, 15);
  }
}

// --- GESTIONE PREFERENZE NVS ---
void loadSettings() {
  preferences.begin("ma3-config", true);
  for (int i = 0; i < 4; i++) {
    char keyIP[8], keyLIP[8];
    snprintf(keyIP, sizeof(keyIP), "ip%d", i);
    snprintf(keyLIP, sizeof(keyLIP), "lip%d", i);
    ipParts[i]  = preferences.getInt(keyIP, DEFAULT_TARGET_IP[i]);
    lipParts[i] = preferences.getInt(keyLIP, DEFAULT_LOCAL_IP[i]);
  }
  portMA3 = preferences.getInt("port", DEFAULT_PORT);
  currentBrightnessLevel = preferences.getInt("brightLvl", 10);
  preferences.end();

  for (int i = 0; i < 40; i++) {
    groupNames[i][0] = '\0';
  }
}

// Salva solo la luminosita' locale senza inviare pacchetti di riavvio al Bridge
void saveBrightnessSetting() {
  preferences.begin("ma3-config", false);
  preferences.putInt("brightLvl", currentBrightnessLevel);
  preferences.end();
}

// Salva la configurazione di rete e invia il pacchetto 'S' di aggiornamento al Bridge
void saveNetworkSettings() {
  preferences.begin("ma3-config", false);
  for (int i = 0; i < 4; i++) {
    char keyIP[8], keyLIP[8];
    snprintf(keyIP, sizeof(keyIP), "ip%d", i);
    snprintf(keyLIP, sizeof(keyLIP), "lip%d", i);
    preferences.putInt(keyIP, ipParts[i]);
    preferences.putInt(keyLIP, lipParts[i]);
  }
  preferences.putInt("port", portMA3);
  preferences.putInt("brightLvl", currentBrightnessLevel);
  preferences.end();

  myData.tipo = 'S';
  for (int i = 0; i < 4; i++) {
    myData.targetIp[i] = ipParts[i];
    myData.localIp[i]  = lipParts[i];
  }
  myData.porta = portMA3;
  sendToBridge();
}

// --- LOGICA TASTI PER OGNI PAGINA ---
void handleSingleTap(int16_t x, int16_t y, unsigned long now) {
  // BARRA DI NAVIGAZIONE IN ALTO (CAMBIO PAGINA)
  if (y < 40) {
    if (x >= 225 && x <= 272) {
      currentPage--;
      if (currentPage < 1) currentPage = 6;
      drawCurrentPage();
    } else if (x >= 273) {
      currentPage++;
      if (currentPage > 6) currentPage = 1;
      drawCurrentPage();
    }
    return;
  }

  // PAGINA 1: TASTIERINO CANALI
  if (currentPage == 1) {
    int col = constrain(x / 80, 0, 3);
    int row = (y - 70) / 42;
    if (y >= 70 && y < 196) {
      if (col == 3 && row == 0) {
        // Tasto '+' (Doppio click -> At Percent + 10)
        if (now - lastPlusClick < doubleClickDelay) {
          if (cmdBuffer.endsWith(" + ")) {
            cmdBuffer.remove(cmdBuffer.length() - 3);
          }
          if (cmdBuffer.length() > 0) {
            sendOSC(cmdBuffer + " At Percent + 10");
            cmdBuffer = "";
          } else {
            sendOSC("At Percent + 10");
          }
          ledFeedback(0xFD20);
          lastPlusClick = 0;
        } else {
          cmdBuffer += " + ";
          ledFeedback(TFT_WHITE);
          lastPlusClick = now;
        }
      } else if (col == 3 && row == 2) {
        // Tasto 'At' (Doppio click -> At Normal)
        if (now - lastAtClick < doubleClickDelay && cmdBuffer.endsWith(" At ")) {
          cmdBuffer.remove(cmdBuffer.length() - 4);
          sendOSC(cmdBuffer + " At Normal");
          cmdBuffer = "";
          ledFeedback(0xFD20);
          lastAtClick = 0;
        } else {
          cmdBuffer += " At ";
          ledFeedback(TFT_WHITE);
          lastAtClick = now;
        }
      } else {
        if (col == 0 && row == 0)      cmdBuffer += "7";
        else if (col == 1 && row == 0) cmdBuffer += "8";
        else if (col == 2 && row == 0) cmdBuffer += "9";
        else if (col == 0 && row == 1) cmdBuffer += "4";
        else if (col == 1 && row == 1) cmdBuffer += "5";
        else if (col == 2 && row == 1) cmdBuffer += "6";
        else if (col == 3 && row == 1) cmdBuffer += " Thru ";
        else if (col == 0 && row == 2) cmdBuffer += "1";
        else if (col == 1 && row == 2) cmdBuffer += "2";
        else if (col == 2 && row == 2) cmdBuffer += "3";
        ledFeedback(TFT_WHITE);
      }
    } else if (y >= 196) {
      if (x < 80) {
        cmdBuffer += "0";
        ledFeedback(TFT_WHITE);
      } else if (x < 160) {
        // Tasto '.' (Doppio click -> At 0)
        if (now - lastDotClick < doubleClickDelay && cmdBuffer.endsWith(".")) {
          cmdBuffer.remove(cmdBuffer.length() - 1);
          sendOSC(cmdBuffer + " At 0");
          cmdBuffer = "";
          ledFeedback(0xFD20);
          lastDotClick = 0;
        } else {
          cmdBuffer += ".";
          ledFeedback(TFT_WHITE);
          lastDotClick = now;
        }
      } else if (x < 240) {
        if (cmdBuffer.length() > 0) cmdBuffer.remove(cmdBuffer.length() - 1);
        else sendOSC("Clear");
        ledFeedback(TFT_RED);
      } else {
        if (cmdBuffer != "") {
          sendOSC(cmdBuffer);
          cmdBuffer = "";
        } else {
          sendOSC("Please");
        }
        ledFeedback(TFT_GREEN);
      }
    }
    drawBuffer();
  }
  // PAGINA 2: UTILITY
  else if (currentPage == 2) {
    if (y > 45 && y < 100) {
      sendOSC((x < 160) ? "Next" : "Prev");
      ledFeedback(TFT_WHITE);
    } else if (y >= 110 && y < 165) {
      if (x < 160) {
        highlightState = !highlightState;
        sendOSC(highlightState ? "Highlight On" : "Highlight Off");
      } else {
        soloState = !soloState;
        sendOSC(soloState ? "Solo On" : "Solo Off");
      }
      updatePage2Toggles();
      ledFeedback(0xFD20);
    } else if (y >= 175) {
      sendOSC("Clear");
      ledFeedback(TFT_RED);
    }
  }
  // PAGINA 3: CUE / SEQ
  else if (currentPage == 3) {
    int col = constrain(x / 80, 0, 3);
    int row = (y - 70) / 42;
    if (y >= 70 && y < 196) {
      if (col == 3 && row == 0)      cmdBuffer += "Goto ";
      else if (col == 3 && row == 1) cmdBuffer += "Cue ";
      else if (col == 3 && row == 2) cmdBuffer += "Sequence ";
      else if (col == 0 && row == 0) cmdBuffer += "7";
      else if (col == 1 && row == 0) cmdBuffer += "8";
      else if (col == 2 && row == 0) cmdBuffer += "9";
      else if (col == 0 && row == 1) cmdBuffer += "4";
      else if (col == 1 && row == 1) cmdBuffer += "5";
      else if (col == 2 && row == 1) cmdBuffer += "6";
      else if (col == 0 && row == 2) cmdBuffer += "1";
      else if (col == 1 && row == 2) cmdBuffer += "2";
      else if (col == 2 && row == 2) cmdBuffer += "3";
      ledFeedback(TFT_WHITE);
    } else if (y >= 196) {
      if (x < 80) {
        cmdBuffer += "0";
        ledFeedback(TFT_WHITE);
      } else if (x < 160) {
        cmdBuffer += ".";
        ledFeedback(TFT_WHITE);
      } else if (x < 240) {
        if (cmdBuffer.length() > 0) cmdBuffer.remove(cmdBuffer.length() - 1);
        else sendOSC("Clear");
        ledFeedback(TFT_RED);
      } else {
        sendOSC(cmdBuffer != "" ? cmdBuffer : "Please");
        cmdBuffer = "";
        ledFeedback(TFT_GREEN);
      }
    }
    drawBuffer();
  }
  // PAGINA 4: GRUPPI
  else if (currentPage == 4) {
    if (y >= 205) {
      if (x >= 5 && x < 70) {
        if (groupOffset >= 6) groupOffset -= 6;
        updatePage4GroupKeys();
        ledFeedback(TFT_SKYBLUE);
      } else if (x >= 75 && x <= 245) {
        requestZeroPluginGroupSync();
        ledFeedback(0x07E0);
        Serial.println("[UI] Sincronizzazione 40 gruppi avviata (Inline Lua Zero-Plugin).");
      } else if (x > 245) {
        if (groupOffset < 34) groupOffset += 6;
        updatePage4GroupKeys();
        ledFeedback(TFT_SKYBLUE);
      }
    } else if (y >= 70) {
      int col = (x < 160) ? 0 : 1;
      int row = (y - 70) / 46;
      int idx = (row * 2) + col + groupOffset;
      if (idx >= 0 && idx < 40) {
        cmdBuffer += "Group " + String(idx + 1) + " ";
        ledFeedback(0xFD20);
        drawBuffer();
      }
    }
  }
  // PAGINA 5: RETE & IP
  else if (currentPage == 5) {
    if (y > 55 && y < 85) {
      for (int i = 0; i < 4; i++) {
        if (x > (i * 78) && x < (i * 78) + 78) {
          activeRow = 1; activeCol = i;
          updatePage5Values();
          ledFeedback(TFT_WHITE);
        }
      }
    } else if (y > 110 && y < 140) {
      for (int i = 0; i < 4; i++) {
        if (x > (i * 78) && x < (i * 78) + 78) {
          activeRow = 0; activeCol = i;
          updatePage5Values();
          ledFeedback(TFT_WHITE);
        }
      }
    } else if (y > 160 && y < 190 && x < 110) {
      activeRow = 2;
      updatePage5Values();
      ledFeedback(TFT_WHITE);
    } else if (y > 155 && y < 195) {
      int delta = (x > 125 && x < 210) ? 1 : (x > 220 ? -1 : 0);
      if (delta != 0) {
        if (activeRow == 0)      ipParts[activeCol]  = constrain(ipParts[activeCol] + delta, 0, 255);
        else if (activeRow == 1) lipParts[activeCol] = constrain(lipParts[activeCol] + delta, 0, 255);
        else if (activeRow == 2) portMA3             = constrain(portMA3 + delta, 1, 65535);
        updatePage5Values();
        ledFeedback(TFT_WHITE);
      }
    } else if (y > 200) {
      saveNetworkSettings();
      ledFeedback(TFT_GREEN);
    }
  }
  // PAGINA 6: BACKLIGHT (10 LIVELLI)
  else if (currentPage == 6) {
    if (y >= 80 && y <= 140) {
      if (x >= 20 && x <= 140) {
        applyBrightnessLevel(currentBrightnessLevel - 1);
        saveBrightnessSetting();
        updatePage6BrightnessText();
        ledFeedback(TFT_WHITE);
      } else if (x >= 180 && x <= 300) {
        applyBrightnessLevel(currentBrightnessLevel + 1);
        saveBrightnessSetting();
        updatePage6BrightnessText();
        ledFeedback(TFT_WHITE);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== PORTABLE MAesp32 - GIALLA (CYD) V3 PRO ===");

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  setLED(0, 0, 0);
  pinMode(BACKLIGHT_PIN, OUTPUT);

  loadSettings();
  applyBrightnessLevel(currentBrightnessLevel);

  tft.init();
  tft.setRotation(1);

  touchSPI.begin(25, 39, 32, 33);
  touch.begin(touchSPI);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Disattiva modem sleep per latenza minima ESP-NOW
  WiFi.disconnect();

  if (esp_now_init() == ESP_OK) {
    Serial.println("[ESP-NOW] Inizializzato con successo.");
    esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, bridgeAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  } else {
    Serial.println("[ERRORE] ESP-NOW fallito!");
  }

  lastTouchTime = millis();
  drawPage1();
  Serial.println("=== SETUP COMPLETATO ===");
}

void loop() {
  unsigned long currentMillis = millis();

  // 0. Spegnimento automatico non bloccante del LED RGB di feedback
  if (ledActive && (long)(currentMillis - ledOffTime) >= 0) {
    setLED(0, 0, 0);
    ledActive = false;
  }

  // 1. Gestione Standby Automatico (30 secondi di inattivita')
  if (!isStandby && (currentMillis - lastTouchTime > STANDBY_TIMEOUT)) {
    isStandby = true;
    analogWrite(BACKLIGHT_PIN, 0);
    Serial.println("[POWER] Standby attivato (30s inattivita).");
  }

  // 2. Invio Ping periodico alla Viola ogni 2 secondi
  if (currentMillis - lastPingTime > 2000) {
    lastPingTime = currentMillis;
    myData.tipo = 'P';
    esp_now_send(bridgeAddress, (uint8_t *)&myData, sizeof(myData));
  }

  // 3. Controllo Timeout Connessione Radio
  bool previousState = isBridgeConnected;
  if (currentMillis - lastBridgeMessage > BRIDGE_TIMEOUT) {
    isBridgeConnected = false;
  }
  if (previousState != isBridgeConnected && !isStandby) {
    drawHeader();
  }

  // 4. Ridisegno sicuro (flicker-free) per la Pagina 4 quando arrivano nomi gruppi via radio
  if (needsRedraw) {
    needsRedraw = false;
    if (currentPage == 4 && !isStandby) {
      updatePage4GroupKeys();
    }
  }

  // 5. Gestione Touch Screen non bloccante (Edge Detection + Auto-Repeat Pagina 5)
  bool currentlyTouched = touch.touched();

  if (currentlyTouched) {
    lastTouchTime = currentMillis;

    // Se lo schermo era in standby, il primo tocco lo risveglia senza premere tasti
    if (isStandby) {
      isStandby = false;
      applyBrightnessLevel(currentBrightnessLevel);
      wasTouched = true; // Ignora il resto della pressione finche' non viene rilasciato il dito
      Serial.println("[POWER] Schermo risvegliato dal touch.");
      return;
    }

    TS_Point p = touch.getPoint();
    int16_t x = constrain(map(p.x, 344, 3643, 0, 320), 0, 319);
    int16_t y = constrain(map(p.y, 544, 3576, 0, 240), 0, 239);

    if (!wasTouched) {
      // Fronte di salita del tocco (debounce di 45ms dal rilascio precedente)
      if (currentMillis - lastReleaseTime >= 45) {
        wasTouched = true;
        touchPressStartTime = currentMillis;
        lastRepeatStepTime = currentMillis;
        handleSingleTap(x, y, currentMillis);
      }
    } else if (currentPage == 5 && y > 155 && y < 195) {
      // Auto-repeat progressivo tenendo premuto '+' o '-' nella Pagina 5 (IP/Porta)
      unsigned long holdDuration = currentMillis - touchPressStartTime;
      unsigned long repeatInterval = (holdDuration > 1200) ? 35 : 140;
      if (holdDuration > 350 && (currentMillis - lastRepeatStepTime >= repeatInterval)) {
        lastRepeatStepTime = currentMillis;
        int delta = (x > 125 && x < 210) ? 1 : (x > 220 ? -1 : 0);
        if (delta != 0) {
          int step = (holdDuration > 2500 && activeRow == 2) ? (delta * 10) : delta;
          if (activeRow == 0)      ipParts[activeCol]  = constrain(ipParts[activeCol] + step, 0, 255);
          else if (activeRow == 1) lipParts[activeCol] = constrain(lipParts[activeCol] + step, 0, 255);
          else if (activeRow == 2) portMA3             = constrain(portMA3 + step, 1, 65535);
          updatePage5Values();
        }
      }
    }
  } else {
    if (wasTouched) {
      wasTouched = false;
      lastReleaseTime = currentMillis;
    }
  }
}

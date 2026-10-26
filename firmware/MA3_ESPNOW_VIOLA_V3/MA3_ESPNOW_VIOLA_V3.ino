/*
 * ============================================================================
 *  PORTABLE MAesp32 - UNITA' VIOLA (BRIDGE ESP-NOW <-> ETHERNET OSC)
 *  Hardware: ESP32-S3 + W5500 Ethernet SPI + Display OLED 0.42" (72x40 I2C)
 * ============================================================================
 *  Caratteristiche principali V3 PRO (Zero-Plugin Edition):
 *  - ZERO-PLUGIN GROUP SYNC: Iniezione diretta di comandi Inline Lua via /cmd
 *    per leggere i 40 Gruppi dal DataPool di GrandMA3 senza installare plugin!
 *  - Scansione gruppi a 8 micro-batch (5 gruppi ogni 75ms) per zero perdita UDP
 *  - Auto-configurazione IP di ritorno sulla riga OSC 1 di GrandMA3
 *  - Zero dipendenza da Serial/USB (nessun blocco con alimentazione standalone)
 *  - Coda FreeRTOS thread-safe per disaccoppiare callback Wi-Fi e bus SPI W5500
 *  - Display OLED in modalita' Full-Buffer (_F_) con refresh non bloccante (60ms)
 * ============================================================================
 */

#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <esp_now.h>
#include <WiFi.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// --- CONFIGURAZIONE PIN HARDWARE (PCB ESP_ETH) ---
#define BOOT_BUTTON_PIN 9
const int sck_pin  = 4;
const int miso_pin = 1;
const int mosi_pin = 7;
const int cs_pin   = 3;

// Display OLED 72x40 in modalita' Full Buffer (_F_) per massime prestazioni I2C
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);
Preferences preferences;

// --- CONFIGURAZIONE DI RETE DI DEFAULT ---
const uint8_t DEFAULT_TARGET_IP[4] = {192, 168, 1, 223};
const uint8_t DEFAULT_LOCAL_IP[4]  = {192, 168, 1, 100};
const unsigned int DEFAULT_PORT    = 8000;

byte mac_eth[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress targetIP(192, 168, 1, 223);
IPAddress localIP(192, 168, 1, 100);
unsigned int targetPort = DEFAULT_PORT;
EthernetUDP Udp;

// Indirizzo MAC dell'unita' GIALLA (Controller CYD)
uint8_t yellowAddress[] = {0x88, 0x13, 0xBF, 0x0D, 0x53, 0xB8};

// --- STATO INTERFACCIA E CONNESSIONE ---
char lastCommand[24] = "READY";
int displayPage = 0;
bool lastBtnState = HIGH;
unsigned long lastBtnDebounce = 0;
unsigned long lastPacketTime = 0;
unsigned long lastOledUpdate = 0;

volatile bool isLinked = false;
volatile unsigned long lastMessageTime = 0;
const unsigned long LINK_TIMEOUT = 4000;
const unsigned long OLED_REFRESH_MS = 60;

// --- MACCHINA A STATI PER SINCRONIZZAZIONE GRUPPI ZERO-PLUGIN (INLINE LUA) ---
bool syncActive = false;
int syncBatchIndex = 0;                 // 8 batch da 5 gruppi ciascuno (0..7 -> Gruppi 1..40)
unsigned long lastSyncBatchTime = 0;
const unsigned long SYNC_BATCH_INTERVAL_MS = 75;

// --- STRUTTURA MESSAGGIO ESP-NOW ---
typedef struct struct_message {
    char tipo;          // 'C'=Cmd, 'P'=Ping, 'A'=Ack, 'S'=Settings, 'F'=Feedback, 'G'=SyncGroups
    char cmd[80];       // Payload comando OSC o feedback
    int targetIp[4];    // IP GrandMA3
    int localIp[4];     // IP Bridge
    int porta;          // Porta OSC
} struct_message;

struct_message feedbackData;
QueueHandle_t rxQueue;

// --- FUNZIONE COSTRUZIONE E INVIO PACCHETTO OSC A GRANDMA3 ---
void sendMA3Command(const char* command, bool updateLabel = true) {
  if (Ethernet.linkStatus() == LinkOFF) {
    strncpy(lastCommand, "ETH DISCONN!", sizeof(lastCommand) - 1);
    lastCommand[sizeof(lastCommand) - 1] = '\0';
    return;
  }

  Udp.beginPacket(targetIP, targetPort);
  Udp.write((const uint8_t*)"/cmd\0\0\0\0", 8);
  Udp.write((const uint8_t*)",s\0\0", 4);

  size_t len = strlen(command);
  Udp.write((const uint8_t*)command, len);

  // Allineamento e terminazione zero-padding a multipli di 4 byte (Specifica OSC 1.0)
  int pad = 4 - (len % 4);
  for (int i = 0; i < pad; i++) {
    Udp.write((uint8_t)'\0');
  }
  Udp.endPacket();

  if (updateLabel) {
    strncpy(lastCommand, command, sizeof(lastCommand) - 1);
    lastCommand[sizeof(lastCommand) - 1] = '\0';
  }
  lastPacketTime = millis();
}

// --- AVVIO SINCRONIZZAZIONE 40 GRUPPI SENZA PLUGIN (INLINE LUA INJECTION) ---
void startZeroPluginGroupSync() {
  if (Ethernet.linkStatus() == LinkOFF) {
    strncpy(lastCommand, "ETH DISCONN!", sizeof(lastCommand) - 1);
    return;
  }

  // Passo 0: Auto-discovery / Auto-creazione NON DISTRUTTIVA della riga OSC su GrandMA3.
  // 1. Abilita l'uscita globale OSC (EnableOutput) se era disattivata.
  // 2. Cerca tra i figli di ShowData().OSCBase se esiste gia' una riga 'MAesp32' o col nostro IP.
  // 3. Se NON esiste, NON tocca le righe esistenti (OSC 1, 2, ecc.), ma fa :Append()
  //    creando una nuova riga dedicata 'MAesp32' in coda!
  // 4. Salva l'indice reale MA3 (o.index) nella variabile globale Lua _G.MAESP.
  char cfgLua[400];
  snprintf(cfgLua, sizeof(cfgLua),
    "Lua \"pcall(function() local b=ShowData().OSCBase;pcall(function() b.EnableOutput=true end);local o=nil;for _,v in ipairs(b:Children()) do if v.Name=='MAesp32' or v.DestinationIP=='%u.%u.%u.%u' then o=v;break end end;if not o then o=b:Append();o.Name='MAesp32' end;o.DestinationIP='%u.%u.%u.%u';o.Port=%u;o.Send=true;_G.MAESP=o.index end)\"",
    localIP[0], localIP[1], localIP[2], localIP[3],
    localIP[0], localIP[1], localIP[2], localIP[3], targetPort);
  sendMA3Command(cfgLua, false);

  // Avvia la scansione degli 8 batch (5 gruppi per volta ogni 75ms)
  syncActive = true;
  syncBatchIndex = 0;
  lastSyncBatchTime = millis();
  strncpy(lastCommand, "LUA SYNC...", sizeof(lastCommand) - 1);
}

// --- ESECUZIONE BATCH INLINE LUA PER LETTURA GRUPPI DA DATAPOOL ---
void processZeroPluginGroupSync(unsigned long now) {
  if (!syncActive) return;
  if (now - lastSyncBatchTime < SYNC_BATCH_INTERVAL_MS) return;

  lastSyncBatchTime = now;
  int startGrp = (syncBatchIndex * 5) + 1;
  int endGrp   = startGrp + 4;

  // Usa l'indice dinamico _G.MAESP (trovato o creato al Passo 0) per SendOSC,
  // e string.char(34) per le virgolette interne evitando conflitti col parser di MA3.
  char luaCmd[300];
  snprintf(luaCmd, sizeof(luaCmd),
    "Lua \"local q=string.char(34);local u=_G.MAESP or 1;local g=DataPool().Groups;for i=%d,%d do local o=g[i];local n=(o and o.name and o.name~='') and o.name or 'Empty';n=n:gsub('[^%%w%%s%%-_.]',''):sub(1,16);Cmd('SendOSC '..u..' '..q..'/g,s,GN'..(i-1)..':'..n..q) end\"",
    startGrp, endGrp);

  sendMA3Command(luaCmd, false);
  snprintf(lastCommand, sizeof(lastCommand), "SYNC G%d-%d", startGrp, endGrp);

  syncBatchIndex++;
  if (syncBatchIndex >= 8) {
    syncActive = false;
  }
}

// --- SALVATAGGIO NUOVA CONFIGURAZIONE DI RETE E RIAVVIO ---
void applyAndSaveNetworkSettings(const struct_message &msg) {
  preferences.begin("net-config", false);
  preferences.putUInt("port", (unsigned int)constrain(msg.porta, 1, 65535));
  for (int i = 0; i < 4; i++) {
    char keyIP[8], keyLIP[8];
    snprintf(keyIP, sizeof(keyIP), "ip%d", i);
    snprintf(keyLIP, sizeof(keyLIP), "lip%d", i);
    preferences.putInt(keyIP, constrain(msg.targetIp[i], 0, 255));
    preferences.putInt(keyLIP, constrain(msg.localIp[i], 0, 255));
  }
  preferences.end();

  // Mostra conferma sul display prima del riavvio
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(4, 16, "SAVING IP...");
  u8g2.drawStr(4, 30, "REBOOTING");
  u8g2.sendBuffer();

  delay(400);
  ESP.restart();
}

// --- CALLBACK RICEZIONE RADIO (ESEGUITA NEL TASK WI-FI) ---
// Nota: Nessun accesso SPI/W5500 o Flash qui dentro per garantire thread-safety.
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingBytes, int len) {
  if (len != sizeof(struct_message)) return;

  struct_message msg;
  memcpy(&msg, incomingBytes, sizeof(struct_message));
  msg.cmd[sizeof(msg.cmd) - 1] = '\0'; // Sicurezza anti buffer-overflow

  isLinked = true;
  lastMessageTime = millis();

  if (rxQueue != NULL) {
    xQueueSend(rxQueue, &msg, 0);
  }
}

// --- DISEGNO INTERFACCIA DISPLAY OLED 72x40 ---
void updateDisplay() {
  u8g2.clearBuffer();
  u8g2.drawHLine(0, 26, 72);

  // Riga inferiore: ultimo comando inviato o feedback ricevuto
  u8g2.setFont(u8g2_font_5x8_tr);
  char cmdShort[15];
  strncpy(cmdShort, lastCommand, 14);
  cmdShort[14] = '\0';
  u8g2.drawStr(0, 38, cmdShort);

  // Riga superiore: etichetta pagina e valore
  u8g2.setFont(u8g2_font_6x10_tr);
  const char* label = "";
  char value[24] = "";

  if (displayPage == 0) {
    label = "BR-IP:";
    snprintf(value, sizeof(value), "%u.%u.%u.%u", localIP[0], localIP[1], localIP[2], localIP[3]);
  } else if (displayPage == 1) {
    label = "MA-IP:";
    snprintf(value, sizeof(value), "%u.%u.%u.%u", targetIP[0], targetIP[1], targetIP[2], targetIP[3]);
  } else if (displayPage == 2) {
    label = "PORT:";
    snprintf(value, sizeof(value), "%u %s", targetPort, (Ethernet.linkStatus() == LinkOFF) ? "!ETH" : "ETH");
  } else {
    label = "CYB-STAT:";
    snprintf(value, sizeof(value), "%s", isLinked ? "LINKED" : "UNLINKED");
  }

  u8g2.drawStr(0, 9, label);
  u8g2.setFont(u8g2_font_5x7_tr);

  int width = u8g2.getStrWidth(value);
  if (width > 72) {
    int offset = (millis() / 40) % (width + 20);
    u8g2.drawStr(-offset + 20, 20, value);
  } else {
    u8g2.drawStr(0, 20, value);
  }

  // Indicatori pagina (4 pallini in alto a destra)
  for (int i = 0; i < 4; i++) {
    if (i == displayPage) u8g2.drawDisc(56 + (i * 4), 4, 1);
    else u8g2.drawFrame(55 + (i * 4), 3, 3, 3);
  }

  // Indicatore attivita' pacchetti (barra lampeggiante)
  if (millis() - lastPacketTime < 120) {
    u8g2.drawHLine(52, 6, 10);
  }

  u8g2.sendBuffer();
}

void setup() {
  // Nessun Serial.begin() per evitare blocchi USB CDC in modalita' standalone
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  u8g2.begin();

  // Creazione coda FreeRTOS per i pacchetti ricevuti via ESP-NOW
  rxQueue = xQueueCreate(16, sizeof(struct_message));

  // Caricamento preferenze di rete da memoria NVS
  preferences.begin("net-config", true);
  targetPort = preferences.getUInt("port", DEFAULT_PORT);
  for (int i = 0; i < 4; i++) {
    char keyIP[8], keyLIP[8];
    snprintf(keyIP, sizeof(keyIP), "ip%d", i);
    snprintf(keyLIP, sizeof(keyLIP), "lip%d", i);
    targetIP[i] = preferences.getInt(keyIP, DEFAULT_TARGET_IP[i]);
    localIP[i]  = preferences.getInt(keyLIP, DEFAULT_LOCAL_IP[i]);
  }
  preferences.end();

  // Inizializzazione Wi-Fi / ESP-NOW a bassa latenza
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Disattiva modem sleep per risposta immediata (~1ms)
  WiFi.disconnect();

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, yellowAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }

  // Inizializzazione Bus SPI e Modulo Ethernet W5500
  SPI.begin(sck_pin, miso_pin, mosi_pin, cs_pin);
  Ethernet.init(cs_pin);
  Ethernet.begin(mac_eth, localIP);
  Udp.begin(targetPort);

  updateDisplay();
}

void loop() {
  unsigned long now = millis();

  // 1. Controllo timeout connessione radio con la Gialla
  if (now - lastMessageTime > LINK_TIMEOUT) {
    isLinked = false;
  }

  // 2. Elaborazione pacchetti ESP-NOW ricevuti (in modo thread-safe sul Core 1)
  struct_message rxMsg;
  while (rxQueue != NULL && xQueueReceive(rxQueue, &rxMsg, 0) == pdTRUE) {
    if (rxMsg.tipo == 'G' || (rxMsg.tipo == 'C' && strcmp(rxMsg.cmd, "SYNC_GROUPS") == 0)) {
      startZeroPluginGroupSync();
    } else if (rxMsg.tipo == 'C') {
      sendMA3Command(rxMsg.cmd);
    } else if (rxMsg.tipo == 'P') {
      feedbackData.tipo = 'A';
      esp_now_send(yellowAddress, (uint8_t *)&feedbackData, sizeof(feedbackData));
    } else if (rxMsg.tipo == 'S') {
      applyAndSaveNetworkSettings(rxMsg);
    }
  }

  // 2b. Avanzamento scansione batch Inline Lua per sincronizzazione 40 gruppi
  processZeroPluginGroupSync(now);

  // 3. Gestione non bloccante del tasto BOOT per cambio pagina OLED
  bool currentBtnState = digitalRead(BOOT_BUTTON_PIN);
  if (currentBtnState == LOW && lastBtnState == HIGH && (now - lastBtnDebounce > 180)) {
    lastBtnDebounce = now;
    displayPage = (displayPage + 1) % 4;
    updateDisplay();
    lastOledUpdate = now;
  }
  lastBtnState = currentBtnState;

  // 4. Ponte Inverso (UDP/OSC da GrandMA3 -> ESP-NOW verso Gialla)
  // Svuota tutti i pacchetti UDP presenti nel buffer W5500 prima del prossimo refresh OLED
  while (Udp.parsePacket() > 0) {
    memset(feedbackData.cmd, 0, sizeof(feedbackData.cmd));
    int len = Udp.read((uint8_t *)feedbackData.cmd, 79);
    if (len > 0) {
      feedbackData.cmd[len] = '\0';
      feedbackData.tipo = 'F';
      esp_now_send(yellowAddress, (uint8_t *)&feedbackData, sizeof(feedbackData));
      delayMicroseconds(800); // Pausa minima per non saturare la coda TX Wi-Fi ESP-NOW

      // Estrae il payload leggibile (es. "GN0:Spot") anche se preceduto da zeri OSC
      const char* preview = feedbackData.cmd;
      for (int i = 0; i < len - 1; i++) {
        if (feedbackData.cmd[i] == 'G' && feedbackData.cmd[i + 1] == 'N') {
          preview = &feedbackData.cmd[i];
          break;
        }
      }
      snprintf(lastCommand, sizeof(lastCommand), "FB:%.11s", preview);
      lastPacketTime = millis();
    }
  }

  // 5. Refresh periodico non bloccante del display OLED (60ms ~ 16 fps)
  if (now - lastOledUpdate >= OLED_REFRESH_MS) {
    lastOledUpdate = now;
    updateDisplay();
  }
}

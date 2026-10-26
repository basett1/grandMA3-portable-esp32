# 🌟 Presentazione del Progetto: Portable MAesp32

> *Questo documento contiene una presentazione pronta all'uso (in Italiano e in Inglese) pensata per la descrizione del repository GitHub, per un post di lancio su LinkedIn / community Lighting (GrandMA3, r/lightingdesign) o per il tuo portfolio professionale.*

---

## 🇮🇹 Versione Italiana (Per Portfolio / LinkedIn / Community)

### 🎛️ Portable MAesp32 — Il telecomando tascabile wireless per GrandMA3 (senza Wi-Fi!)

Chi lavora nel mondo del lighting live, teatrale o televisivo conosce bene la frustrazione di usare un tablet o uno smartphone via Wi-Fi sul palco durante il *focusing*: disconnessioni improvvise, canali Wi-Fi a 2.4/5 GHz saturi durante gli eventi, schermi che vanno in blocco e interfacce web scomode da usare con una mano sola mentre si è in scala o sul palco.

Per risolvere questo problema ho progettato e costruito **Portable MAesp32**: un sistema hardware e firmware completo, open-source e tascabile per controllare **GrandMA3** in tempo reale.

#### 💡 Come funziona?
Invece di appoggiarsi a un router Wi-Fi esterno, il sistema è diviso in due unità basate su **ESP32** che comunicano direttamente tra loro tramite il protocollo **ESP-NOW peer-to-peer** (con una latenza inferiore a **2 millisecondi**):

1. **🟣 Unità VIOLA (Bridge Ethernet <-> ESP-NOW)**  
   Basata su **ESP32-S3**, modulo Ethernet hardware **W5500** e mini-display **OLED 72×40**, montati su una **PCB custom** che ho disegnato in KiCad (con modello 3D e file Gerber pronti per la produzione). Si collega con un normale cavo di rete RJ45 allo switch della console GrandMA3 e traduce istantaneamente i pacchetti radio in comandi **UDP/OSC** nativi (`/cmd ,s`) e viceversa.

2. **🟡 Unità GIALLA (Controller Touch Tascabile)**  
   Basata su **ESP32-2432S028 (CYD)** con schermo touch da 2.8", interfaccia grafica custom a **6 pagine** e feedback visivo tramite LED RGB:
   - **Pagina 1 (Keypad Canali)**: tastierino rapido con scorciatoie *double-tap* (`++` per *At % +10*, `At At` per *At Normal*, `..` per *At 0*).
   - **Pagina 2 (Focus Utility)**: tasti rapidi `NEXT`, `PREV`, `HIGHLIGHT`, `SOLO` e `CLEAR`.
   - **Pagina 3 (Playback)**: controllo immediato di `GOTO`, `CUE` e `SEQUENCE`.
   - **Pagina 4 (Sync 40 Gruppi Bidirezionale Zero-Plugin)**: premendo `SYNC NAMES`, il Bridge inietta automaticamente via `/cmd` uno script Inline Lua su GrandMA3 che crea/configura una riga OSC dedicata (`MAesp32`) senza toccare le connessioni esistenti, legge i nomi reali dei primi 40 gruppi dallo showfile e li invia via rete/radio direttamente sui pulsanti del display touch — **senza dover installare alcun plugin sulla console**!
   - **Pagina 5 (Configurazione IP da Remoto)**: permette di cambiare l'IP del Bridge, l'IP della console MA3 e la porta OSC direttamente dal palmare touch, aggiornando il Bridge via radio senza mai dover collegare un PC.
   - **Pagina 6 (Gestione Energetica)**: controllo PWM della luminosità a 10 livelli e **standby automatico** dopo 30 secondi di inattività con risveglio istantaneo al tocco.

#### 🛠️ Cosa trovi nel repository GitHub:
- **Firmware V3 PRO (Zero-Plugin Edition)** per entrambe le unità (con code FreeRTOS thread-safe, iniezione Inline Lua a 8 micro-batch, display OLED full-buffer non bloccante e gestione touch edge-triggered).
- **Progetto KiCad 9.0 completo + File Gerber** per stampare la PCB del Bridge Ethernet + file **3D `.step` e `.3mf`**.

---

## 🇬🇧 English Version (For GitHub About / International Lighting Communities)

### 🎛️ Portable MAesp32 — Pocket-Sized Wireless Remote & Ethernet OSC Bridge for GrandMA3

Relying on standard Wi-Fi routers and web remotes on stage during focus sessions can be frustrating: crowded RF environments, dropped connections, phone lock-screens, and clunky interfaces.

**Portable MAesp32** is a purpose-built, ultra-low-latency hardware and firmware solution designed for **GrandMA3** consoles and onPC rigs. It eliminates external Wi-Fi routers entirely by combining a **wired Ethernet OSC Bridge** at FOH with a **pocket-sized 2.8" Touchscreen Remote** communicating over direct peer-to-peer **ESP-NOW (~1ms latency)**.

#### ✨ Key Highlights:
- **Two-Unit Architecture**:
  - **🟣 VIOLA (FOH Ethernet Bridge)**: Powered by an **ESP32-S3 + W5500 SPI Ethernet + 0.42" OLED (72×40)** mounted on a **custom-designed PCB** (KiCad 9 project, 3D STEP/3MF models, and production-ready Gerber files included). Converts ESP-NOW frames into native 4-byte aligned **GrandMA3 OSC UDP packets** (`/cmd ,s`).
  - **🟡 GIALLA (Handheld CYD Touch Controller)**: Powered by an **ESP32-2432S028 (2.8" 320×240 TFT Touchscreen)** featuring a custom **6-page lighting UI** and RGB LED tactile feedback.
- **Zero-Plugin Bidirectional Group Sync (40 Groups)**: No Lua plugins to import into the showfile! Press `SYNC NAMES` on Page 4 and the Bridge automatically injects an Inline Lua command over `/cmd` that non-destructively appends a dedicated `MAesp32` OSC line (leaving existing QLab/Companion OSC lines untouched) and streams Groups 1–40 from the active DataPool back to the handheld touchscreen.
- **Double-Tap Lighting Shortcuts**: Double-tap `+` for `At Percent + 10`, double-tap `At` for `At Normal`, or double-tap `.` for `At 0`.
- **Over-The-Air Network Setup**: Configure the Bridge Local IP, GrandMA3 Target IP, and OSC Port directly from Page 5 on the handheld unit and push the configuration wirelessly to the FOH Bridge.
- **Stage-Ready Firmware (V3 PRO)**: Built with FreeRTOS queues for SPI/Wi-Fi thread safety, zero dynamic heap fragmentation, flicker-free partial TFT redraws, and automatic 30-second backlight standby.

---

## 📋 Testo Breve per il campo "About" di GitHub (Max 160 caratteri)

> **IT:** Telecomando wireless tascabile (ESP32 CYD) + Bridge Ethernet OSC su PCB custom (ESP32-S3 + W5500) per console luci GrandMA3, con sync bidirezionale dei gruppi.
>
> **EN:** Pocket wireless touch remote (ESP32 CYD) & custom PCB Ethernet OSC Bridge (ESP32-S3 + W5500) for GrandMA3 lighting consoles with bidirectional group sync.

### 🏷️ Tag / Topics consigliati su GitHub:
`grandma3` `esp32` `esp-now` `osc` `lighting-control` `stage-lighting` `esp32-s3` `w5500` `cyd` `esp32-2432s028` `kicad` `lua` `dmx` `show-control`

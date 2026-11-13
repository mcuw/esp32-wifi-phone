# C6 Phone – WLAN-Telefon für Waveshare ESP32-C6-Touch-AMOLED-2.06

[![CI](https://github.com/mcuw/esp32-wifi-phone/actions/workflows/ci.yml/badge.svg)](https://github.com/mcuw/esp32-wifi-phone/actions/workflows/ci.yml)

Zwei (oder mehr) Geräte im selben WLAN, Kontaktliste per mDNS, Sprache per UDP.
ESP-IDF **v6.1**, Wi-Fi 6 mit **iTWT**, WLAN-Zugang per **softAP mit Captive Portal**, gespeichert im **NVS**.

## Architektur

```
Boot ─┬─ keine Zugangsdaten / BOOT gedrückt / "WLAN ändern" ──► Provisioning-Modus
      │     softAP "C6Phone-XXXX" (Zufallspasswort auf dem Display)
      │     Captive Portal: DNS :53 (alles → 192.168.4.1) + DHCP-Option 114
      │     HTTP :80   Astro + Qwik Seite (aus SPIFFS-Partition "www"),
      │                Host ≠ 192.168.4.1 → 302 auf http://192.168.4.1/
      │                GET /api/info · GET /api/scan · POST /api/wifi → NVS → Neustart
      └─ Zugangsdaten vorhanden ──► Telefon-Modus
            STA (HE20) → mDNS "_c6phone._udp" → Kontaktliste (LVGL)
            Anruf: UDP-Signalisierung :5070, Audio :5072 (16 kHz, IMA-ADPCM 64 kbit/s)
            Idle (20 s ohne Touch): Display aus + iTWT (~1 s Intervall)
            Anruf / Display an:     iTWT aus, PS_NONE
```

Provisioning und Telefon laufen nie gleichzeitig: Nach dem Speichern startet das Gerät neu.
Das spart RAM (kein PSRAM auf dem Board) und vermeidet Heap-Fragmentierung.

| Datei | Aufgabe |
|---|---|
| `main/net.c` | STA/softAP, HE-Protokoll, iTWT Setup/Teardown |
| `main/provision.c` | HTTP-Server: statische Seite, JSON-API, Portal-Weiterleitung |
| `main/json_util.c` | Kleiner JSON-Parser/-Escaper für die Setup-API |
| `main/dns_server.c` | Captive-Portal-DNS (beantwortet jede A-Anfrage mit der AP-Adresse) |
| `main/storage.c` | NVS: SSID, Passwort, Gerätename, Setup-Flag |
| `main/board.c` | Display/Touch über BSP; ES8311 + ES7210 + I2S direkt (Pins aus `docs/hardware.md`) |
| `main/peers.c` | mDNS-Ankündigung und -Suche |
| `main/call.c` | Anruf-Zustandsmaschine, Jitter-Buffer, ADPCM, Klingelton |
| `main/ui.c` | Kontakte, Anruf, Einstellungen, Provisioning-Anzeige |
| `web/` | Astro + Qwik Einrichtungsseite, `src/lib/api.ts` = kleiner `fetch`-Client |

## Bauen

```bash
# 1. Web-UI bauen (erzeugt web/dist, wird in die Partition "www" geflasht)
cd web && npm install && npm run build && cd ..

# 2. Firmware (ESP-IDF v6.1, z. B. installiert mit: eim install -i v6.1)
idf.py set-target esp32c6
idf.py build flash monitor
```

Falls `npm install` bei `@qwikdev/astro` Versionskonflikte meldet: `npx astro add @qwikdev/astro`
ausführen (setzt passende Versionen). Es gibt auch die Qwik-v2-Variante `@qwik.dev/astro` + `@qwik.dev/core`.

## Tests und CI

```bash
sh test/host/run.sh                    # C-Tests (ADPCM, JSON, DNS) mit ASan/UBSan, ohne ESP-IDF
(cd web && npm test)                   # Test des Web-Clients gegen einen Mock-Server (Node >= 22)
```

GitHub Actions (`.github/workflows/ci.yml`), bei Push auf `main`, Pull Requests und Tags `v*`:

1. **Host tests**: obige Tests.
2. **Web UI**: `npm run build`, prüft, dass `dist/` in die ~1,9-MiB-Partition `www` passt.
3. **Firmware**: baut mit `espressif/esp-idf-ci-action` (ESP-IDF `v6.1`, Ziel `esp32c6`) und lädt die
   flashbaren Dateien als Artefakt hoch. Flashen: Artefakt entpacken, dann
   `esptool --chip esp32c6 write_flash @flash_args`.
4. **Release** (nur bei Tag `v*`): hängt die Firmware als ZIP an ein GitHub-Release.

Sobald lokal einmal gebaut wurde, `web/package-lock.json` und `dependencies.lock` committen:
Die CI nutzt dann `npm ci` und feste Komponentenversionen.

## Einrichten

1. Gerät starten. Ohne gespeicherte Zugangsdaten zeigt das Display SSID und Passwort des softAP.
2. Mit dem Handy/Laptop diesem WLAN beitreten. **Die Einrichtungsseite öffnet sich automatisch**
   (Captive Portal; auf Android als „Im Netzwerk anmelden“-Benachrichtigung bzw. Fenster).
   Falls nicht: `http://192.168.4.1` im Browser öffnen.
3. Netz wählen, Passwort und Gerätenamen eingeben, speichern.
4. Gerät startet neu und tritt dem WLAN bei. Jedes Gerät braucht einen eigenen Namen.

API direkt testen (Laptop im softAP):

```bash
curl http://192.168.4.1/api/info
curl http://192.168.4.1/api/scan
curl -X POST -d '{"ssid":"MeinNetz","pass":"geheim1234","name":"Anna"}' http://192.168.4.1/api/wifi
```

## Verifikationsstand (bitte lesen)

**Geprüft:**
- Pin-Map, Codecs (ES8311 Lautsprecher, ES7210 Dual-Mikro), CO5300/FT3168 gegen das Waveshare-Repo (`docs/hardware.md`) und das Schaltbild.
- ESP-IDF v6.1 ist veröffentlicht; iTWT-API (`esp_wifi_sta_itwt_setup`, `esp_wifi_sta_itwt_teardown`, `WIFI_EVENT_ITWT_SETUP`) gegen Espressif-Quellen abgeglichen.
- Auf dem Host getestet: `api.ts` gegen einen Mock-Server, ADPCM-Roundtrip (30,7 dB SNR bei 20 % Paketverlust), JSON-Parser mit Anführungszeichen/Backslash.

- Auf dem Host getestet: DNS-Antwortbau (A mit EDNS-Zusatzrecord, AAAA → leere Antwort, kaputte Pakete werden ignoriert).

**Nicht geprüft (kein ESP-IDF, kein Netz, keine Hardware in meiner Umgebung):**
- **Die Firmware wurde nicht kompiliert und nicht auf Hardware ausgeführt.** Rechne mit einer Runde Compilerfehler.
- **BSP-API:** Ich konnte die Header von `waveshare/esp32_c6_touch_amoled_2_06` v2 nicht lesen. `board.c` nutzt die übliche Espressif-BSP-Konvention (`bsp/esp-bsp.h`, `bsp_display_start()`, `bsp_display_lock/unlock()`, `bsp_display_brightness_set()`). Namen ggf. anpassen; alles Board-spezifische steckt in `board.c`.
- **I²C-Port:** `board.c` greift mit `i2c_master_get_bus_handle(I2C_NUM_0)` auf den BSP-Bus zu. Falls das BSP einen anderen Port nutzt: `BOARD_I2C_PORT` setzen.
- **BOOT-Taste** ist als GPIO 9 angenommen. Gegen das Schaltbild prüfen.
- **TWT-API unter v6.1:** `wifi_twt_setup_config_t` samt Feldern wird vom v6.1-Compiler akzeptiert (CI-Log). Der Setup-Status im Event ist ein `esp_err_t` (1 = Erfolg), `WIFI_BW_HT20` heißt seit v6.0 `WIFI_BW20`, und `esp_wifi_he.h` muss explizit eingebunden werden. Ob iTWT zur Laufzeit klappt, ist weiterhin ungetestet.
- **Komponenten in IDF 6:** Falls eine Kernkomponente (z. B. `spiffs`) in die Registry verschoben wurde, mit `idf.py add-dependency` nachziehen.
- **Captive Portal auf echten Geräten:** Ablauf folgt dem Muster des IDF-Beispiels `captive_portal` (DNS-Umleitung + DHCP-Option 114 per `ESP_NETIF_CAPTIVEPORTAL_URI`), aber ich konnte kein Android/iOS/Windows-Gerät testen. Include-Pfad `dhcpserver/dhcpserver.h` für `dhcps_offer_t` ggf. anpassen.
- **Astro/Qwik-Build** wurde nicht ausgeführt (kein npm-Zugriff). Die Dateien sind nach der Dokumentation der Integration aufgebaut, Versionen sind nicht festgenagelt.

## Bekannte Grenzen und Entwurfsentscheidungen

- **iTWT und Anrufe widersprechen sich.** iTWT spart Strom, kostet aber Latenz. Deshalb nur im Idle: Ein eingehender Anruf wartet bis zu ~1 s auf den nächsten Service Period (der Anrufer sendet INVITE bis zu ~8 s lang erneut). Während eines Anrufs ist iTWT abgeschaltet. iTWT funktioniert nur, wenn der Router Wi-Fi 6 mit TWT-Responder kann; sonst fällt das Gerät auf Modem-Sleep zurück (Log-Warnung).
- **Portal erscheint nicht?** Android „Privates DNS“ auf *Automatisch* stellen (fest eingestelltes DNS-over-TLS umgeht die Umleitung), mobile Daten aus, ggf. WLAN vergessen und neu verbinden. Die Probe-Antworten sind absichtlich nie „online“, damit das Portal geöffnet bleibt.
- **Nur 2,4 GHz.** Der ESP32-C6 hat kein 5-GHz-Funkmodul. Das Netz muss 2,4 GHz mit Wi-Fi 6 anbieten.
- **mDNS-Multicast ist unter iTWT unzuverlässig.** Die Kontaktsuche läuft deshalb nur bei eingeschaltetem Display.
- **Kein Echo-Canceller.** Die Platine hat zwar einen ES7210 mit Referenzpfad, aber ich habe keine Software-AEC für den C6 verifiziert. `call.c` dämpft das Mikro per einfachem Ducking, wenn die Gegenseite laut ist. Für Freisprechen reicht das nur bedingt; Hörer-Betrieb mit niedriger Lautstärke ist robuster. Die zwei Mikrofone werden nur gemittelt (kein Beamforming).
- **Keine Verschlüsselung der Gespräche.** Signalisierung und Audio gehen unverschlüsselt per UDP durchs LAN. Für mehr als ein Heimnetz wären SRTP/DTLS nötig. Die Provisioning-Daten laufen über das WPA2-geschützte softAP-Netz (Zufallspasswort pro Boot), nicht über TLS.
- **Zugangsdaten liegen im NVS im Klartext.** Für Produktion: Flash-Encryption + NVS-Verschlüsselung aktivieren.
- **Umlaute** im Gerätenamen erscheinen auf dem Display nicht (Montserrat ohne Latin-1). Eigenen LVGL-Font mit Umlauten generieren, wenn nötig.
- **Aufwecken per Touch** löst im ersten Moment auch den Button unter dem Finger aus. Ein „erste Berührung nur wecken“-Filter fehlt noch.
- **Speicher:** 512 KB SRAM ohne PSRAM, LVGL-Puffer + Wi-Fi + mDNS + Audio sind knapp. Bei Heap-Problemen zuerst Puffergrößen im BSP und Task-Stacks prüfen (`heap_caps_print_heap_info`).
- Ein Anruf zugleich, keine Konferenz, keine Anrufliste.

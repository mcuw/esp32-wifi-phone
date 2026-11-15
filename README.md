# C6 Phone – WLAN-Telefon für Waveshare ESP32-C6-Touch-AMOLED-2.06

[![CI](https://github.com/mcuw/esp32-wifi-phone/actions/workflows/ci.yml/badge.svg)](https://github.com/mcuw/esp32-wifi-phone/actions/workflows/ci.yml)

Zwei (oder mehr) Geräte im selben WLAN, Kontaktliste per mDNS, Sprache per UDP.
ESP-IDF **v6.1**, Wi-Fi 6 mit **iTWT**, WLAN-Zugang per **softAP mit Captive Portal**, gespeichert im **NVS**.

**Status:** Firmware und Einrichtungsseite bauen (lokal und in der CI), die Firmware läuft auf dem Board.

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
| `main/board.c` | Display/Touch über BSP; ES8311 + ES7210 + I2S direkt (Pins aus dem Waveshare-Repo/Schaltbild) |
| `main/peers.c` | mDNS-Ankündigung und -Suche |
| `main/call.c` | Anruf-Zustandsmaschine, Jitter-Buffer, ADPCM, Klingelton |
| `main/ui.c` | Kontakte, Anruf, Einstellungen, Provisioning-Anzeige |
| `web/` | Astro 7 + Qwik 2 Einrichtungsseite, `src/lib/api.ts` = kleiner `fetch`-Client |

### Flash-Aufteilung (16 MB, `partitions.csv`)

| Partition | Größe | Inhalt |
|---|---|---|
| `ota_0`, `ota_1` | je 3 MiB | App-Slots für spätere OTA-Updates |
| `nvs` | 24 KiB | Zugangsdaten, Gerätename (für HMAC-basierte NVS-Verschlüsselung vorbereitet) |
| `otadata`, `phy_init` | 8 + 4 KiB | OTA-Status, PHY-Kalibrierung |
| `coredump` | 64 KiB | Absturzabbild (ELF), `encrypted` |
| `www` | ~1,8 MiB | SPIFFS mit dem Build der Einrichtungsseite (`web/dist`) |
| `storage` | 8 MiB | LittleFS für Anwendungsdaten (braucht `joltwallet/littlefs`) |

Die Partitionstabelle liegt bei `0xF000`, damit ein Bootloader mit Secure Boot V2 + Flash-Encryption
davor passt.

## Bauen

```bash
# 1. Web-UI bauen (erzeugt web/dist, wird in die Partition "www" geflasht)
cd web && npm ci && npm run build && cd ..

# 2. Firmware (ESP-IDF v6.1, z. B. installiert mit: eim install -i v6.1)
idf.py set-target esp32c6
idf.py build flash monitor
```

`idf.py flash` schreibt Bootloader, Partitionstabelle, App und das SPIFFS-Abbild `www` in einem Schritt.
`web/dist` muss deshalb vor dem Firmware-Build existieren.

Versionen sind festgenagelt: `web/package-lock.json` (Astro 7, `@qwik.dev/astro`, `@qwik.dev/core` 2.x)
und `dependencies.lock` (ESP-IDF-Komponenten) sind eingecheckt. Komponenten aktualisieren:
`idf.py update-dependencies`, Web-Pakete: `npm update` in `web/`, danach die Lockfiles committen.

## Tests und CI

```bash
sh test/host/run.sh                    # C-Tests (ADPCM, JSON, DNS) mit ASan/UBSan, ohne ESP-IDF
(cd web && npm test)                   # Test des Web-Clients gegen einen Mock-Server (Node >= 22)
```

GitHub Actions (`.github/workflows/ci.yml`), bei Push auf `main`, Pull Requests und Tags `v*`:

1. **Host tests**: obige Tests.
2. **Web UI**: `npm run build`, prüft, dass `dist/` in die ~1,8-MiB-Partition `www` passt.
3. **Firmware**: baut mit `espressif/esp-idf-ci-action` (ESP-IDF `v6.1`, Ziel `esp32c6`) und lädt die
   flashbaren Dateien als Artefakt hoch. Flashen: Artefakt entpacken, dann
   `esptool --chip esp32c6 write_flash @flash_args`.
4. **Release** (nur bei Tag `v*`): hängt die Firmware als ZIP an ein GitHub-Release.

Die CI nutzt dank der eingecheckten Lockfiles `npm ci` und feste Komponentenversionen.

### Bench-Test: Anruf auf ein schlafendes Telefon

`test/bench/` prüft auf dem Testbench mit zwei Telefonen, dass ein Anruf ein Telefon im iTWT-Sparmodus
erreicht (Issue #5). Der Test läuft nicht in der CI. Der Anrufer bekommt eine Firmware mit
**Auto-Wählen** (`CONFIG_PHONE_TEST_AUTODIAL`, Overlay `sdkconfig.autodial`). Sie ruft den ersten
Kontakt aus der mDNS-Liste an, so als würde man auf den Kontakt tippen, und legt nach 30 s wieder auf.
In normalen Builds ist das Auto-Wählen nicht enthalten.

```bash
idf.py build                                               # Angerufener (normale Firmware) -> build/
idf.py -B build-autodial -DSDKCONFIG=build-autodial/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.autodial" build   # Anrufer -> build-autodial/
pip install pytest
pytest test/bench -v --wt-url http://<bench>:8080 \
       --caller-slot SLOT2 --caller-mac <mac-anrufer> \
       --callee-slot SLOT1 --callee-mac <mac-angerufener>
```

Der Test prüft die MACs, flasht nur die App (NVS mit den WLAN-Daten bleibt) und startet beide Boards
neu, damit der ARP-Cache des Anrufers leer ist. Dann wertet er zwei Anrufe aus:

- Der Angerufene schlief (`iTWT agreed`).
- RINGING kommt innerhalb von 2 s.
- Der statische ARP-Eintrag steht ab dem ersten INVITE.
- Der Angerufene schläft nach dem Auflegen innerhalb von 25 s wieder.

Zum Schluss flasht er dem Anrufer wieder die normale Firmware. Dauer: etwa 5 min.

Bei jedem Lauf, ob bestanden oder nicht, speichert der Test die seriellen Logs beider Boards unter
`test/bench/logs/<zeit>-caller-<slot>.log` und `…-callee-<slot>.log` (anderer Ort: `--log-dir`). Vor
jeder Zeile steht die Uhrzeit des Testbench. So lassen sich beide Dateien nebeneinander lesen, etwa
gesendetes INVITE beim Anrufer gegen empfangenes beim Angerufenen. Bei einem Fehlschlag nennt pytest
die Pfade.

Zur Fehlersuche lohnt der Blick in diese Logzeilen:

- `call: sig tx/rx …` mit `arp yes/NO`: Ist die MAC der Gegenstelle bekannt?
- `call: static ARP …`
- `net: iTWT agreed (…)` mit den ausgehandelten Parametern und `net: iTWT torn down`
- `autodial: …`

Die Parameter des Auto-Wählens (Zielname, Wartezeit, Klingeldauer, Pause, Anzahl) lassen sich per
`menuconfig` unter „Telefon: Testfunktionen“ ändern. Bei `ROUNDS=0` wählt der Anrufer endlos, für
Langzeittests.

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

## Absturzabbild (Core Dump)

Bei einem Absturz schreibt die Firmware ein Abbild in die Partition `coredump`. Ohne
Flash-Encryption liest man es per USB mit `idf.py coredump-info`. Mit Flash-Encryption sieht USB nur
Chiffretext; dann holt man es über den Einrichtungsmodus (softAP, Passwort steht auf dem Display):

```bash
curl http://192.168.4.1/api/coredump                    # Zusammenfassung (Task, PC, Ursache, ELF-SHA)
curl -o coredump.bin http://192.168.4.1/api/coredump.bin
esp-coredump info_corefile -t raw -c coredump.bin build/c6_phone.elf
curl -X DELETE http://192.168.4.1/api/coredump          # danach löschen
```

`elf_sha` bzw. `same_fw` zeigen, ob das Abbild zur laufenden Firmware passt. Zum Auswerten braucht es
die `c6_phone.elf` genau des abgestürzten Builds (liegt im CI-Artefakt). **Das Abbild enthält
RAM-Inhalte, also möglicherweise das WLAN-Passwort: vertraulich behandeln und nach dem Auswerten löschen.**

**Nach einer Änderung der Partitionstabelle** bleiben Daten an den verschobenen Stellen liegen, denn
`idf.py flash` schreibt nur Bootloader, Partitionstabelle, App, `otadata` und `www`. Steht in der
Partition `coredump` dann etwas anderes als ein Absturzabbild, meldet ESP-IDF beim Booten
`Incorrect size of core dump image`. Die Firmware löscht die Partition in diesem Fall einmalig
(Log `coredump partition held no core dump … erased`), ab dem nächsten Boot ist die Meldung weg.
Sauberer ist es, nach einer Layoutänderung einmal den ganzen Flash zu löschen. Das löscht auch NVS,
das Telefon muss danach neu eingerichtet werden:

```bash
idf.py erase-flash flash          # bzw. esptool --chip esp32c6 erase-flash
```

## Verifikationsstand

**Erledigt:**
- **Firmware baut** mit ESP-IDF v6.1 für `esp32c6` (lokal und in der CI) und **läuft auf dem Board**
  (Waveshare ESP32-C6-Touch-AMOLED-2.06).
- **Einrichtungsseite baut** (Astro 7 + Qwik 2) und passt in die Partition `www`.
- BSP `waveshare/esp32_c6_touch_amoled_2_06` v2, `esp_codec_dev` und `mdns` lösen auf und sind in
  `dependencies.lock` festgehalten.
- iTWT-API unter v6.1: `wifi_twt_setup_config_t`, `esp_wifi_sta_itwt_setup/teardown` und
  `WIFI_EVENT_ITWT_SETUP` kompilieren. Hinweise zur v6-API: Der Setup-Status im Event ist ein
  `esp_err_t`, `WIFI_BW_HT20` heißt seit v6.0 `WIFI_BW20`, `esp_wifi_he.h` muss explizit eingebunden werden.
- Pin-Map, Codecs (ES8311 Lautsprecher, ES7210 Dual-Mikro), CO5300/FT3168 gegen das Waveshare-Repo und das Schaltbild abgeglichen.
- Host-Tests: `api.ts` gegen einen Mock-Server, ADPCM-Roundtrip (30,7 dB SNR bei 20 % Paketverlust),
  JSON-Parser mit Anführungszeichen/Backslash, DNS-Antwortbau (A mit EDNS-Zusatzrecord, AAAA → leere
  Antwort, kaputte Pakete werden ignoriert).

**Noch gezielt zu prüfen:**
- **iTWT zur Laufzeit:** Kommt die Vereinbarung mit dem Router zustande (Log `WIFI_EVENT_ITWT_SETUP`),
  und wie hoch ist der Stromverbrauch im Idle?
- **Captive Portal** auf verschiedenen Geräten (Android, iOS, Windows, macOS).
- **Anrufqualität:** Latenz, Echo/Ducking, Verhalten bei Paketverlust im echten WLAN.
- **BOOT-Taste** als GPIO 9 (Einstieg in den Einrichtungsmodus beim Start).
- **Core-Dump-Abruf** über `/api/coredump` mit aktivierter Flash-Encryption.

## Bekannte Grenzen und Entwurfsentscheidungen

- **iTWT und Anrufe widersprechen sich.** iTWT spart Strom, kostet aber Latenz. Deshalb nur im Idle: Ein eingehender Anruf wartet bis zu ~1 s auf den nächsten Service Period (der Anrufer sendet INVITE bis zu ~10 s lang erneut). Ein schlafendes Telefon empfängt keine Broadcasts, also auch keine ARP-Anfragen. Deshalb veröffentlicht jedes Telefon seine MAC per mDNS, und der Anrufer trägt sie für die Dauer des Anrufs als statischen ARP-Eintrag ein (Issue #5). Während eines Anrufs ist iTWT abgeschaltet. iTWT funktioniert nur, wenn der Router Wi-Fi 6 mit TWT-Responder kann; sonst fällt das Gerät auf Modem-Sleep zurück (Log-Warnung).
- **Portal erscheint nicht?** Android „Privates DNS“ auf *Automatisch* stellen (fest eingestelltes DNS-over-TLS umgeht die Umleitung), mobile Daten aus, ggf. WLAN vergessen und neu verbinden. Die Probe-Antworten sind absichtlich nie „online“, damit das Portal geöffnet bleibt.
- **Nur 2,4 GHz.** Der ESP32-C6 hat kein 5-GHz-Funkmodul. Das Netz muss 2,4 GHz mit Wi-Fi 6 anbieten.
- **mDNS-Multicast ist unter iTWT unzuverlässig.** Die Kontaktsuche läuft deshalb nur bei eingeschaltetem Display.
- **Kein Echo-Canceller.** Die Platine hat zwar einen ES7210 mit Referenzpfad, aber ich habe keine Software-AEC für den C6 verifiziert. `call.c` dämpft das Mikro per einfachem Ducking, wenn die Gegenseite laut ist. Für Freisprechen reicht das nur bedingt; Hörer-Betrieb mit niedriger Lautstärke ist robuster. Die zwei Mikrofone werden nur gemittelt (kein Beamforming).
- **Keine Verschlüsselung der Gespräche.** Signalisierung und Audio gehen unverschlüsselt per UDP durchs LAN. Für mehr als ein Heimnetz wären SRTP/DTLS nötig. Die Provisioning-Daten laufen über das WPA2-geschützte softAP-Netz (Zufallspasswort pro Boot), nicht über TLS.
- **Zugangsdaten liegen im NVS im Klartext.** Geplant: NVS-Verschlüsselung im HMAC-Schema (`CONFIG_NVS_ENCRYPTION`, `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`, `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID`). Fehlt der eFuse-Schlüssel, erzeugt und brennt die Firmware ihn beim ersten Start (irreversibel). Eine `nvs_keys`-Partition braucht dieses Schema nicht.
- **Umlaute** im Gerätenamen erscheinen auf dem Display nicht (Montserrat ohne Latin-1). Eigenen LVGL-Font mit Umlauten generieren, wenn nötig.
- **Aufwecken per Touch** löst im ersten Moment auch den Button unter dem Finger aus. Ein „erste Berührung nur wecken“-Filter fehlt noch.
- **Speicher:** 512 KB SRAM ohne PSRAM, LVGL-Puffer + Wi-Fi + mDNS + Audio sind knapp. Bei Heap-Problemen zuerst Puffergrößen im BSP und Task-Stacks prüfen (`heap_caps_print_heap_info`).
- Ein Anruf zugleich, keine Konferenz, keine Anrufliste.

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
      │     HTTP :80   Astro + Qwik Seite (in die App eingebettet),
      │                Host ≠ 192.168.4.1 → 302 auf http://192.168.4.1/
      │                GET /api/info · GET /api/scan · POST /api/wifi → NVS → Neustart
      └─ Zugangsdaten vorhanden ──► Telefon-Modus
            STA (HE20) → mDNS "_c6phone._udp" → Kontaktliste (LVGL)
            Anruf: UDP-Signalisierung :5070, Audio :5072 (16 kHz, IMA-ADPCM 64 kbit/s)
            Idle (20 s ohne Touch): Display aus + iTWT (~1 s Intervall)
            Anruf / Display an:     iTWT aus, PS_NONE
            Firmware-Update: Manifest der Releases prüfen, nach Tippen installieren (main/ota.c)
```

Provisioning und Telefon laufen nie gleichzeitig: Nach dem Speichern startet das Gerät neu.
Das spart RAM (kein PSRAM auf dem Board) und vermeidet Heap-Fragmentierung.

| Datei | Aufgabe |
|---|---|
| `main/net.c` | STA/softAP, HE-Protokoll, iTWT Setup/Teardown |
| `main/provision.c` | HTTP-Server: statische Seite, JSON-API, Portal-Weiterleitung |
| `main/ota.c` | Firmware-Update aus den GitHub Releases, Rollback-Bestätigung |
| `main/version.c` | Versionsvergleich (Git-Tag, z. B. `v0.2.0` oder `v0.2.0-3-gabc1234`) |
| `main/json_util.c` | Kleiner JSON-Parser/-Escaper für die Setup-API |
| `main/dns_server.c` | Captive-Portal-DNS (beantwortet jede A-Anfrage mit der AP-Adresse) |
| `main/storage.c` | NVS: SSID, Passwort, Gerätename, Setup-Flag |
| `main/board.c` | Display/Touch über BSP; ES8311 + ES7210 + I2S direkt (Pins aus dem Waveshare-Repo/Schaltbild) |
| `main/peers.c` | mDNS-Ankündigung und -Suche |
| `main/call.c` | Anruf-Zustandsmaschine, Jitter-Buffer, ADPCM, Klingelton |
| `main/ui.c` | Kontakte, Anruf, Einstellungen, Provisioning-Anzeige |
| `web/` | Astro 7 + Qwik 2 Einrichtungsseite, `src/lib/api.ts` = kleiner `fetch`-Client; `tools/pack_web.py` bettet `web/dist` in die App ein |

### Flash-Aufteilung (16 MB, `partitions.csv`)

| Partition | Größe | Inhalt |
|---|---|---|
| `ota_0`, `ota_1` | je 3 MiB | App-Slots für Firmware-Updates (App ~1,7 MB inkl. Einrichtungsseite) |
| `nvs` | 24 KiB | Zugangsdaten, Gerätename (für HMAC-basierte NVS-Verschlüsselung vorbereitet) |
| `otadata`, `phy_init` | 8 + 4 KiB | OTA-Status, PHY-Kalibrierung |
| `coredump` | 64 KiB | Absturzabbild (ELF), `encrypted` |
| `www` | ~1,8 MiB | frei (früher die Einrichtungsseite, die steckt seit Issue #16 in der App) |
| `storage` | 8 MiB | LittleFS für Anwendungsdaten (braucht `joltwallet/littlefs`) |

Die Partitionstabelle liegt bei `0xF000`, damit ein Bootloader mit Secure Boot V2 + Flash-Encryption
davor passt.

## Bauen

```bash
# 1. Web-UI bauen (erzeugt web/dist, wird beim Firmware-Build in die App eingebettet)
cd web && npm ci && npm run build && cd ..

# 2. Firmware (ESP-IDF v6.1, z. B. installiert mit: eim install -i v6.1)
idf.py set-target esp32c6
idf.py build flash monitor
```

`idf.py flash` schreibt Bootloader, Partitionstabelle, `otadata` und App in einem Schritt.
`web/dist` muss vor dem Firmware-Build existieren, sonst fehlt die Einrichtungsseite (CMake-Warnung).

Die Firmware-Version kommt aus dem Git-Tag (`git describe`, z. B. `v0.2.0-3-gabc1234`) und steht im
Einstellungen-Bildschirm und in `/api/info`.

Versionen sind festgenagelt: `web/package-lock.json` (Astro 7, `@qwik.dev/astro`, `@qwik.dev/core` 2.x)
und `dependencies.lock` (ESP-IDF-Komponenten) sind eingecheckt. Komponenten aktualisieren:
`idf.py update-dependencies`, Web-Pakete: `npm update` in `web/`, danach die Lockfiles committen.

## Tests und CI

```bash
sh test/host/run.sh                    # C-Tests (ADPCM, JSON, DNS, Version) mit ASan/UBSan, ohne ESP-IDF
(cd web && npm test)                   # Test des Web-Clients gegen einen Mock-Server (Node >= 22)
```

GitHub Actions (`.github/workflows/ci.yml`), bei Push auf `main`, Pull Requests und Tags `v*`:

1. **Host tests**: obige Tests.
2. **Web UI**: `npm run build`, prüft, dass `dist/` unter 512 KiB bleibt (wird in die App eingebettet).
3. **Firmware**: schreibt die Version aus `git describe` nach `version.txt`, baut mit
   `espressif/esp-idf-ci-action` (ESP-IDF `v6.1`, Ziel `esp32c6`) und lädt die
   flashbaren Dateien als Artefakt hoch. Flashen: Artefakt entpacken, dann
   `esptool --chip esp32c6 write_flash @flash_args`.
4. **Release** (nur bei Tag `v*`): prüft, dass das Image die Tag-Version trägt, und hängt an das
   GitHub-Release die Firmware als ZIP, das App-Image `c6_phone.bin` und das Update-Manifest
   `ota.json`. Tags mit Bindestrich (`v1.0.0-rc1`) werden Pre-Releases und keinem Telefon angeboten.

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

## Firmware aktualisieren

Das Telefon holt Updates aus den [GitHub Releases](https://github.com/mcuw/esp32-wifi-phone/releases).
Es prüft eine Minute nach dem Verbinden mit dem WLAN, danach alle 24 Stunden und auf Knopfdruck
(**Einstellungen → Nach Update suchen**). Gibt es eine neuere Version, steht sie dort mit dem Knopf
**Update … installieren**. Installiert wird nur nach diesem Tippen, nie während eines Anrufs. Während
des Updates zeigt das Display den Fortschritt, und eingehende Anrufe bekommen „besetzt“.

- **Quelle:** `releases/latest/download/ota.json` mit `{"version":"v1.2.3","url":".../c6_phone.bin"}`.
  Die URL ist per `menuconfig` einstellbar (**Telefon: Firmware-Update (OTA)**), etwa für eigene
  Builds mit eigener Update-Quelle.
- **Prüfung:** Angeboten wird nur eine höhere Release-Version (keine Pre-Releases). Das geladene Image
  muss zu diesem Projekt gehören und die Version aus dem Manifest tragen. Die Echtheit stützt sich
  vorerst auf HTTPS zu GitHub. Signierte Images kommen erst mit Secure Boot.
- **Rollback:** Eine neue Firmware startet „auf Probe“ und gilt erst als gültig, wenn WLAN, mDNS und
  Signalisierung laufen (oder der Einrichtungsmodus gestartet ist). Schafft sie das nicht innerhalb
  von 5 min oder stürzt sie vorher ab, startet der Bootloader wieder die vorherige Firmware. Diese
  Version bietet das Telefon danach nicht noch einmal an.
- **Einmal per USB:** Der Rollback steckt im Bootloader, und den ändert ein Update nicht. Geräte mit
  einer Firmware vor Issue #16 brauchen deshalb einmal `idf.py flash` (Bootloader, Partitionstabelle,
  `otadata`, App). Die WLAN-Daten im NVS bleiben erhalten.
- **USB bleibt offen:** Es gibt kein Secure Boot, keine Flash-Verschlüsselung und keine eFuse-Änderung,
  Flashen per USB funktioniert wie bisher.
- **Achtung beim Flashen nur der App:** Nach einem Update startet das Telefon aus `ota_1`. Wer dann nur
  `c6_phone.bin` nach `0x10000` (`ota_0`) schreibt, sieht weiter die alte Firmware. `idf.py flash` bzw.
  `esptool … write_flash @flash_args` schreibt auch `ota_data_initial.bin` und startet wieder aus `ota_0`.

### Bench-Test: Update und Rollback

`test/bench/test_ota.py` spielt auf dem Testbench ein Update durch. Manifest und Images kommen dabei
aus der Firmware-Ablage des Testbench statt von GitHub. Die Test-Images bauen mit dem Overlay
`sdkconfig.otatest` (`CONFIG_PHONE_TEST_OTA`: installieren ohne Tippen, Manifest über `http://`):

| Image | Version | Besonderheit |
|---|---|---|
| A | `v0.0.1` | wählt zusätzlich automatisch (wie `sdkconfig.autodial`), wird per USB geflasht |
| B | `v0.0.2` | |
| C | `v0.0.3` | meldet sich nie als gültig (`CONFIG_PHONE_TEST_OTA_NEVER_VALID`) |

```bash
echo 'CONFIG_PHONE_OTA_MANIFEST_URL="http://<bench>:8080/firmware/c6phone-otatest/ota.json"' \
     > sdkconfig.otatest.local       # Adresse des Testbench im WLAN der Telefone, nicht im Repo
test/bench/build_ota.sh              # -> build-ota-a, build-ota-b, build-ota-c
pytest test/bench/test_ota.py -v --wt-url http://<bench>:8080 \
       --caller-slot SLOT2 --caller-mac <mac-prüfling> \
       --callee-slot SLOT1 --callee-mac <mac-gegenstelle>
```

Der Prüfling bekommt A komplett per USB (mit Bootloader und `ota_data_initial`). Die Gegenstelle muss
nur die normale Telefon-Firmware haben. Geprüft wird:

- A findet B, lehnt die Installation während des eigenen Anrufs ab und installiert B nach dem Auflegen.
- B startet aus `ota_1` und meldet sich als gültig.
- B bietet die eigene Version nicht an.
- B installiert C. C meldet sich nie als gültig, nach 60 s startet wieder B. B meldet den Rollback und
  bietet C nicht noch einmal an.
- Keine unerwarteten Fehlerzeilen, kein Absturz.

Zum Schluss flasht der Test die normale Firmware aus `build/` zurück (`--restore-dir`). Dauer: etwa
6 min. Das Log liegt unter `test/bench/logs/<zeit>-ota-<slot>.log`.

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
`idf.py flash` schreibt nur Bootloader, Partitionstabelle, App und `otadata`. Steht in der
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
- **Einrichtungsseite baut** (Astro 7 + Qwik 2) und ist in die App eingebettet (~48 KB gzip).
- BSP `waveshare/esp32_c6_touch_amoled_2_06` v2, `esp_codec_dev` und `mdns` lösen auf und sind in
  `dependencies.lock` festgehalten.
- iTWT-API unter v6.1: `wifi_twt_setup_config_t`, `esp_wifi_sta_itwt_setup/teardown` und
  `WIFI_EVENT_ITWT_SETUP` kompilieren. Hinweise zur v6-API: Der Setup-Status im Event ist ein
  `esp_err_t`, `WIFI_BW_HT20` heißt seit v6.0 `WIFI_BW20`, `esp_wifi_he.h` muss explizit eingebunden werden.
- Pin-Map, Codecs (ES8311 Lautsprecher, ES7210 Dual-Mikro), CO5300/FT3168 gegen das Waveshare-Repo und das Schaltbild abgeglichen.
- Host-Tests: `api.ts` gegen einen Mock-Server, ADPCM-Roundtrip (30,7 dB SNR bei 20 % Paketverlust),
  JSON-Parser mit Anführungszeichen/Backslash, DNS-Antwortbau (A mit EDNS-Zusatzrecord, AAAA → leere
  Antwort, kaputte Pakete werden ignoriert).

**Auf zwei Boards am Testbench geprüft** (Stand November 2026, Wi-Fi-6-Router mit WPA3-SAE und PMF):
- **WLAN-Anmeldung:** 12 von 12 Neustarts beim ersten Versuch verbunden, IP nach 3,7–4,2 s (#6).
- **iTWT zur Laufzeit:** Der Router nimmt die Anfrage an (Log `iTWT agreed (… cmd 4 …)`), Intervall
  ~1,0 s (`123*2^13 us`), Wachphase 32,8 ms, unangekündigt, ohne Trigger.
- **Anruf auf ein schlafendes Telefon:** RINGING nach 0,2–0,35 s, auch mit leerem ARP-Cache beim
  Anrufer (#5). Nach dem Gespräch oder einem unbeantworteten Klingeln über 20 s schläft das Telefon
  nach ~21 s wieder. Automatisch geprüft durch den [Bench-Test](#bench-test-anruf-auf-ein-schlafendes-telefon).
- **Mehrere Anrufe seit dem Booten:** kein Absturz, keine Fehlermeldung (df093ef, #8). Ein
  angenommenes Gespräch funktioniert auch beim zweiten Anruf (von Hand geprüft).
- **Core Dump:** Ein Absturz wird in die Partition `coredump` geschrieben und beim nächsten Boot mit
  korrekter Prüfsumme gefunden. Fremddaten in der Partition werden einmalig gelöscht (#7).

**Noch gezielt zu prüfen:**
- **Stromverbrauch** im Idle mit iTWT, im Vergleich zu Modem-Sleep (bisher nicht gemessen).
- **Captive Portal** auf verschiedenen Geräten (Android, iOS, Windows, macOS).
- **Anrufqualität:** Latenz, Echo/Ducking, Verhalten bei Paketverlust im echten WLAN.
- **BOOT-Taste** als GPIO 9 (Einstieg in den Einrichtungsmodus beim Start).
- **Core-Dump-Abruf** über `/api/coredump` (ohne und mit Flash-Encryption).

## Bekannte Grenzen und Entwurfsentscheidungen

- **iTWT und Anrufe widersprechen sich.** iTWT spart Strom, kostet aber Latenz. Deshalb nur im Idle: Ein eingehender Anruf wartet bis zu ~1 s auf den nächsten Service Period (der Anrufer sendet INVITE bis zu ~10 s lang erneut). Ein schlafendes Telefon empfängt keine Broadcasts, also auch keine ARP-Anfragen. Deshalb veröffentlicht jedes Telefon seine MAC per mDNS, und der Anrufer trägt sie für die Dauer des Anrufs als statischen ARP-Eintrag ein (Issue #5). Während eines Anrufs ist iTWT abgeschaltet. iTWT funktioniert nur, wenn der Router Wi-Fi 6 mit TWT-Responder kann; sonst fällt das Gerät auf Modem-Sleep zurück (Log-Warnung).
- **Portal erscheint nicht?** Android „Privates DNS“ auf *Automatisch* stellen (fest eingestelltes DNS-over-TLS umgeht die Umleitung), mobile Daten aus, ggf. WLAN vergessen und neu verbinden. Die Probe-Antworten sind absichtlich nie „online“, damit das Portal geöffnet bleibt.
- **Nur 2,4 GHz.** Der ESP32-C6 hat kein 5-GHz-Funkmodul. Das Netz muss 2,4 GHz mit Wi-Fi 6 anbieten.
- **mDNS-Multicast ist unter iTWT unzuverlässig.** Die Kontaktsuche läuft deshalb nur bei eingeschaltetem Display.
- **Gastnetz mit Client-Isolation geht nicht.** Isolation blockiert Multicast (Kontaktsuche per mDNS) und direkten Unicast (Signalisierung und Audio). Die Telefone finden sich dann nie („Suche Telefone im WLAN …“). Im Router für das Gastnetz den Zugriff aufs Heimnetz bzw. Intranet erlauben oder ein normales WLAN nehmen. Das ist vor jeder Fehlersuche in der Firmware zu prüfen.
- **Kein Echo-Canceller.** Die Platine hat zwar einen ES7210 mit Referenzpfad, aber ich habe keine Software-AEC für den C6 verifiziert. `call.c` dämpft das Mikro per einfachem Ducking, wenn die Gegenseite laut ist. Für Freisprechen reicht das nur bedingt; Hörer-Betrieb mit niedriger Lautstärke ist robuster. Die zwei Mikrofone werden nur gemittelt (kein Beamforming).
- **Keine Verschlüsselung der Gespräche.** Signalisierung und Audio gehen unverschlüsselt per UDP durchs LAN. Für mehr als ein Heimnetz wären SRTP/DTLS nötig. Die Provisioning-Daten laufen über das WPA2-geschützte softAP-Netz (Zufallspasswort pro Boot), nicht über TLS.
- **Zugangsdaten liegen im NVS im Klartext.** Geplant: NVS-Verschlüsselung im HMAC-Schema (`CONFIG_NVS_ENCRYPTION`, `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`, `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID`). Fehlt der eFuse-Schlüssel, erzeugt und brennt die Firmware ihn beim ersten Start (irreversibel). Eine `nvs_keys`-Partition braucht dieses Schema nicht.
- **Umlaute** im Gerätenamen erscheinen auf dem Display nicht (Montserrat ohne Latin-1). Eigenen LVGL-Font mit Umlauten generieren, wenn nötig.
- **Aufwecken per Touch** löst im ersten Moment auch den Button unter dem Finger aus. Ein „erste Berührung nur wecken“-Filter fehlt noch.
- **Speicher:** 512 KB SRAM ohne PSRAM, LVGL-Puffer + Wi-Fi + mDNS + Audio sind knapp. Bei Heap-Problemen zuerst Puffergrößen im BSP und Task-Stacks prüfen (`heap_caps_print_heap_info`).
- Ein Anruf zugleich, keine Konferenz, keine Anrufliste.

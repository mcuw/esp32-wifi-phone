# Erinnerungsanruf vom PC

`erinnerung.py` ruft ein C6-Telefon an und spricht eine Nachricht, etwa „Morgen um zehn Uhr hast du
den Termin beim Zahnarzt“. Der PC verhält sich dabei wie ein weiteres Telefon. Er spricht dasselbe
UDP-Protokoll wie `main/call.c` (Signalisierung :5070, Audio :5072, 16 kHz IMA-ADPCM). Die Firmware
muss dafür nicht geändert werden.

```
Text ──► F5-TTS (Stimme aus einer Probe, GPU) ──► WAV ──► 16 kHz ──► ADPCM ──► Anruf
```

Ablauf eines Anrufs: INVITE alle 0,7 s, bis das Telefon klingelt. Danach wird bis zu 40 s gewartet.
Nimmt jemand ab, folgen 1 s Stille, dann die Nachricht zweimal mit 1,5 s Pause, danach legt der PC
auf. Nimmt niemand ab, beendet der PC das Klingeln mit BYE und versucht es auf Wunsch später noch
einmal.

## Einrichten (Windows-PC mit NVIDIA-Karte)

Python 3.10–3.12, im Ordner `tools/erinnerung`:

```bash
python -m venv .venv
```

```bash
.venv\Scripts\activate
```

PyTorch mit CUDA, Befehl passend zur Treiberversion von <https://pytorch.org/get-started/locally/>:

```bash
pip install torch torchaudio --index-url https://download.pytorch.org/whl/cu128
```

```bash
pip install -r requirements.txt
```

Der erste Lauf lädt die Modelle von Hugging Face: F5-TTS, den Vocoder und für die Stimmprobe Whisper.
Das sind einige GB. Auf einer RTX 4060 (8 GB) dauert ein Satz danach wenige Sekunden.

### Deutsches Modell

Das F5-Grundmodell kann nur Englisch und Chinesisch. Für Deutsch braucht es ein deutsch trainiertes
Checkpoint, z. B. [aihpi/F5-TTS-German](https://huggingface.co/aihpi/F5-TTS-German). Die Dateinamen
für Checkpoint und `vocab.txt` und die Architektur (`F5TTS_Base` oder `F5TTS_v1_Base`) stehen auf
der Modellkarte. Entweder die Dateien herunterladen und lokal angeben oder direkt als
`hf://aihpi/F5-TTS-German/<pfad-zur-datei>`. Andere Modelle lassen sich über `--tts-befehl`
einbinden (siehe unten).

### Stimmprobe

F5-TTS spricht mit der Stimme einer Probe: 5–15 s, eine Person, klar, ohne Musik und Hall, als WAV.
Selbst aufnehmen ist nicht nötig. Es geht jede Aufnahme, die man verwenden darf, etwa aus einem
frei lizenzierten deutschen Sprachdatensatz (die Thorsten-Voice-Datensätze sind CC0; Lizenz der
konkreten Datei prüfen). Die Stimme einer echten Person bitte nur mit ihrem Einverständnis verwenden.

Fehlt `--ref-text`, schreibt F5-TTS die Probe selbst mit Whisper ab. Wer den Text kennt, gibt ihn an.
Das ist genauer.

### Voreinstellungen

`erinnerung.beispiel.json` nach `erinnerung.json` kopieren und die Pfade eintragen. Die Datei ist in
`.gitignore`. Relative Pfade gelten relativ zur JSON-Datei. Jede Option der Kommandozeile kann dort
stehen, mit `_` statt `-`.

## Benutzen

```bash
python erinnerung.py --liste
```

```bash
python erinnerung.py --text "Morgen um zehn Uhr hast du den Termin beim Zahnarzt." --nur-erzeugen probe.wav
```

`probe.wav` klingt so wie am Telefon (16 kHz). `probe.original.wav` ist die volle Qualität aus dem
Modell. So lässt sich die Stimme ausprobieren, ohne jemanden anzurufen.

```bash
python erinnerung.py --an Kueche --text "Morgen um zehn Uhr hast du den Termin beim Zahnarzt." --versuche 3 --pause-min 10
```

| Option | Bedeutung |
|---|---|
| `--an NAME` / `--ip IP [--mac MAC]` | Ziel per mDNS-Name oder direkt |
| `--text` / `--wav` | Was gesagt wird |
| `--wiederholen 2` | Nachricht so oft abspielen |
| `--klingeln 40` | Sekunden klingeln lassen |
| `--versuche 1`, `--pause-min 5` | Neue Versuche, wenn niemand abnimmt |
| `--anrufer-name Erinnerung` | Name auf dem Display des Telefons (ohne Umlaute) |
| `--tempo 0.9` | Langsamer sprechen |
| `--lautstaerke 0.7` | Spitzenpegel |
| `--tts befehl --tts-befehl "…{text}…{out}"` | Anderes TTS-Programm, schreibt eine WAV nach `{out}` |

Rückgabewert, z. B. für die Windows-Aufgabenplanung: `0` zugestellt, `3` nicht angenommen,
`4` abgelehnt oder besetzt, `5` nicht erreichbar, `6` vor dem Ende der ersten Wiedergabe aufgelegt.

Erzeugte Sprache wird unter `~/.cache/c6-erinnerung` zwischengespeichert (anderer Ort:
`C6_TTS_CACHE`). Ein zweiter Versuch mit demselben Text rechnet also nicht neu.

## Wichtig im Netz

- **Admin-Rechte für schlafende Telefone.** Ein Telefon im iTWT-Schlaf beantwortet keine
  ARP-Anfragen (siehe Haupt-README, Issue #5). Wie die Telefone untereinander trägt das Skript
  deshalb die MAC aus mDNS kurz als statischen ARP-Eintrag ein. Dafür braucht es Admin-Rechte:
  unter Windows die Konsole „Als Administrator ausführen“, unter Linux `sudo`. Ohne Admin-Rechte
  erscheint eine Warnung. Der Anruf klappt dann nur, wenn das Telefon gerade wach ist.
- **Gleiches Netz.** Der PC muss im selben Netz sein wie die Telefone. Per Kabel am selben Router
  ist auch in Ordnung, ein Gastnetz mit Client-Isolation dagegen nicht.
- **Firewall.** Beim ersten Start fragt Windows evtl. nach UDP-Zugriff für Python: im
  **privaten** Netz erlauben.
- **mDNS.** Die Namenssuche (`--an`, `--liste`) findet schlafende Telefone unzuverlässig
  (Haupt-README). Für feste Aufgaben besser `--ip` und `--mac` angeben; beides zeigt `--liste`.
  Im Router eine feste IP für das Telefon vergeben.

## Tests

```bash
python -m unittest discover -s tools/erinnerung -v
```

Die Tests prüfen den ADPCM-Encoder bitgenau gegen `main/adpcm.c` und die Umrechnung auf 16 kHz. Den
Anruf spielen sie gegen ein simuliertes Telefon auf `127.0.0.2` durch: angenommen, abgelehnt, nicht
angenommen, nicht erreichbar, vorzeitig aufgelegt. Sie laufen auch in der CI. Am echten Telefon ist
das Werkzeug noch nicht geprüft.

## Ausblick

- Terminliste oder Kalender (ICS) auslesen und automatisch vorher anrufen.
- Antwort verstehen: Das Telefon schickt während des Anrufs sein Mikrofon zurück. `Caller.heard`
  sammelt das schon (16 kHz). Mit Whisper ließe sich daraus „ja“ oder „später nochmal“ erkennen.
  Wegen der Echo-Unterdrückung im Telefon erst nach dem Ende der Ansage.

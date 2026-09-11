# badge-schwammintelligenz

## Ideensammlung
- PCB mit Gehirn-Form und Schwamm-Farbe
  - gelber Lötstopplack
  - nacktes FR4 als Kontrastfarbe für Porösität des Schwammes
  - Konturlinien in schwarzem oder weißem Bestückungsdruck
- viele sehr kleine LEDs auf der Vorderseite
  - blitzen auf, um Neuronen zu mimen
  - mit [Charlieplexing](https://en.wikipedia.org/wiki/Charlieplexing#Origin) kann man 110 LEDs mit nur 11 Pins einzeln ansteuern
    - (`nLedsMax = nGpio * (nGpio - 1)`)
    - [Beispielcode für Charlieplexing](https://goodarduinocode.com/guides/charlieplexing) -> Zeilenweise ansteuern
  - [XINGLIGHT XL0201UWC](https://jlcpcb.com/partdetail/XINGLIGHT-XL0201UWC/C965789) wäre z.B. nur 0,7 mm * 0,4 mm groß (Pads komplett unter dem Package) und kostet nur 0,0326 USD / St. bei 5000 St. Abnahme
- Kompatibilität zu Mesh-Cat-Ears?
  - [Rudelblinken](https://github.com/zebreus/rudelblinken)
  - Die laufen mit BLE auf einem ESP32-C3
  - Wir könnten denselben Controller nutzen, um mit den Katzenohren meshen zu können
- ein Taster auf der Rückseite, um einen Puls auszulösen
  - erstmal an den eigenen LEDs
  - langfristig sonst auch auf Gehirnen in der Nähe
  - evtl. jeder Mesh-Teilnehmer eine eigene LED?
  - 3-4 Taster (Soft touch) (Typ [Alps SKPM](https://tech.alpsalpine.com/e/products/detail/SKPMANE010/)?)

## Implementierung der Farben

Die Platine wird mit **gelbem Lötstopplack** und **weißem Bestückungsdruck** gefertigt.

Kupfer auf der Rückseite minimal halten und wo möglich unter dem Kupfer der Oberseite verlegen.

Der Entwurf sieht folgendermaßen aus:

![von vorne](./pcb/Front.png)

![von hinten](./pcb/Back.png)

### Farbaufbau

| Farbe | Objekte | Beteiligte Lagen | Notizen |
| --- | --- | --- | --- |
| weiß | ??? | F.Silkscreen | Bestückungsdruck (muss auf Lötstopplack liegen) |
| silber | ??? | F.Cu, F.Mask | freies Kupfer mit HASL Finish |
| hellergelb | Schwammoberfläche | F.Mask | blankes FR4 (kein Kupfer auf der Rückseite) - ca. `e9d645ff` |
| hellgelb | Vertiefung flach | none | Lötstopplack auf FR4 (kein Kupfer auf der Rückseite) - ca. `f6c50eff` |
| dunkelgelb | Vertiefung tief | F.Cu | Lötstopplack auf Kupfer - ca. `e8b203ff` |
| grüngelb | --- | F.Cu, F.Mask | blankes FR4 mit Kupfer auf der Rückseite |

### Lagenaufbau

| Lage | Objekte |
| --- | --- |
| F.Cu | union( silber, dunkelgelb ) |
| F.Mask | invert( union( weiß, hellgelb, dunkelgelb ) ) |
| F.Silkscreen | weiß |


## BOM

Die folgenden Preise beziehen sich auf ein einzelnes Badge wenn wir Material für 50 Stück kaufen.

- JLCPCB Preis vom 2026-06-15
  - PCB ca. 100 mm x 100 mm
  - Silkscreen gleb
  - 1,6 mm dick
  - LeadFree HASL
  - je 110 St. XINGLIGHT XL-0201UWC (LCSC C965789) von JLCPCB auf der Vorderseite bestückt
  - 258,80 € (Material und Produktion) + 48,03 € (Versand EuroPacket) = 306,83 €
  - Es ist mit 2 Wochen Lieferzeit zu rechnen, wenn es keine Rücksprachen braucht

| Anzahl | Preis | Bezeichnung |
| --- | --- | --- |
| 1 | 6,14 € | PCB mit LEDs (Stand 2026-06-15 von JLCPCB teilbestückt) |
| 1 | ca. 4 € | ESP32-C3 |
| ? | ??? | ??? |

## Firmware

Liegt in `firmware/` und läuft auf dem Waveshare ESP32-C3-Zero.

**Für alle, die einfach ein Badge haben:** Auf https://schwammhirn.c3fl.de/ gibt es alles in
einer Seite. Badge per USB anstecken, „Firmware flashen“ klicken, Port wählen. Danach per
Bluetooth verbinden und steuern. Keine Installation nötig, Chrome oder Edge am Desktop reicht
(zum Steuern auch Chrome auf Android, auf dem iPhone der Browser „Bluefy“).

**Zum Mitentwickeln:** Bauen wahlweise mit der Arduino IDE (Board „Waveshare ESP32-C3-Zero“,
Bibliothek NimBLE-Arduino 2.x) oder mit PlatformIO im Repo-Root: `pio run -t upload`.

- `firmware/led_map.h` wird von `tools/gen_led_map.py` aus `pcb/pcb.kicad_pcb` erzeugt
  (Pin-Paar und Position jeder LED). Nach Layoutänderungen neu ausführen, das Skript
  schreibt auch `web/led_map.js`.
- Ein Timer-Interrupt scannt die Charlieplex-Matrix mit 4 Bit Helligkeit, `loop()`
  rendert Animationen in einen Framebuffer.
- Taster: kurz drücken löst eine Welle aus (auch auf Badges in Reichweite), lang drücken
  wechselt den Modus (Neuronen, Wellen, Laufschrift, Löt-Test). Im Löt-Test startet kurz
  drücken den Durchlauf von vorne. Alle drei Taster liegen auf demselben Eingang.
- Sync: Badges senden Modus, Tempo, Helligkeit, Schriftlayout und Phase per BLE-Advertising.
  Die höchste Änderungsgeneration gewinnt, bei Gleichstand die kleinere ID. Kein
  Verbindungsaufbau nötig. Der Text selbst wird nicht übertragen.
- BLE-Anzeigename ist standardmäßig `Schwammhirn-<ID>` aus der Bluetooth-MAC, änderbar.
  Die Web-Seite findet Badges über die Herstellerkennung im Werbepaket, unabhängig vom Namen.

### Modi

| Modus | Kommando | Was passiert |
| --- | --- | --- |
| Neuronen | `mode neurons` | zufällige Blitze, die ausklingen (Standard) |
| Wellen | `mode pulse` | Ringe laufen von einem zufälligen Punkt nach außen |
| Laufschrift | `mode text` | Text läuft durch, Größe und Abstand oben einstellbar |
| Löt-Test | `mode test` | füllt jede Zeile LED für LED, dann dunkel, nächste Zeile; danach dasselbe spaltenweise. Jede neue LED wird gemeldet. Fehlt eine LED, ist es die Lötstelle; fehlt eine Zeile oder Spalte, der GPIO oder sein 10-Ω-Widerstand. Wird nicht synchronisiert und nicht gespeichert. |
| Malen | `mode remote` | zeigt ein Rohbild, das per `frame` gesendet wurde (Web-Seite: LEDs antippen) |

### Kommandos (Serial und BLE)

Dieselben Textkommandos gehen über Serial (115200 Baud, USB) und über BLE. BLE nutzt den
Nordic-UART-Service (Schreiben auf `6E400002-…`, Antworten als Notify auf `6E400003-…`),
jede BLE-Terminal-App funktioniert also, zum Beispiel „Serial Bluetooth Terminal“ auf Android.
Eine Zeile pro Kommando, Antwort beginnt mit `ok` oder `err`.

| Kommando | Bedeutung |
| --- | --- |
| `mode <name>` oder `mode <0-4>` | Modus wählen: `neurons`, `pulse`, `text`, `test`, `remote` |
| `text <Text>` | Laufschrift setzen, max. 63 Zeichen, Umlaute werden ersetzt, schaltet auf Laufschrift |
| `tsize <1-5>` | Schriftgröße (Leinwand-Pixel pro Font-Pixel), Standard 3 |
| `ty <0-30>` | Abstand der Schrift vom oberen Rand, Standard 6 |
| `speed <0-100>` | Tempo aller Animationen, Standard 4 |
| `bright <1-15>` | Helligkeit, Standard 15 |
| `pulse` oder `pulse <LED 0-109>` | Welle auslösen, auch auf Badges in Reichweite. Im Löt-Test: Durchlauf neu starten |
| `frame <220 Hex-Zeichen>` | Rohbild, ein Byte Helligkeit pro LED in D-Reihenfolge, schaltet auf Malen |
| `name <Name>` | BLE-Anzeigename, max. 20 Zeichen, leer = Standard `Schwammhirn-<ID>` |
| `sync on` / `sync off` | Einstellungen von Badges in der Nähe übernehmen oder ignorieren |
| `mirror on` / `mirror off` | Framebuffer etwa 10-mal pro Sekunde als `F<220 Hex>` per BLE-Notify streamen (Live-Bild der Web-Seite) |
| `status` | Zustand ausgeben: Modus, Tempo, Helligkeit, Schriftlayout, Text, Sync, Generation, ID, Badges in Reichweite, Name |

Modus, Tempo, Helligkeit, Text, Schriftlayout und Name bleiben über Neustarts erhalten.

### Web-Steuerung

Unter https://schwammhirn.c3fl.de/ oder lokal aus `web/index.html`. „Mit Badge verbinden“
drücken, dann Modi, Text, Regler und Umbenennen. Zeigt live das Bild des Badges, im Modus
„Malen“ lassen sich LEDs antippen. Ohne Verbindung läuft dieselbe Animation als Simulation
1:1 auf der Platinenkontur, so lässt sich zum Beispiel Laufschrift vorab beurteilen.
Web Bluetooth braucht HTTPS oder localhost, deshalb funktioniert Verbinden aus dem
Dateisystem nur in Chrome, das `file://` als sicher behandelt.

Firmware flashen ohne Toolchain: Button „Firmware flashen“ (ESP Web Tools, Chrome/Edge am
Desktop). Die Binaries liegen in `web/flash/`, die Seite zeigt den Commit-Hash der Version.
Nach einer Firmware-Änderung: `pio run`, Code committen, dann `tools/export_firmware.sh` und
die Binaries als eigenen Commit nachschieben.

Deployment: Das `Dockerfile` im Repo-Root liefert `web/` per nginx aus. In Coolify eine
Ressource vom Typ „Public Repository“ mit Build Pack „Dockerfile“ anlegen, Port 80, Domain
eintragen. HTTPS macht Coolify per Let's Encrypt. Nach jedem Merge einmal redeployen.

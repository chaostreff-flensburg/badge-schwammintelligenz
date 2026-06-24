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



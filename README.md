# badge-schwammintelligenz

## Ideensammlung
- PCB mit Gehirn-Form und Schwamm-Farbe
  - gelber Lötstopplack
  - nacktes FR4 als Kontrastfarbe für Porösität des Schwammes
  - Konturlinien in schwarzem oder weißem Bestückungsdruck
- viele sehr kleine LEDs auf der Vorderseite
  - blitzen auf, um Neuronen zu mimen
  - mit [Charlieplexing](https://en.wikipedia.org/wiki/Charlieplexing#Origin) kann man 110 LEDs mit nur 11 Pins einzeln ansteuern (`nLedsMax = nGpio * (nGpio - 1)`)
  - [XINGLIGHT XL0201UWC](https://jlcpcb.com/partdetail/XINGLIGHT-XL0201UWC/C965789) wäre z.B. nur 0,7 mm * 0,4 mm groß (Pads komplett unter dem Package) und kostet nur 0,0326 USD / St. bei 5000 St. Abnahme
- Kompatibilität zu Mesh-Cat-Ears?
  - Die laufen möglicherweise mit ESP-NOW auf einem bestimmten ESP32 (citation needed)
  - Wir könnten denselben Controller nutzen, um mit den Katzenohren meshen zu können



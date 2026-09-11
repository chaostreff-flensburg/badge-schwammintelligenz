#!/bin/sh
# Sammelt die Binaries des letzten PlatformIO-Builds nach web/flash/ und schreibt das
# Manifest für ESP Web Tools. Vorher: pio run. Danach committen, dann kann jeder unter
# https://schwammhirn.c3fl.de/ im Browser flashen.
set -e
cd "$(dirname "$0")/.."
BUILD=.pio/build/esp32c3zero
BOOT_APP0=$(find ~/.platformio/packages/framework-arduinoespressif32 -name boot_app0.bin | head -1)
cp "$BUILD/bootloader.bin" "$BUILD/partitions.bin" "$BUILD/firmware.bin" "$BOOT_APP0" web/flash/
VERSION=$(git describe --always --dirty)
cat > web/flash/manifest.json <<JSON
{
  "name": "Schwammhirn",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-C3",
      "parts": [
        { "path": "bootloader.bin", "offset": 0 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "boot_app0.bin", "offset": 57344 },
        { "path": "firmware.bin", "offset": 65536 }
      ]
    }
  ]
}
JSON
echo "web/flash/ aktualisiert, Version $VERSION"

// This is built for use on an ESP32-C3-Zero dev board soldered to one of the boards from this same repository

constexpr int matrixPins[] = {5, 6, 4, 7, 3, 8, 2, 9, 1, 0, 20};
constexpr int flashChance = 64; // 0 (least probable) ... 255 (most probable)

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  for (int i = 0; i < sizeof(matrixPins); ++i)
  {
    // init all matrix pins to High-Z
    pinMode(matrixPins[i], INPUT);
  }
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
}

void loop()
{
  for (int iCol = 0; iCol < sizeof(matrixPins); ++iCol)
  {
    // iterate columns
    digitalWrite(matrixPins[iCol], HIGH);
    pinMode(matrixPins[iCol], OUTPUT);

    for (int iRow = 0; iRow < sizeof(matrixPins); ++iRow)
    {
      if (iCol != iRow)
      {
        auto rand8Bit = static_cast<int>(random() % 256);
        if (rand8Bit < flashChance)
        {
          digitalWrite(matrixPins[iRow], LOW);
          pinMode(matrixPins[iRow], OUTPUT);
        }
      }
    }

    digitalWrite(LED_BUILTIN, LOW);
    delay(1);
    digitalWrite(LED_BUILTIN, HIGH);
    
    for (int iPin = 0; iPin < sizeof(matrixPins); ++iPin)
    {
      // reset all matrix pins to High-Z
      pinMode(matrixPins[iPin], INPUT);
    }
  }
}

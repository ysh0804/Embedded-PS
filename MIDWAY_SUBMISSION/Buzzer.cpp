#include "Buzzer.h"

void buzzerTone(uint8_t pin, uint16_t duration, uint16_t interval)
{
  for (int i = 0; i < duration; ++i)
  {
    REG_WRITE(GPIO_OUT_W1TS_REG, 1<<pin);
    delayMicroseconds(interval);
    REG_WRITE(GPIO_OUT_W1TC_REG, 1<<pin);
    delayMicroseconds(interval);
  }
}
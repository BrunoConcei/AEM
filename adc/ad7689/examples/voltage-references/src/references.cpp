// This example illustrates the different settings for input polarity, differential measurements and voltage references,
// and how they can be set. Note: external reference voltages require modifications to the AD7689 shield.

#include <ad7689.h>

AD7689 *adc;
const uint8_t AD7689_SS_pin = 10;
uint8_t ch_cnt = 0; // channel counter

void setup() {
  // first initialize the serial debugger
  Serial.begin(115200);
  while(!Serial);

  // AD7689 connected through SPI with SS specified in constructor
  adc = new AD7689(AD7689_SS_pin);
  if (adc->selftest())
  {
    Serial.println("AD7689 connected and ready");
  } else {
    Serial.println("Error: couldn't connect to AD7689. Check wiring.");
    while (1);
  }

  // SETTINGS FOR POLARITY
  //   UNIPOLAR_MODE:      a positive input voltage referenced to ground
  //   BIPOLAR_MODE:       a positive input voltage referenced to half the set reference level
  //   DIFFERENTIAL_MODE:  difference between 2 subsequent channels, even positive and odd nevative, referenced to ground

  // SETTINGS FOR REFERENCES
  //   REF_INTERNAL:       use internal bandgap reference, either 2.5V or 4.096V
  //   REF_EXTERNAL:       use external reference input on the REFIN pin, amplified to the REF pin
  //   REF_GND:            reference with respect to ADC ground
  //   REF_COM:            reference with respect to common mode input
  // note: when reference is set to internal with another voltage level than 2.5V or 4.096V, the ADC will default to 4.096V
  adc->setReference(REF_INTERNAL, 4.096, UNIPOLAR_MODE, false);
}

void loop() {
  Serial.print("AD7689 voltage input "+ String(ch_cnt)+" :");
  Serial.println(adc->acquireChannel(ch_cnt, NULL), DEC);
  ch_cnt = (ch_cnt + 1) % 8;
  delay(200);
}

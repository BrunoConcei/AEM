#include "ad7689.h"

void AD7689::configureSequencer() {
  // load a new configuration with the setting specified by the user
  AD7689_conf sequence = getADCConfig();

  // turn on sequencer if it hasn't been turned on yet, and set it to read temperature too
  sequence.SEQ_conf = SEQ_SCAN_INPUT_TEMP;
  // disable readback
  sequence.RB_conf = false;
  // overwrite existing command
  sequence.CFG_conf = true;

  // convert ADC configuration to command word
  uint16_t command = toCommand(sequence);

  // send command to configure ADC and enable sequencer
  shiftTransaction(command, false, NULL);

  // skip a frame
  shiftTransaction(0, false, NULL);

  // remember that the sequencer is active to prevent unnecessary intializations
  sequencerActive = true;
}

// configure the voltage reference
void AD7689::setReference(uint8_t refSource, float posRef, uint8_t polarity, bool differential) {

    // set input configuration and negative reference
    if (differential) {
      if (polarity == BIPOLAR_MODE) {
        // bipolar differential pairs, INx- referenced to Vref/2
        inputConfig = INCC_BIPOLAR_DIFF;
        negref = posRef / 2;
      }  else {
        // unipolar differential pairs, INx- referenced to GND
        inputConfig = INCC_UNIPOLAR_DIFF; // default differential
        negref = 0;
      }

    } else { // single ended
      if (polarity == BIPOLAR_MODE) {
        // bipolar single ended, INx referenced to COM = Vref/2
        inputConfig = INCC_BIPOLAR_COM;
        negref = posRef / 2;

      } else { // unipolar
        // unipolar, Inx referenced to GND
        inputConfig = INCC_UNIPOLAR_REF_GND;
        negref = 0;
      }
    }

    // set positive reference
    if (refSource == REF_INTERNAL)
    {
      if (posRef == INTERNAL_25) {
        refsrc = INT_REF_25;
        posref = INTERNAL_25;
      }
      else if (posRef == INTERNAL_4096) {
        refsrc = INT_REF_4096;
        posref = INTERNAL_4096;
      }
      else {
        posref = INTERNAL_4096;
        refsrc = INT_REF_4096; // default to 4.096V internal voltage reference
      }
    } else { // external reference
      refsrc = EXT_REF_TEMP_BUF;
      posref = posRef;
    }

}

// enable filtering to reduce bandwidth to 25%
void AD7689::enableFiltering(bool onOff) {
  filterConfig = onOff;
}


// calculate a voltage from the sample using reference voltages
float AD7689::acquireChannel(uint8_t channel, uint32_t* timeStamp) {
  if (micros() > (timeStamps[channel] + framePeriod * (TOTAL_CHANNELS - 1))) { // sequence outdated, acquire a new one
    uint8_t cycles = 1;
    if (channel < 2) cycles++; // double sequence to update first 2 channels

    // run 1 or 2 sequences depending on the outdated channel
    for (uint8_t cycle = 0; cycle < cycles; cycle++)
      readChannels(inputCount, ((inputConfig == INCC_BIPOLAR_DIFF) || (inputConfig == INCC_UNIPOLAR_DIFF)), samples, &curTemp);
  }

  *timeStamp = timeStamps[channel];

  return calculateVoltage(samples[channel], posref, negref);
}

// convert sample to voltage
float AD7689::calculateVoltage(uint16_t sample, float posRef, float negRef) {
  //Serial.println("calculateVoltage:" + String(sample) +","+String(posref) +","+String(negref));
  return (sample * (posRef - negRef) / TOTAL_STEPS);
}

// convert sample to temperature
float AD7689::calculateTemp(uint16_t temp) {
  // calculate temperature from ADC value:
  // output is 283 mV @ 25°C, and sensitivity of 1 mV/°C
  //return BASE_TEMP + ((temp * posref / TOTAL_STEPS)- TEMP_BASE_VOLTAGE) * TEMP_RICO;
  return temp;
}

// return absolute temperature
float AD7689::acquireTemperature() {
  if (micros() > (tempTime + framePeriod * (TOTAL_CHANNELS - 1)))  // temperature outdated, acquire a new one
    readChannels(inputCount, ((inputConfig == INCC_BIPOLAR_DIFF) || (inputConfig == INCC_UNIPOLAR_DIFF)), &samples[0], &curTemp);

  return calculateTemp(curTemp);
}

// constructor, intialize SPI and set SS pin
// set up the speed, mode and endianness of each device
// MODE0: SCLK idle low (CPOL=0), MOSI read on rising edge (CPHI=0)
// use CPHA = CPOL = 0
// SPI runs at max CPU clock as long as it is below 38 MHz
AD7689::AD7689(uint8_t SSpin, uint8_t numberChannels) : AD7689_settings (F_CPU >= MAX_FREQ ? MAX_FREQ : F_CPU, MSBFIRST, SPI_MODE0) ,
                                                        AD7689_PIN(SSpin),
                                                        inputCount(numberChannels)  {
  // initialize SPI transceiver
  // if it's already active, then this doesn't do anything
  SPI.begin();

  pinMode(SSpin, OUTPUT);    // configure slave select pin as output (not controlled by SPI transceiver)
  digitalWrite(SSpin, HIGH); // slave select active low

  // set default configuration options
  inputConfig = INCC_UNIPOLAR_REF_GND;  // default to unipolar mode with negative reference to ground
  refConfig = INT_REF_4096;             // internal 4.096V reference
  filterConfig = false;                 // full bandwidth

  // measure how long it takes to complete a 16-bit r/w cycle using current F_CPU for accurate sample timing
  cycleTimingBenchmark();

  // reset sample time stamps and force an update sequence at the next read command
  initSampleTiming();
  sequencerActive = false; // sequencer disabled by default

  // set reference source and voltage to the most commonly used values
  setReference(REF_INTERNAL, INTERNAL_4096, UNIPOLAR_MODE, false);
}

// measures the time required to transceive a complete 16 bit frame, using the current CPU clock speed
// this is required to generate accurate time stamps
// should be called once when starting the ADC, or whenever the clock frequency is changed (i.e. dynamic clock switching)
void AD7689::cycleTimingBenchmark() {

  uint32_t startTime = micros(); // record current CPU time
  uint16_t data;
  // make 10 transactions, then average the duration
  for (uint8_t trans = 0; trans < 10; trans++)
    data += shiftTransaction(toCommand(getADCConfig(false)), false, NULL); // default configuration, no readback

  framePeriod = (micros() - startTime) / 10;
  lastSeqEndTime = startTime;
}

// reads voltages from selected channels, always read temperature too
// params:
//   channels: last channel to read (starting at 0, max 7, in differential mode always read even number of channels)
//   mode: unipolar, bipolar or differential
//   data: pointer to a vector holding the data, length depending on channels and mode
//   temp: pointer to a variable holding the temperature
void AD7689::readChannels(uint8_t channels, uint8_t mode, uint16_t data[], uint16_t* temp) {

  // if the sequencer insn't active yet, enable it
  // occurs after self testing or at start-up
  if (!sequencerActive)
    configureSequencer();

  uint8_t scans = channels; // unipolar mode default
  if (mode == DIFFERENTIAL_MODE) {
    scans = channels / 2;
    if ((channels % 2) > 0)
      scans++;
  }

  // read as many values as there are ADC channels active
  // when reading differential, only half the number of channels will be read
  for (uint8_t ch = 0; ch < scans; ch++) {
    data[ch] = shiftTransaction(0, false, NULL);
    if (ch < 2) { // calculate time stamp based on ending of previous sequence for first 2 frames
      timeStamps[ch] = lastSeqEndTime - (1 - ch) * framePeriod;
    } else {
      timeStamps[ch] = micros() - framePeriod * 2; // sequenceTime in µs, 2 frames lag
    }
  }

  // capture temperature too
  *temp = shiftTransaction(0, false, NULL);
  tempTime = micros() - framePeriod * 2;
}

// reset time stamps for all samples and force an update sequence at the start of the next read command
uint32_t AD7689::initSampleTiming() {
  uint32_t curTime = micros(); // retrieve microcontroller run time in microseconds

  // set time for all samples to current time to force an update sequence
  for (uint8_t i = 0; i < TOTAL_CHANNELS; i++) timeStamps[i] = curTime;

  tempTime = curTime;
  return curTime;
}

/* sends a 16 bit word to the ADC, and simultaneously captures the response
   ADC responses lag 2 frames behind on commands
   if readback is activated, 32 bits will be captured instead of 16
*/
uint16_t AD7689::shiftTransaction(uint16_t command, bool readback, uint16_t* rb_cmd_ptr) {

  // one time start-up sequence
  if (!init_complete) {
    // give ADC time to start up
    delay(STARTUP_DELAY);

    // synchronize start of conversion
    digitalWrite(AD7689_PIN, LOW);
    delayMicroseconds(1); // miniumum 10 ns
    digitalWrite(AD7689_PIN, HIGH);
    delayMicroseconds(TCONV); // minimum 3.2 µs
    init_complete = true;
  }

  // allow time to sample
  delayMicroseconds(TCONV);

  // send config (RAC mode) and acquire data
  SPI.beginTransaction(AD7689_settings);
  digitalWrite(AD7689_PIN, LOW); // activate the ADC

  uint16_t data = SPI.transfer(command >> 8) << 8;  // transmit 8 MSB
  data |= SPI.transfer(command & 0xFF);             // transmit / LSB

  // if a readback is requested, the 16 bit frame is extended with another 16 bits to retrieve the value
  if (rb_cmd_ptr && readback) {
    // duplicate previous command
    *rb_cmd_ptr = (SPI.transfer(command >> 8) << 8) | SPI.transfer(command & 0xFF);
  }

  digitalWrite(AD7689_PIN, HIGH); // release the ADC
  SPI.endTransaction();

  // delay to allow data acquisition for the next cycle
  delayMicroseconds(2); // minumum 1.2µs

  return data;
}

// converts a command structure to a 16 bit word that can be transmitted over SPI
uint16_t AD7689::toCommand(AD7689_conf cfg) const {

  // build 14 bit configuration word
  uint16_t command = 0;
  command |= cfg.CFG_conf << CFG;		// update config on chip
  command |= (cfg.INCC_conf & 0b111) << INCC;	// mode - single ended, differential, ref, etc
  command |= (cfg.INx_conf & 0b111) << INx;	// channel
  command |= cfg.BW_conf << BW;		// 1 adds more filtering
  command |= (cfg.REF_conf & 0b111) << REF; // internal 4.096V reference
  command |= (cfg.SEQ_conf & 0b11) << SEQ;		// don't auto sequence
  command |= !(cfg.RB_conf) << RB;		// read back config value

  // convert 14 bits to 16 bits, 2 LSB are don't cares
  command = command << 2;

  return command;
}

// assemble user settings into a configuration for the ADC, or return a default configuration
AD7689_conf AD7689::getADCConfig(bool default_config) {
  AD7689_conf def;

  def.CFG_conf   = true;                    // overwrite existing configuration
  def.INCC_conf  = INCC_UNIPOLAR_REF_GND;   // default unipolar inputs, with reference to ground
  def.INx_conf   = TOTAL_CHANNELS;          // read all channels
  def.BW_conf    = true;                    // full bandwidth
  def.REF_conf   = INT_REF_4096;            // use interal 4.096V reference voltage
  def.SEQ_conf   = SEQ_OFF;                 // disable sequencer
  def.RB_conf    = false;                   // disable readback

  if (!default_config) { // default settings preloaded
    def.INCC_conf  = inputConfig;
    def.INx_conf   = (inputCount - 1);
    def.BW_conf    = !filterConfig;
    def.REF_conf   = refConfig;
  }

  // sequencer disabled, remember to restart it when taking measurements
  sequencerActive = false;

  return def;
}

// returns a value indicating if the ADC is properly connected and responding
bool AD7689::selftest() {
  // ADC will be tested with its readback function, which reads back a previous command
  // this process takes 3 cycles

  AD7689_conf rb_conf = getADCConfig(true);
  rb_conf.RB_conf = true;    // enable readback

  // send readback command
  shiftTransaction(toCommand(rb_conf), false, NULL);

  // skip second frame
  shiftTransaction(toCommand(getADCConfig(false)), false, NULL);

  // capture readback response
  uint16_t readback;
  shiftTransaction(toCommand(getADCConfig(false)), true, &readback);

  // response with initial readback command
  return (readback == toCommand(rb_conf));
}

// preliminary test results:
// raw values range from 4260 at room temperature to over 4400 when heated
// need calibration with ice cubes (= 0°C) and boiling methanol (= 64.7°C) or boiling ether (= 34.6°C)
float AD7689::readTemperature() {

  AD7689_conf temp_conf = getADCConfig(false);

  // set to use internal reference voltage
  // this automatically turns on the temperature sensor
  if (TEMP_REF == INTERNAL_25)
    temp_conf.REF_conf = INT_REF_25;
  else
    temp_conf.REF_conf = INT_REF_4096;

  // configure MUX for temperature sensor
  temp_conf.INCC_conf = INCC_TEMP;

  digitalWrite(AD7689_PIN, LOW);
  digitalWrite(AD7689_PIN, HIGH);
  delayMicroseconds(TCONV);

  // send the command
  shiftTransaction(toCommand(temp_conf), false, NULL);

  // skip second frame
  shiftTransaction(toCommand(getADCConfig(false)), false, NULL);

  // retrieve temperature reading
  uint16_t t = shiftTransaction(toCommand(getADCConfig(false)), false, NULL);

  // calculate temperature from ADC value:
  // output is 283 mV @ 25°C, and sensitivity of 1 mV/°C
  float temp = BASE_TEMP + ((t * TEMP_REF / TOTAL_STEPS)- TEMP_BASE_VOLTAGE) * TEMP_RICO;

  return temp;
}

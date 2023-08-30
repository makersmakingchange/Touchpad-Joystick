// 7MM_Joystick_GlidePoint_MKII_v1A
// 2022-11-14-A
const long buildDate = 2022111401;

//=================================================================================

// IMPORTANT: For the GlidePoint device I am using the QT PY (SAMD21) board.
//            You MUST use the TinyUSB package instead of the usual "joystick.h".
//                TOOLS >> USB STACK >> TinyUSB must be selected (not "arduino").

// IMPORTANT: The Cirque/Pinnacle/GlidePoint sensor is 3V ONLY. Use a 3V board!!!

// Board: Adafruit QT PY (SAMD21)
// Optimize: Faster (-O3)
// USB Stack: TinyUSB  <-- Important: Using TinyUSB, not Arduino USB!
// Debug: Off
// Port: COMxx (Adafruit QT PY (SAMD21))

// This sketch is only valid on boards which have native USB support
// and compatibility with Adafruit TinyUSB library. 
// For example SAMD21, SAMD51, nRF52840.

//=================================================================================

const boolean debug = true; // if true, output info to serial monitor or plotter.

//=================================================================================

#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_TinyUSB.h>

//=================================================================================

Adafruit_NeoPixel pixels = Adafruit_NeoPixel(1, PIN_NEOPIXEL);

uint32_t red = pixels.Color(255,0,0);
uint32_t green = pixels.Color(0,255,0);
uint32_t blue = pixels.Color(0,0,255);
uint32_t black = pixels.Color(0,0,0);

//=================================================================================

//Joystick via TinyUSB
uint8_t const desc_hid_report[] = { TUD_HID_REPORT_DESC_GAMEPAD() };
Adafruit_USBD_HID usb_hid(desc_hid_report, sizeof(desc_hid_report), HID_ITF_PROTOCOL_NONE, 2, false);
hid_gamepad_report_t    joystick;

//=================================================================================

// CIRQUE: Hardware pin-number labels
#define CS_PIN    0 // A0
#define DR_PIN    1 // A1

// CIRQUE: Masks for Cirque Register Access Protocol (RAP)
#define WRITE_MASK  0x80
#define READ_MASK   0xA0

// CIRQUE: Register config values
#define SYSCONFIG_1   0x00
#define FEEDCONFIG_1  0x03
#define FEEDCONFIG_2  0x1F
#define Z_IDLE_COUNT  0x05

// CIRQUE: Coordinate scaling values
#define PINNACLE_XMAX     2047    // max value Pinnacle can report for X
#define PINNACLE_YMAX     1535    // max value Pinnacle can report for Y
#define PINNACLE_X_LOWER  127     // min "reachable" X value
#define PINNACLE_X_UPPER  1919    // max "reachable" X value
#define PINNACLE_Y_LOWER  63      // min "reachable" Y value
#define PINNACLE_Y_UPPER  1471    // max "reachable" Y value
#define PINNACLE_X_RANGE  (PINNACLE_X_UPPER-PINNACLE_X_LOWER)
#define PINNACLE_Y_RANGE  (PINNACLE_Y_UPPER-PINNACLE_Y_LOWER)
#define ZONESCALE 256   // divisor for reducing x,y values to an array index for the LUT
#define ROWS_Y ((PINNACLE_YMAX + 1) / ZONESCALE)
#define COLS_X ((PINNACLE_XMAX + 1) / ZONESCALE)

// CIRQUE: ADC-attenuation settings (held in BIT_7 and BIT_6)
// 1X = most sensitive, 4X = least sensitive
#define ADC_ATTENUATE_1X   0x00
#define ADC_ATTENUATE_2X   0x40
#define ADC_ATTENUATE_3X   0x80
#define ADC_ATTENUATE_4X   0xC0

#define ADC_LOWER_DEFAULT  0x02

// CIRQUE: Select the overlay type
// 1 = Curved Overlay, 0 = Flat Overlay
#define SENSE0_OVERLAY_CURVE  1


typedef struct _absData
{
  uint16_t xValue;
  uint16_t yValue;
  uint16_t zValue;
  uint8_t buttonFlags;
  bool touchDown;
  bool hovering;
} absData_t;

absData_t touchData;


// These values require tuning for optimal touch-response
// Each element represents the Z-value below which is considered "hovering" in that XY region of the sensor.
// The values present are not guaranteed to work for all HW configurations.
const uint8_t ZVALUE_MAP[ROWS_Y][COLS_X] =
{
  {0, 0,  0,  0,  0,  0, 0, 0},
  {0, 2,  3,  5,  5,  3, 2, 0},
  {0, 3,  5, 15, 15,  5, 2, 0},
  {0, 3,  5, 15, 15,  5, 3, 0},
  {0, 2,  3,  5,  5,  3, 2, 0},
  {0, 0,  0,  0,  0,  0, 0, 0},
};

//=================================================================================

int joyMin = -127;
int joyMax = +127;

int rawHorz, rawVert; // raw values from sensor
int lmtHorz, lmtVert; // limited (range checked) value
int mapHorz, mapVert; // mapped value (limited)

// invert if needed
bool invHorz = true; 
bool invVert = true; 

// set the default min/max values. determined during testing.
int minHorz=50; // was: 100
int maxHorz=1000; // was: 900
int minVert=50;
int maxVert=1000;

//=================================================================================

void setup() {
  
  if (debug) { Serial.begin(115200); }

  pinMode(CS_PIN, OUTPUT);
  pinMode(DR_PIN, INPUT);

  // Startup the NeoPixel
  pixels.begin(); pixels.clear(); pixels.setPixelColor(0, red); pixels.show();
  
  // Startup SPI and TrackPoint
  SPI.begin();
  Pinnacle_Init();

  // These functions are required for use with thick overlays (curved)
  setAdcAttenuation(ADC_ATTENUATE_2X);
  tuneEdgeSensitivity();

  #if defined(ARDUINO_ARCH_MBED) && defined(ARDUINO_ARCH_RP2040)
    // Manual begin() is required on core without built-in support for TinyUSB such as mbed rp2040
    TinyUSB_Device_Init(0);
  #endif

  // Startup the joystick
  // IMPORTANT: This will loop until device is mounted.
  //            So, if you see a RED light, it's powered, but not mounted.
  //            When the LED goes GREEN, it's mounted.
  //            When touched (sending data) LED goes BLUE
  usb_hid.begin();
  while( !TinyUSBDevice.mounted() ) delay(10);

  Pinnacle_EnableFeed(true);

}

//=================================================================================

void loop() {
  
  if(DR_Asserted())
  {    
    
    Pinnacle_GetAbsolute(&touchData); // Get the data from the GlidePoint
    Pinnacle_CheckValidTouch(&touchData);     // Checks for "hover" caused by curved overlays
    ScaleData(&touchData, 1024, 1024);  // Scale coordinates to X, Y resolution

    rawHorz = (int) touchData.xValue;
    rawVert = (int) touchData.yValue;

    lmtHorz = rawHorz;
    lmtVert = rawVert;
    if (lmtHorz<minHorz) {lmtHorz=minHorz;}
    if (lmtHorz>maxHorz) {lmtHorz=maxHorz;}
    if (lmtVert<minVert) {lmtVert=minVert;}
    if (lmtVert>maxVert) {lmtVert=maxVert;}
    

    // Map values to a range the XAC likes
    mapHorz = map(lmtHorz, minHorz, maxHorz, joyMin, joyMax);
    mapVert = map(lmtVert, minVert, maxVert, joyMin, joyMax);

    if (debug) {
      Serial.print("Build: ");
      Serial.print(buildDate);      
      Serial.print(" T: ");
      Serial.print(touchData.touchDown);
      Serial.print(" X: ");
      Serial.print(touchData.xValue);
      Serial.print(" Y: ");
      Serial.print(touchData.yValue);
      Serial.print(" Z: ");
      Serial.print(touchData.zValue);
      Serial.print(" RH: ");
      Serial.print(rawHorz);
      Serial.print(" RV: ");
      Serial.print(rawVert);
      Serial.print(" LH: ");
      Serial.print(lmtHorz);
      Serial.print(" LV: ");
      Serial.print(lmtVert);
      Serial.print(" MH: ");
      Serial.print(mapHorz);
      Serial.print(" MV: ");
      Serial.print(mapVert);
      Serial.println();
    }

    if ( usb_hid.ready() ) {
      joystick.x = mapHorz;
      joystick.y = mapVert;
      usb_hid.sendReport(0, &joystick, sizeof(joystick));
    }
  
  }//DR_Asserted

  if (touchData.touchDown) {
    pixels.clear(); pixels.setPixelColor(0, blue); pixels.show();
  }
  else { 
    pixels.clear(); pixels.setPixelColor(0, green); pixels.show(); 
    if ( usb_hid.ready() ) { joystick.x = 0; joystick.y = 0; usb_hid.sendReport(0, &joystick, sizeof(joystick)); }
  }

}//loop

//=================================================================================



/*  Pinnacle-based TM040040 Functions  */

void Pinnacle_Init()
{
  DeAssert_CS();

  // Host clears SW_CC flag
  Pinnacle_ClearFlags();
  // Host configures bits of registers 0x03 and 0x05
  RAP_Write(0x03, SYSCONFIG_1);
  RAP_Write(0x05, FEEDCONFIG_2);
  // Host enables preferred output mode (absolute)
  RAP_Write(0x04, FEEDCONFIG_1);
  // Host sets z-idle packet count to 5 (default is 30)
  RAP_Write(0x0A, Z_IDLE_COUNT);
}

void Pinnacle_GetAbsolute(absData_t * result)
{
// Reads XYZ data from Pinnacle registers 0x14 through 0x17
// Stores result in absData_t struct with xValue, yValue, and zValue members

  uint8_t data[6] = { 0,0,0,0,0,0 };
  RAP_ReadBytes(0x12, data, 6);
  Pinnacle_ClearFlags();

  result->buttonFlags = data[0] & 0x3F;
  result->xValue = data[2] | ((data[4] & 0x0F) << 8);
  result->yValue = data[3] | ((data[4] & 0xF0) << 4);
  result->zValue = data[5] & 0x3F;
  result->touchDown = result->xValue != 0;
}

void Pinnacle_ClearFlags()
{
  // Clears Status1 register flags (SW_CC and SW_DR)
  RAP_Write(0x02, 0x00);
  delayMicroseconds(50);
}


void Pinnacle_EnableFeed(bool feedEnable)
{
  // Enables/Disables the feed
  uint8_t temp;
  RAP_ReadBytes(0x04, &temp, 1);  // Store contents of FeedConfig1 register
  if(feedEnable)
  {
    temp |= 0x01;                 // Set Feed Enable bit
    RAP_Write(0x04, temp);
  }
  else
  {
    temp &= ~0x01;                // Clear Feed Enable bit
    RAP_Write(0x04, temp);
  }
}

//=================================================================================

/*  Curved Overlay Functions  */
// Adjusts the feedback in the ADC, effectively attenuating the finger signal
// By default, the the signal is maximally attenuated (ADC_ATTENUATE_4X for use with thin, flat overlays
void setAdcAttenuation(uint8_t adcGain)
{
  uint8_t temp = 0x00;

  if (debug) { Serial.println(); }
  if (debug) { Serial.println("Setting ADC gain..."); }
  ERA_ReadBytes(0x0187, &temp, 1);
  temp &= 0x3F; // clear top two bits
  temp |= adcGain;
  ERA_WriteByte(0x0187, temp);
  ERA_ReadBytes(0x0187, &temp, 1);
  if (debug) { Serial.print("ADC gain set to:\t"); }
  if (debug) { Serial.print(temp &= 0xC0, HEX); }
  switch(temp)
  {
    case ADC_ATTENUATE_1X:
      if (debug) { Serial.println(" (X/1)"); }
      break;
    case ADC_ATTENUATE_2X:
      if (debug) { Serial.println(" (X/2)"); }
      break;
    case ADC_ATTENUATE_3X:
      if (debug) { Serial.println(" (X/3)"); }
      break;
    case ADC_ATTENUATE_4X:
      if (debug) { Serial.println(" (X/4)"); }
      break;
    default:
      break;
  }
}

// Changes thresholds to improve detection of fingers
void tuneEdgeSensitivity()
{
  uint8_t temp = 0x00;

  if (debug) { Serial.println(); }
  if (debug) { Serial.println("Setting xAxis.WideZMin..."); }
  ERA_ReadBytes(0x0149, &temp, 1);
  if (debug) { Serial.print("Current value:\t"); }
  if (debug) { Serial.println(temp, HEX); }
  ERA_WriteByte(0x0149,  0x04);
  ERA_ReadBytes(0x0149, &temp, 1);
  if (debug) { Serial.print("New value:\t"); }
  if (debug) { Serial.println(temp, HEX); }

  if (debug) { Serial.println(); }
  if (debug) { Serial.println("Setting yAxis.WideZMin..."); }
  ERA_ReadBytes(0x0168, &temp, 1);
  if (debug) { Serial.print("Current value:\t"); }
  if (debug) { Serial.println(temp, HEX); }
  ERA_WriteByte(0x0168,  0x03);
  ERA_ReadBytes(0x0168, &temp, 1);
  if (debug) { Serial.print("New value:\t"); }
  if (debug) { Serial.println(temp, HEX); }
}

// This function identifies when a finger is "hovering" so your system can choose to ignore them.
// Explanation: Consider the response of the sensor to be flat across it's area. The Z-sensitivity of the sensor projects this area
// a short distance upwards above the surface of the sensor. Imagine it is a solid cylinder (wider than it is tall)
// in which a finger can be detected and tracked. Adding a curved overlay will cause a user's finger to dip deeper in the middle, and higher
// on the perimeter. If the sensitivity is tuned such that the sensing area projects to the highest part of the overlay, the lowest
// point will likely have excessive sensitivity. This means the sensor can detect a finger that isn't actually contacting the overlay in the shallower area.
// ZVALUE_MAP[][] stores a lookup table in which you can define the Z-value and XY position that is considered "hovering". Experimentation/tuning is required.
// NOTE: Z-value output decreases to 0 as you move your finger away from the sensor, and it's maximum value is 0x63 (6-bits).
void Pinnacle_CheckValidTouch(absData_t * touchData)
{
  uint32_t zone_x, zone_y;
  //eliminate hovering
  zone_x = touchData->xValue / ZONESCALE;
  zone_y = touchData->yValue / ZONESCALE;
  touchData->hovering = !(touchData->zValue > ZVALUE_MAP[zone_y][zone_x]);
}

//=================================================================================

  /*  ERA (Extended Register Access) Functions  */

void ERA_ReadBytes(uint16_t address, uint8_t * data, uint16_t count)
{  
  // Reads <count> bytes from an extended register at <address> (16-bit address),
  // stores values in <*data>
  uint8_t ERAControlValue = 0xFF;

  Pinnacle_EnableFeed(false); // Disable feed
  RAP_Write(0x1C, (uint8_t)(address >> 8));     // Send upper byte of ERA address
  RAP_Write(0x1D, (uint8_t)(address & 0x00FF)); // Send lower byte of ERA address
  for(uint16_t i = 0; i < count; i++)
  {
    RAP_Write(0x1E, 0x05);  // Signal ERA-read (auto-increment) to Pinnacle
    // Wait for status register 0x1E to clear
    do
    {
      RAP_ReadBytes(0x1E, &ERAControlValue, 1);
    } while(ERAControlValue != 0x00);
    RAP_ReadBytes(0x1B, data + i, 1);
    Pinnacle_ClearFlags();
  }
}

void ERA_WriteByte(uint16_t address, uint8_t data)
{
  // Writes a byte, <data>, to an extended register at <address> (16-bit address)
  uint8_t ERAControlValue = 0xFF;
  Pinnacle_EnableFeed(false); // Disable feed
  RAP_Write(0x1B, data);      // Send data byte to be written
  RAP_Write(0x1C, (uint8_t)(address >> 8));     // Upper byte of ERA address
  RAP_Write(0x1D, (uint8_t)(address & 0x00FF)); // Lower byte of ERA address
  RAP_Write(0x1E, 0x02);  // Signal an ERA-write to Pinnacle
  // Wait for status register 0x1E to clear
  do
  {
    RAP_ReadBytes(0x1E, &ERAControlValue, 1);
  } while(ERAControlValue != 0x00);
  Pinnacle_ClearFlags();
}

//=================================================================================

/*  RAP Functions */

void RAP_ReadBytes(uint8_t address, uint8_t * data, uint8_t count)
{
  // Reads <count> Pinnacle registers starting at <address>
  uint8_t cmdByte = READ_MASK | address;   // Form the READ command byte
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE1));
  Assert_CS();
  SPI.transfer(cmdByte);  // Signal a RAP-read operation starting at <address>
  SPI.transfer(0xFC);     // Filler byte
  SPI.transfer(0xFC);     // Filler byte
  for(uint8_t i = 0; i < count; i++)
  {
    data[i] =  SPI.transfer(0xFC);  // Each subsequent SPI transfer gets another register's contents
  }
  DeAssert_CS();
  SPI.endTransaction();
}

void RAP_Write(uint8_t address, uint8_t data)
{
  // Writes single-byte <data> to <address>
  uint8_t cmdByte = WRITE_MASK | address;  // Form the WRITE command byte
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE1));
  Assert_CS();
  SPI.transfer(cmdByte);  // Signal a write to register at <address>
  SPI.transfer(data);    // Send <value> to be written to register
  DeAssert_CS();
  SPI.endTransaction();
}

//=================================================================================

/*  Logical Scaling Functions */

void ClipCoordinates(absData_t * coordinates)
{
  // Clips raw coordinates to "reachable" window of sensor
  // NOTE: values outside this window can only appear as a result of noise
  if(coordinates->xValue < PINNACLE_X_LOWER) { coordinates->xValue = PINNACLE_X_LOWER; }
  else if(coordinates->xValue > PINNACLE_X_UPPER) { coordinates->xValue = PINNACLE_X_UPPER; }
  if(coordinates->yValue < PINNACLE_Y_LOWER) { coordinates->yValue = PINNACLE_Y_LOWER; }
  else if(coordinates->yValue > PINNACLE_Y_UPPER) { coordinates->yValue = PINNACLE_Y_UPPER; }
}

void ScaleData(absData_t * coordinates, uint16_t xResolution, uint16_t yResolution)
{
  // Scales data to desired X & Y resolution
  uint32_t xTemp = 0;
  uint32_t yTemp = 0;
  ClipCoordinates(coordinates);
  xTemp = coordinates->xValue;
  yTemp = coordinates->yValue;
  // translate coordinates to (0, 0) reference by subtracting edge-offset
  xTemp -= PINNACLE_X_LOWER;
  yTemp -= PINNACLE_Y_LOWER;
  // scale coordinates to (xResolution, yResolution) range
  coordinates->xValue = (uint16_t)(xTemp * xResolution / PINNACLE_X_RANGE);
  coordinates->yValue = (uint16_t)(yTemp * yResolution / PINNACLE_Y_RANGE);
}

//=================================================================================

/*  I/O Functions */

void Assert_CS() { digitalWrite(CS_PIN, LOW); }

void DeAssert_CS(){ digitalWrite(CS_PIN, HIGH); }

bool DR_Asserted(){ return digitalRead(DR_PIN); }

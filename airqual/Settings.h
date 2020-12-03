/** The MIT License (MIT)

Copyright (c) 2018 David Payne

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/******************************************************************************
 * NOTE: The settings here are the default settings for the first loading.
 * After loading you will manage changes to the settings via the Web Interface.
 * If you want to change settings again in the settings.h, you will need to
 * erase the file system on the Wemos or use the “Reset Settings” option in
 * the Web Interface.
 ******************************************************************************/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPUpdateServer.h>
#include "TimeClient.h"
#include "OpenWeatherMapClient.h"
#include "WeatherStationFonts.h"
#include "FS.h"
#include "SH1106Wire.h"
#include "SSD1306Wire.h"
#include "OLEDDisplayUi.h"
#include <Wire.h>
#include <sps30.h>
#include <SPIFFSLogger.h>
#include <time.h>

/* constants */
#define SECS_IN_DAY 60*60*24
#define DAYS_IN_WEEK 7
#define DAYS_IN_YEAR 365

//******************************
// Start Settings
//******************************

// Weather Configuration
boolean DISPLAYWEATHER = true; // true = show weather / false = no weather
String  WeatherApiKey  = "";   // Your API Key from http://openweathermap.org/
// Default City Location (use http://openweathermap.org/find to find city ID)
int     CityIDs[] = {5304391};
boolean IS_METRIC = false; // false = Imperial and true = Metric
// Languages: ar, bg, ca, cz, de, el, en, fa, fi, fr, gl, hr, hu, it, ja, kr,
// la, lt, mk, nl, pl, pt, ro, ru, se, sk, sl, es, tr, ua, vi, zh_cn, zh_tw
String WeatherLanguage = "en"; // Default (en) English

// Webserver
const int     WEBSERVER_PORT    = 80;
const boolean WEBSERVER_ENABLED = true;
boolean       IS_BASIC_AUTH = true; // true = require athentication to change
                                    // configuration settings / false = no auth
char* www_username = "admin";       // User account for the Web Interface
char* www_password = "password";    // Password for the Web Interface

// Date and Time
float   UtcOffset = -6;    // Hour offset from GMT for your timezone
boolean IS_24HOUR = false; // 23:00 millitary 24 hour clock
int     minutesBetweenDataRefresh = 15;
boolean DISPLAYCLOCK = true; // true = Show Clock when not printing / false =
                             // turn off display when not printing

// Display Settings
// I2C Address of your Display(usually 0x3c or 0x3d)
const int I2C_DISPLAY_ADDRESS = 0x3c;
const int SDA_PIN             = 4;
const int SCL_PIN             = 5;
// true = pins at top | false = pins at the bottom
boolean INVERT_DISPLAY = true;
//#define DISPLAY_SH1106       // Uncomment this line to use the SH1106 display
//-- SSD1306 is used by default and is most common

// LED Settings
const int externalLight = LED_BUILTIN; // Set to unused pin, like D1, to disable use of built-in LED (LED_BUILTIN)

// ALARM Settings
const int ALARM_PIN = 15;
const int ALARM_DURATION = 10; //Milliseconds

// OTA Updates
boolean ENABLE_OTA = true;     // this will allow you to load firmware to the device over WiFi (see OTA for ESP8266)
String OTA_Password = "";      // Set an OTA password here -- leave blank if you don't want to be prompted for password

String themeColor = "light-green"; // this can be changed later in the web interface.

//******************************
// End Settings
//******************************

// SVM global data, arguably should be put in the .c file.

typedef struct {
    struct sps30_measurement m; /* for sps30 data */
    float                    tempC;
    float                    RH;
    uint16_t                 TVOC;  
    uint16_t                 CO2eq; 
    float                    volts;
} data_sample_t;

data_sample_t data_sample; /* all the measured data goes here for logging */

const int samples = 5; /* should be odd */
struct sps30_measurement rolling[samples];

uint8_t  result;
uint8_t  data[10]; /* Data buffer for i2c */
uint8_t  tmp;
uint8_t  flt[4];      /** @todo CGR change this, its just bytes for eeprom */
uint16_t readings[2]; // 2 16 bit integers
uint16_t tempraw;
float    tempC;
uint16_t humidityraw;
float    RH;
uint16_t mincount; // minute counter for VOC baseline houskeeping
uint8_t  baseline; // 0 if not initialized, 1 when initialized
uint16_t TVOC;     
uint16_t CO2eq;    

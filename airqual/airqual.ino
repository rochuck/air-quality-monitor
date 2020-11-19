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
/* Additional Contributions:

/*
 20200920 - Chuck Rohs.
    Based on code initially developed by Dave Viberg, derived from printermonitor
    Removed all extraneous code.

    Updated SPS30 code to use library, instead of bit bashing.

    Updated web page to look noice.

    Added logging functionalty
    see https://github.com/esp8266/arduino-esp8266fs-plugin
    and https://circuits4you.com/2018/03/10/esp8266-jquery-and-ajax-web-server
    using: https://cdnjs.com/libraries/Chart.js
    for logging: https://github.com/bitmario/SPIFFSLogger

    Added circuitry for monitoring battery voltage.
    Since the ESP reads 0-1V we need a voltage divider of 174K/10K. This gives
    a 1V/full scale reading at 18.4 volts, i.e. take the adc reading and divide
    by 55.6 for the actual volts. Charge when voltage drops to 6.6. The box
    should be shut off by then.

 */

/* VOC Notes:
 voc sensor gives error on read for 15sec while initializing, if no previous baseline take 12 hours to stabilize for saving
 routines- i2c read and write, init air quality, probe and features, start measurment, measure iaq,
 routines- read ready flag, get/set baseline, TVOC CO2eq, temp humidity
 ** use arduino eeprom for saving baseline, esp8266 version implements in flash **

 temp sensor resets on power on, get temp and humidity poll on data ready (15msec) then read and to feed voc sensor
 write 0xE0, 0x78, 0x66 to set temperature sensor to read temp first (svm30 datasheet page 11),
 write 0xE0, 0x5c, 0x24, read 0xe1 until success, read 0xe1 and 4 bytes data with 2 crc's, save in ram, calc to display

 init voc 0x2003, set humidity 0x2061, set baseline 0x201e, measure air quality 0x2008 every sec to keep base line updated
 baseline should be saved every 10 min or so, so as not to wear out flash, 4 bytes data and 2 bytes crc
 measure air quality returns 2 byte CO2eq (ppm) and 2 byte TVOC (ppb)
 */

 /***********************************************
 * Edit Settings.h for personalization
 ***********************************************/
#include <GDBStub.h>
#include "Settings.h"
#include <EEPROM.h>

#define VERSION "2.0.0"

#define HOSTNAME "AirQual-"
#define CONFIG "/conf.txt"

/* Useful Constants */
#define SECS_PER_MIN  (60UL)
#define SECS_PER_HOUR (3600UL)

/* Useful Macros for getting elapsed time */
#define numberOfSeconds(_time_) (_time_ % SECS_PER_MIN)
#define numberOfMinutes(_time_) ((_time_ / SECS_PER_MIN) % SECS_PER_MIN)
#define numberOfHours(_time_) (_time_ / SECS_PER_HOUR)

// Initialize the oled display for I2C_DISPLAY_ADDRESS
// SDA_PIN and SCL_PIN
#if defined(DISPLAY_SH1106)
  SH1106Wire     display(I2C_DISPLAY_ADDRESS, SDA_PIN, SCL_PIN);
#else
  SSD1306Wire     display(I2C_DISPLAY_ADDRESS, SDA_PIN, SCL_PIN); // this is the default
#endif

OLEDDisplayUi   ui( &display );

/* Alarm data */
float alarm_low    = 10; /* 10 uG/m^3 default */
float alarm_high   = 100;
bool     alarm_enable = false;

/* create the logger */
SPIFFSLogger<data_sample_t> logger("/log/mydata",2); /* keep two days */
SPIFFSLogger<data_sample_t> hist_logger("/log/myhisdata",365); /* keep a year */
FSInfo fs_info;

void drawProgress(OLEDDisplay *display, int percentage, String label);
void drawOtaProgress(unsigned int, unsigned int);
void drawHeaderOverlay(OLEDDisplay *display, OLEDDisplayUiState* state);
void drawTinyAQ(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawPM05(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawPM10(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawPM25(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawPM40(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawPM100(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawWeather(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);

// Set the number of Frames supported
const int numberOfFrames = 7; /* weather, rh/temp, 5x aqi */
FrameCallback frames[numberOfFrames];
boolean isClockOn = false;

OverlayCallback overlays[] = { drawHeaderOverlay };
int numberOfOverlays = 1;

// Time
TimeClient timeClient(UtcOffset);
long       lastEpoch        = 0;
long       firstEpoch       = 0;
long       displayOffEpoch  = 0;
String     lastMinute       = "xx";
String     lastSecond       = "xx";
String     lastReportStatus = "";

OpenWeatherMapClient weatherClient(WeatherApiKey, CityIDs, 1, IS_METRIC, WeatherLanguage);
void configModeCallback (WiFiManager *myWiFiManager);
int8_t getWifiQuality();

ESP8266WebServer server(WEBSERVER_PORT);
ESP8266HTTPUpdateServer serverUpdater;

String WEB_ACTIONS =  "<a class='w3-bar-item w3-button' href='/'><i class='fa fa-home'></i> Home</a>"
                      "<a class='w3-bar-item w3-button' href='/index.html'><i class='fa fa-area-chart'></i> Real Time</a>"
                      "<a class='w3-bar-item w3-button' href='/clean' onclick='return confirm(\"Do you want clean the particle counter?\")'><i class='fa fa-bath'></i> Run Cleaning Cycle</a>"
                      "<a class='w3-bar-item w3-button' href='/daily.html'><i class='fa fa-line-chart'></i> Daily</a>"
                      "<a class='w3-bar-item w3-button' href='/week.html'><i class='fa fa-calendar-o'></i> Last Week</a>"                      
                      "<a class='w3-bar-item w3-button' href='/year.html'><i class='fa fa-birthday-cake'></i> Last Year</a>"                      
                      "<a class='w3-bar-item w3-button' href='/configure'><i class='fa fa-cog'></i> Configure</a>"
                      "<a class='w3-bar-item w3-button' href='/configureweather'><i class='fa fa-cloud'></i> Weather</a>"
                      "<a class='w3-bar-item w3-button' href='/systemreset' onclick='return confirm(\"Do you want to reset to default settings?\")'><i class='fa fa-undo'></i> Reset Settings</a>"
                      "<a class='w3-bar-item w3-button' href='/forgetwifi' onclick='return confirm(\"Do you want to forget to WiFi connection?\")'><i class='fa fa-wifi'></i> Forget WiFi</a>"
                      "<a class='w3-bar-item w3-button' href='/update'><i class='fa fa-wrench'></i> Firmware Update</a>"
                      "<a class='w3-bar-item w3-button' href='https://github.com/rochuck' target='_blank'><i class='fa fa-question-circle'></i> About</a>";

String CHANGE_FORM =  "<form class='w3-container' action='/updateconfig' method='get'><h2>Configuration:</h2>"
                      "<p><input name='is24hour' class='w3-check w3-margin-top' type='checkbox' %IS_24HOUR_CHECKED%> Use 24 Hour Clock (military time)</p>"
                      "<p><input name='invDisp' class='w3-check w3-margin-top' type='checkbox' %IS_INVDISP_CHECKED%> Flip display orientation</p>"
                      "<p>Clock Sync / Weather Refresh (minutes) <select class='w3-option w3-padding' name='refresh'>%OPTIONS%</select></p>";

String ALARM_FORM =   "<hr>"
                      "<p><label>Low Particle alarm</label><input class='w3-input w3-border w3-margin-bottom' type='text' name='lomcalarm' value='%LOMCALARM%' maxlength='6'></p>"
                      "<p><label>High Particle alarm</label><input class='w3-input w3-border w3-margin-bottom' type='text' name='himcalarm' value='%HIMCALARM%' maxlength='6'></p>"                      
                      "<hr>";

String THEME_FORM =   "<p>Theme Color <select class='w3-option w3-padding' name='theme'>%THEME_OPTIONS%</select></p>"
                      "<p><label>UTC Time Offset</label><input class='w3-input w3-border w3-margin-bottom' type='text' name='utcoffset' value='%UTCOFFSET%' maxlength='12'></p><hr>"
                      "<p><input name='isBasicAuth' class='w3-check w3-margin-top' type='checkbox' %IS_BASICAUTH_CHECKED%> Use Security Credentials for Configuration Changes</p>"
                      "<p><label>User ID (for this interface)</label><input class='w3-input w3-border w3-margin-bottom' type='text' name='userid' value='%USERID%' maxlength='20'></p>"
                      "<p><label>Password </label><input class='w3-input w3-border w3-margin-bottom' type='password' name='stationpassword' value='%STATIONPASSWORD%'></p>"
                      "<button class='w3-button w3-block w3-grey w3-section w3-padding' type='submit'>Save</button></form>";

String WEATHER_FORM = "<form class='w3-container' action='/updateweatherconfig' method='get'><h2>Weather Config:</h2>"
                      "<p><input name='isWeatherEnabled' class='w3-check w3-margin-top' type='checkbox' %IS_WEATHER_CHECKED%> Display Weather when printer is off</p>"
                      "<label>OpenWeatherMap API Key (get from <a href='https://openweathermap.org/' target='_BLANK'>here</a>)</label>"
                      "<input class='w3-input w3-border w3-margin-bottom' type='text' name='openWeatherMapApiKey' value='%WEATHERKEY%' maxlength='60'>"
                      "<p><label>%CITYNAME1% (<a href='http://openweathermap.org/find' target='_BLANK'><i class='fa fa-search'></i> Search for City ID</a>) "
                      "or full <a href='http://openweathermap.org/help/city_list.txt' target='_BLANK'>city list</a></label>"
                      "<input class='w3-input w3-border w3-margin-bottom' type='text' name='city1' value='%CITY1%' onkeypress='return isNumberKey(event)'></p>"
                      "<p><input name='metric' class='w3-check w3-margin-top' type='checkbox' %METRIC%> Use Metric (Celsius)</p>"
                      "<p>Weather Language <select class='w3-option w3-padding' name='language'>%LANGUAGEOPTIONS%</select></p>"
                      "<button class='w3-button w3-block w3-grey w3-section w3-padding' type='submit'>Save</button></form>"
                      "<script>function isNumberKey(e){var h=e.which?e.which:event.keyCode;return!(h>31&&(h<48||h>57))}</script>";

String LANG_OPTIONS = "<option>ar</option>"
                      "<option>bg</option>"
                      "<option>ca</option>"
                      "<option>cz</option>"
                      "<option>de</option>"
                      "<option>el</option>"
                      "<option>en</option>"
                      "<option>fa</option>"
                      "<option>fi</option>"
                      "<option>fr</option>"
                      "<option>gl</option>"
                      "<option>hr</option>"
                      "<option>hu</option>"
                      "<option>it</option>"
                      "<option>ja</option>"
                      "<option>kr</option>"
                      "<option>la</option>"
                      "<option>lt</option>"
                      "<option>mk</option>"
                      "<option>nl</option>"
                      "<option>pl</option>"
                      "<option>pt</option>"
                      "<option>ro</option>"
                      "<option>ru</option>"
                      "<option>se</option>"
                      "<option>sk</option>"
                      "<option>sl</option>"
                      "<option>es</option>"
                      "<option>tr</option>"
                      "<option>ua</option>"
                      "<option>vi</option>"
                      "<option>zh_cn</option>"
                      "<option>zh_tw</option>";

String COLOR_THEMES = "<option>red</option>"
                      "<option>pink</option>"
                      "<option>purple</option>"
                      "<option>deep-purple</option>"
                      "<option>indigo</option>"
                      "<option>blue</option>"
                      "<option>light-blue</option>"
                      "<option>cyan</option>"
                      "<option>teal</option>"
                      "<option>green</option>"
                      "<option>light-green</option>"
                      "<option>lime</option>"
                      "<option>khaki</option>"
                      "<option>yellow</option>"
                      "<option>amber</option>"
                      "<option>orange</option>"
                      "<option>deep-orange</option>"
                      "<option>blue-grey</option>"
                      "<option>brown</option>"
                      "<option>grey</option>"
                      "<option>dark-grey</option>"
                      "<option>black</option>"
                      "<option>w3schools</option>";

void setup() {
    int16_t ret;

    Serial.begin(115200);
    SPIFFS.begin();
    delay(2000);
    gdbstub_init();

    /* boot beep :) */
    pinMode(ALARM_PIN, OUTPUT);
    sound_alarm(3,300,200);
    
    // New Line to clear from start garbage
    Serial.println();

    // Initialize digital pin for LED (little blue light on the Wemos D1 Mini)
    pinMode(externalLight, OUTPUT);

    readSettings();

    // initialize display
    display.init();
    if (INVERT_DISPLAY) {
        display.flipScreenVertically(); // connections at top of OLED display
    }
    display.clear();
    display.display();

    display.setFont(ArialMT_Plain_16);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setContrast(255); // default is 255
    display.drawString(64,
                       5,
                       "Air Quality Monitor\nBy DAV/CGR\nV" +
                           String(VERSION)); // dv
    display.display();

    // WiFiManager
    // Local intialization. Once its business is done, there is no need to keep
    // it around
    WiFiManager wifiManager;

    // Uncomment for testing wifi manager
    // wifiManager.resetSettings();
    wifiManager.setAPCallback(configModeCallback);

    String hostname(HOSTNAME);
    hostname += String(ESP.getChipId(), HEX);
    if (!wifiManager.autoConnect(
            (const char*) hostname.c_str())) { // new addition
        delay(3000);
        WiFi.disconnect(true);
        ESP.reset();
        delay(5000);
    }

    // You can change the transition that is used
    // SLIDE_LEFT, SLIDE_RIGHT, SLIDE_TOP, SLIDE_DOWN
    ui.setFrameAnimation(SLIDE_LEFT);
    ui.setTargetFPS(30);
    ui.disableAllIndicators();
    ui.setFrames(frames, 7);
    frames[0] = drawWeather;
    frames[1] = drawTinyAQ;
    frames[2] = drawPM05;
    frames[3] = drawPM10;
    frames[4] = drawPM25;
    frames[5] = drawPM40;
    frames[6] = drawPM100;

    ui.setOverlays(overlays, numberOfOverlays);

    // Inital UI takes care of initalising the display too.
    ui.init();
    if (INVERT_DISPLAY) {
        display.flipScreenVertically(); // connections at top of OLED display
    }

    // print the received signal strength:
    Serial.print("Signal Strength (RSSI): ");
    Serial.print(getWifiQuality());
    Serial.println("%");

    if (ENABLE_OTA) {
        ArduinoOTA.onStart([]() { Serial.println("Start"); });
        ArduinoOTA.onEnd([]() { Serial.println("\nEnd"); });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
        });
        ArduinoOTA.onError([](ota_error_t error) {
            Serial.printf("Error[%u]: ", error);
            if (error == OTA_AUTH_ERROR)
                Serial.println("Auth Failed");
            else if (error == OTA_BEGIN_ERROR)
                Serial.println("Begin Failed");
            else if (error == OTA_CONNECT_ERROR)
                Serial.println("Connect Failed");
            else if (error == OTA_RECEIVE_ERROR)
                Serial.println("Receive Failed");
            else if (error == OTA_END_ERROR)
                Serial.println("End Failed");
        });
        ArduinoOTA.setHostname((const char*) hostname.c_str());
        if (OTA_Password != "") {
            ArduinoOTA.setPassword(((const char*) OTA_Password.c_str()));
        }
        ArduinoOTA.begin();
    }

    if (WEBSERVER_ENABLED) {
        server.on("/", display_air_quality);
        server.on("/clean", handleClean);
        server.on("/systemreset", handleSystemReset);
        server.on("/forgetwifi", handleWifiReset);
        server.on("/updateconfig", handleUpdateConfig);
        server.on("/updateweatherconfig", handleUpdateWeather);
        server.on("/configure", handleConfigure);
        server.on("/configureweather", handleWeatherConfigure);
        server.onNotFound(handleWebRequests); //Set setver all paths are not found so we can handle as per URI
        server.on("/readquality", handle_quality); //This page is called by java Script AJAX
        server.on("/readdaily", handle_daily); //This page is called by java Script AJAX
        server.on("/readweek", handle_weeks_data); //This page is called by java Script AJAX
        server.on("/readyear", handle_years_data); //This page is called by java Script AJAX


        serverUpdater.setup(&server, "/update", www_username, www_password);
        // Start the server
        server.begin();
        Serial.println("Server started");
        // Print the IP address
        String webAddress = "http://" + WiFi.localIP().toString() + ":" +
                            String(WEBSERVER_PORT) + "/";
        Serial.println("Use this URL : " + webAddress);
        display.clear();
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.setFont(ArialMT_Plain_10);
        display.drawString(64, 10, "Web Interface On");
        display.drawString(64, 20, "You May Connect to IP");
        display.setFont(ArialMT_Plain_16);
        display.drawString(64, 30, WiFi.localIP().toString());
        display.drawString(64, 46, "Port: " + String(WEBSERVER_PORT));
        display.display();
    } else {
        Serial.println("Web Interface is Disabled");
        display.clear();
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.setFont(ArialMT_Plain_10);
        display.drawString(64, 10, "Web Interface is Off");
        display.drawString(64, 20, "Enable in Settings.h");
        display.display();
    }
    flashLED(5, 500);

    // for Sensirion SPS30 start particle counter measurements
    sensirion_i2c_init();

    while (sps30_probe() != 0) {
        Serial.print("SPS sensor probing failed\n");
        delay(500);
    }

    /* setup auto clean, make this a web parameter */
    uint8_t auto_clean_days = 4;
    /*
      ret = sps30_set_fan_auto_cleaning_interval_days(auto_clean_days);
      if (ret) {
          Serial.print("error setting the auto-clean interval: ");
          Serial.println(ret);
      }
    */
    ret = sps30_start_measurement();
    if (ret < 0) { Serial.print("error starting measurement\n"); }

    // dv for Sensirion SVM30 VOC temp and humidity measurements, address 0x58
    // for sgp30 voc, 0x70 shtc1 t + rh dv for VOC, temp sensor resets on power
    // on, start measurement, wait 15msec then read and to feed voc sensor
    starttemprh(); // start measurement
    delay(15);     // wait for humidity measurement to complete
    readtemprh();  // get initial reading to setup VOC

    // dv need humidity to set up voc sensor
    // dv init VOC, wait 2msec, set humidity, if baseline set baseline, then set
    // humidity
    EEPROM.begin(256);
    // send initialization command
    iicWr2byteandcrc(0x58, 0x20, 0x03); // sends command to init VOC sensor
    mincount = 0;                       // initialize minute counter
    EEPROM.get(0, flt);
    if ((flt[0] == 0xFF) && (flt[1] == 0xFF)){
        baseline == 0; // no baseline, have to wait 12 hours before getting and
                       // saving baseline
    }else{
        baseline = 1;
    }
    delay(3);                    // msec wait for initialization to complete
    if (baseline) {setbaseline();} // set baseline from EE
    // set humidity
    flt[0] = ((humidityraw & 0xFF00) >> 8);
    flt[1] = humidityraw & 0x00FF;
    sethumidity();
    // now ready to read VOC anytime or every minute and save baseline every
    // hour, optionally log data every 10 min or hour
    // dv

    /* set up logging here, logger uses this version of time ugh */
    configTime(0, 0, "pool.ntp.org");

    logger.init();
    hist_logger.init();
    /*  our data record size: 52 bytes*/
    Serial.printf("sizeof(data_sample_t): %zu\n", sizeof(data_sample_t));
    /* time_t timestamp + our data, as stored in SPIFFS: 56 bytes */
    Serial.printf("sizeof(SPIFFSLogData<data_sample_t>): %zu\n", sizeof(SPIFFSLogData<data_sample_t>));

    Serial.println("*** Leaving setup()");
}

//************************************************************
// Main Loop
//************************************************************
void loop() {
    uint16_t ret;
    uint16_t data_ready;

    // Get Time Update
    if ((getMinutesFromLastRefresh() >= minutesBetweenDataRefresh) ||
        lastEpoch == 0) {
        getUpdateTime();
    }

    /* do the logging stuff, required */
//    Serial.printf("******** Calling process!");
    logger.process();
    hist_logger.process();

    if (lastMinute != timeClient.getMinutes()) {
        // Check status every 60 seconds
        digitalWrite(externalLight, LOW);
        lastMinute = timeClient.getMinutes(); // reset the check value
        digitalWrite(externalLight, HIGH);
        // dv once per minute, read sensors, temp and humidity, particle counter
        // and VOC sensor
        starttemprh();  /** @todo CGR not sure this makes sense */
        delay(15);
        readtemprh(); // read started in setup
        Serial.println();
        Serial.print("temp     = ");
        Serial.print(tempC);
        Serial.println(" C");

        Serial.print("humidity = ");
        Serial.print(RH);
        Serial.println(" %"); //

        // sps30 particle   iic 0x69, shtc1 rh,t iic 0x70 (RH and T on svm30voc
        // board), sgp30 VOC sensor  iic 0x58 (VOC on svm30voc board)

        do {
            ret = sps30_read_data_ready(&data_ready);
            if (ret < 0) {
                Serial.print("error reading data-ready flag: ");
                Serial.println(ret);
            } else if (!data_ready)
                Serial.print("data not ready, no new measurement available\n");
            else
                break;
            delay(100); /* retry in 100ms */
        } while (1);

        ret = sps30_read_measurement(&data_sample.m);
        if (ret < 0) {
            Serial.print("error reading measurement\n");
            delay(1000);
        } else {
            struct sps30_measurement *m =&data_sample.m;
            Serial.print("pm1.0 ug/m^3= ");
            Serial.println(m->mc_1p0);
            Serial.print("pm2.5 ug/m^3= ");
            Serial.println(m->mc_2p5 - m->mc_1p0);
            Serial.print("pm4.0 ug/m^3= ");
            Serial.println(m->mc_4p0 - m->mc_2p5);
            Serial.print("pm10. ug/m^3= ");
            Serial.println(m->mc_10p0 - m->mc_4p0);
            Serial.print("particle count 0.5 #/cm^3= ");
            Serial.println(m->nc_0p5);
            Serial.print("particle count 1.0 #/cm^3= ");
            Serial.println(m->nc_1p0 - m->nc_0p5);
            Serial.print("particle count 2.5 #/cm^3= ");
            Serial.println(m->nc_2p5 - m->nc_1p0);
            Serial.print("particle count 4.0 #/cm^3= ");
            Serial.println(m->nc_4p0 - m->nc_2p5);
            Serial.print("particle count 10. #/cm^3= ");
            Serial.println(m->nc_10p0 - m->nc_4p0);
            Serial.print("avg particlesize um= ");
            Serial.println(m->typical_particle_size);
        } // end read particle measurments
        // read VOC
        mincount++;
        iicWr2byteandcrc(0x58, 0x20, 0x08); // read VOC command
        delay(20);                          // msec
        if (iicRdVOC(0x58, 6))
            Serial.println("crc fail"); // get VOC data should be read at
                                          // 1Hz according to SVM30 datasheet
        // format data and print data[0msb,1lsb] CO2eqv (ppm), data[3msb,4lsb]
        // TVOC (ppb)
        CO2eq = (data[0] << 8) | data[1];
        Serial.print("CO2eq= ");
        Serial.print(CO2eq);
        Serial.println(" ppm");
        TVOC = (data[3] << 8) | data[4];
        Serial.print("TVOC= ");
        Serial.print(TVOC);
        Serial.println(" ppb");
        // housekeeping, on first start wait 12 hours and save baseline, after
        // that save baseline every hour
        Serial.print("baseline= ");
        Serial.println(baseline);
        if (baseline == 0) {                   // wait 12 hours if no baseline
            if (mincount >= 720) baseline = 1; // 720 min = 12 hours
        }
        if ((baseline == 1) && (mincount >= 60)) {
            iicWr2byteandcrc(0x58, 0x20, 0x15); // get baseline
            if (iicRdVOC(0x58, 6))
                Serial.println("crc fail"); // get baseline data
            else {
                flt[0] = data[0];
                flt[1] = data[1];
                flt[2] = data[3];
                flt[3] = data[4];
                EEPROM.put(0, flt);
                EEPROM.commit();
                // set humidity
                flt[0] = ((humidityraw & 0xFF00) >> 8);
                flt[1] = humidityraw & 0x00FF;
                sethumidity();
                // log data
                mincount = 0;
            }
        }

        /* fill out the voc and co2 in the data sample */
        data_sample.CO2eq = CO2eq;
        data_sample.TVOC  = TVOC;

        float volts =
            (float) analogRead(A0) / 57.8; /* this divisor is a bit empirical */
        data_sample.volts = volts;
        const time_t now  = time(nullptr);
        logger.write(data_sample);
        
        /* write our historical sample every hour */
        if (timeClient.getMinutes() == "00"){
            hist_logger.write(data_sample);
        }

        /* check if we need to alarm */
        if (alarm_enable && data_sample.m.mc_10p0 > alarm_high){
            sound_alarm(3,300,300);
        } else if (alarm_enable && data_sample.m.mc_10p0 > alarm_low){
             sound_alarm(1,200,0);
        } else {
            alarm_enable = true;
        }

        size_t row_count = logger.rowCount(now);
        Serial.printf("Number of rows is now %zu\n", row_count);
        SPIFFS.info(fs_info);
        Serial.printf("Spiffs total %d, used %d\n", fs_info.totalBytes, fs_info.usedBytes);

    }

    ui.update();

    if (WEBSERVER_ENABLED) { server.handleClient(); }
    if (ENABLE_OTA) { ArduinoOTA.handle(); }
}

// dv
uint8_t CalcCrc(uint8_t* data) { // from sensirion SGP30 datasheet
    uint8_t crc = 0xFF;
    for (int i = 0; i < 2; i++) {
        crc ^= data[i];
        for (uint8_t bit = 8; bit > 0; --bit) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31u; // xor 49
            } else {
                crc = (crc << 1);
            }
        }
    }
    return crc;
}

void starttemprh(void){ // starts sensirion SVM30 temperature measurements
  // start measurment which takes 15msec after start
  // dv for temp and RH, write  0x5c, 0x24, wait 15msec, read and 4 bytes data with 2 crc's, save in ram, calc to display
  Wire.setClock(100000L);             // all Sensrion devices use iic ant 100KHz max!
  Wire.beginTransmission(0x70);       // transmit to device 0x70, R + RH
  Wire.write(byte(0x78));             // sends command to start measurement (read temp first)
  Wire.write(byte(0x66));             // sends command to start measurement
  data[0]=0x78;data[1]=0x66;
  result=CalcCrc(data);
  Wire.write(byte(result));            // sends chk
  Wire.endTransmission();             // stop transmitting
}

void readtemprh(void){ // read sensirion SVM30 temperature and humidity measurements
  // dv for temp and RH, after 15msec, read and 4 bytes data with 2 crc's, save in ram, calc to display
  bool ret = 0;
  Wire.requestFrom(0x70, 6);          // request data bytes from slave device 0x70
  if (6 <= Wire.available()) {
    for(tmp=0;tmp<6;tmp++) {
      data[tmp] = Wire.read();        // receive data
    }
  }
  for(tmp=0;tmp<2;tmp++){                // for 2 crc protected data pairs (6bytes)
    for(result=0;result<3;result++){        // read 2 bytes data and crc
      flt[result]=data[(tmp*3)+result];
    }
    if(CalcCrc(flt)!=flt[2]) ret = true; // crc fail
  }
  if(ret) {
    Serial.println("crc failed");
  }
  else {
    readings[0]=data[1];
    readings[0]|=(data[0]<<8);
    tempraw = readings[0];
    tempC = -45.68+(175.7*(((float)tempraw)/65536.0));
    readings[1]=data[4];
    readings[1]|=(data[3]<<8);
    humidityraw = readings[1];
    RH = ((103.7-(3.2*((float)tempraw)/65536.0))*(((float)humidityraw)/65536.0));
  }
}

void iicWr2byteandcrc(uint8_t addr, uint8_t hibyte, uint8_t lobyte) {
  // send initialization command
  Wire.setClock(100000L);             // all Sensirion devices use iic ant 100KHz max!
  Wire.beginTransmission(addr);       // transmit to device 0x58, VOC detector
  Wire.write(byte(hibyte));             // sends command byte
  Wire.write(byte(lobyte));             // sends dummy byte
  data[0]=hibyte;data[1]=lobyte;
  result=CalcCrc(data);
  Wire.write(byte(result));            // sends chk
  Wire.endTransmission();             // stop transmitting
}

uint8_t iicRdVOC(uint8_t addr, uint8_t numbytes) {// data is returned in global data[] return of 1 means fail
  uint8_t chk[10];
  Wire.requestFrom(addr, numbytes);               // request numbytes from slave device
  for(tmp=0;tmp<numbytes;tmp++){
    data[tmp]=Wire.read();
  }
  // need to crc check rx'd byte pairs, 20 readings of 2 bytes plus crc byte, compute and build crc chk array
  for(tmp=0;tmp<(numbytes/3);tmp++){     // for numbytes crc protected data pairs
    for(result=0;result<3;result++){        // read 2 bytes data and crc
      flt[result]=data[(tmp*3)+result];    // flt[] used as temp storage
    }
    if(CalcCrc(flt)==flt[2]) chk[tmp]=1; // crc pass
    else chk[tmp]=0;                     // crc fail
  }
  result=0;
  for(tmp=0;tmp<(numbytes/3);tmp++) {
    if(chk[tmp]==0) result=1;             // one or mor crc's fail
    if(chk[tmp]==0) {
//      Serial.print(tmp);Serial.print(" fail\n\r");
    }
  }
  return(result);
}

void setbaseline(void) {    // uses data from global array flt[]
    Wire.setClock(100000L); // all Sensrion devices use iic ant 100KHz max!
    Wire.beginTransmission(
        0x58); // transmit set baseline to device 0x58, VOC detector
    Wire.write(byte(0x20));   // sends command to set baseline
    Wire.write(byte(0x1e));   // sends command to set baseline
    Wire.write(byte(flt[0])); // sends msb high word
    Wire.write(byte(flt[1])); // sends lsb high word
    data[0] = flt[0];
    data[1] = flt[1];
    result   = CalcCrc(data);
    Wire.write(byte(result));  // sends crc
    Wire.write(byte(flt[2])); // sends msb low word
    Wire.write(byte(flt[3])); // sends las low word
    data[0] = flt[2];
    data[1] = flt[3];
    result   = CalcCrc(data);
    Wire.write(byte(result)); // sends crc
    Wire.endTransmission();  // stop transmitting
}

void sethumidity(void) {    // uses data from global array flt[]
    Wire.setClock(100000L); // all Sensrion devices use iic ant 100KHz max!
    Wire.beginTransmission(
        0x58); // transmit set humidity to device 0x58, VOC detector
    Wire.write(byte(0x20));   // sends command to set humidity
    Wire.write(byte(0x61));   // sends command to set humidity
    Wire.write(byte(flt[0])); // sends msb high word
    Wire.write(byte(flt[1])); // sends lsb low word
    data[0] = flt[0];
    data[1] = flt[1];
    result   = CalcCrc(data);
    Wire.write(byte(result)); // sends crc
    Wire.endTransmission();  // stop transmitting
    Serial.println("set humidity");
}
//dv

void getUpdateTime() {
    digitalWrite(externalLight, LOW); // turn on the LED
    Serial.println();

    if (DISPLAYWEATHER) {
        Serial.println("Getting Weather Data...");
        weatherClient.updateWeather();
    }

    Serial.println("Updating Time...");
    // Update the Time
    timeClient.updateTime();
    lastEpoch = timeClient.getCurrentEpoch();
    Serial.println("Local time: " + timeClient.getAmPmFormattedTime());

    digitalWrite(externalLight, HIGH); // turn off the LED
}

boolean authentication() {
    if (IS_BASIC_AUTH &&
        (strlen(www_username) >= 1 && strlen(www_password) >= 1)) {
        return server.authenticate(www_username, www_password);
    }
    return true; // Authentication not required
}


void handleClean() {
    if (!authentication()) { return server.requestAuthentication(); }
    /* run manual cleaning cycle here */
    sps30_start_manual_fan_cleaning();
    Serial.println("Started cleaning cycle");
    redirectHome();
}

void handleSystemReset() {
    if (!authentication()) { return server.requestAuthentication(); }
    Serial.println("Reset System Configuration");
    if (SPIFFS.remove(CONFIG)) {
        redirectHome();
        ESP.restart();
    }
}

void handleUpdateWeather() {
    if (!authentication()) { return server.requestAuthentication(); }
    DISPLAYWEATHER  = server.hasArg("isWeatherEnabled");
    WeatherApiKey   = server.arg("openWeatherMapApiKey");
    CityIDs[0]      = server.arg("city1").toInt();
    IS_METRIC       = server.hasArg("metric");
    WeatherLanguage = server.arg("language");
    writeSettings();
    isClockOn = false; // this will force a check for the display
    lastEpoch = 0;
    redirectHome();
}

void handleUpdateConfig() {
    boolean flipOld = INVERT_DISPLAY;
    if (!authentication()) { return server.requestAuthentication(); }
    IS_24HOUR                 = server.hasArg("is24hour");
    INVERT_DISPLAY            = server.hasArg("invDisp");
    minutesBetweenDataRefresh = server.arg("refresh").toInt();
    themeColor                = server.arg("theme");
    UtcOffset                 = server.arg("utcoffset").toFloat();
    String temp               = server.arg("userid");
    alarm_high                = server.arg("himcalarm").toFloat();
    alarm_low                 = server.arg("lomcalarm").toFloat();
    temp.toCharArray(www_username, sizeof(temp));
    temp = server.arg("stationpassword");
    temp.toCharArray(www_password, sizeof(temp));
    writeSettings();
    if (INVERT_DISPLAY != flipOld) {
        ui.init();
        if (INVERT_DISPLAY) display.flipScreenVertically();
        ui.update();
    }
    lastEpoch = 0;
    redirectHome();
}

void handleWifiReset() {
    if (!authentication()) { return server.requestAuthentication(); }
    // WiFiManager
    // Local intialization. Once its business is done, there is no need to keep
    // it around
    redirectHome();
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    ESP.restart();
}

void handleWeatherConfigure() {
    if (!authentication()) { return server.requestAuthentication(); }
    digitalWrite(externalLight, LOW);
    String html = "";

    server.sendHeader("Cache-Control", "no-cache, no-store");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");

    html = getHeader();
    server.sendContent(html);

    String form             = WEATHER_FORM;
    String isWeatherChecked = "";
    if (DISPLAYWEATHER) { isWeatherChecked = "checked='checked'"; }
    form.replace("%IS_WEATHER_CHECKED%", isWeatherChecked);
    form.replace("%WEATHERKEY%", WeatherApiKey);
    form.replace("%CITYNAME1%", weatherClient.getCity(0));
    form.replace("%CITY1%", String(CityIDs[0]));
    String checked = "";
    if (IS_METRIC) { checked = "checked='checked'"; }
    form.replace("%METRIC%", checked);
    String options = LANG_OPTIONS;
    options.replace(">" + String(WeatherLanguage) + "<",
                    " selected>" + String(WeatherLanguage) + "<");
    form.replace("%LANGUAGEOPTIONS%", options);
    server.sendContent(form);

    html = getFooter();
    server.sendContent(html);
    server.sendContent("");
    server.client().stop();
    digitalWrite(externalLight, HIGH);
}

void handleConfigure() {
    if (!authentication()) { return server.requestAuthentication(); }
    digitalWrite(externalLight, LOW);
    String html = "";

    server.sendHeader("Cache-Control", "no-cache, no-store");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");

    html = getHeader();
    server.sendContent(html);

    String form = CHANGE_FORM;

    String is24hourChecked = "";
    if (IS_24HOUR) { is24hourChecked = "checked='checked'"; }
    form.replace("%IS_24HOUR_CHECKED%", is24hourChecked);
    String isInvDisp = "";
    if (INVERT_DISPLAY) { isInvDisp = "checked='checked'"; }
    form.replace("%IS_INVDISP_CHECKED%", isInvDisp);
    String hasPSUchecked = "";

    String options = "<option>10</option><option>15</option><option>20</"
                     "option><option>30</option><option>60</option>";
    options.replace(">" + String(minutesBetweenDataRefresh) + "<",
                    " selected>" + String(minutesBetweenDataRefresh) + "<");
    form.replace("%OPTIONS%", options);

    server.sendContent(form);

    form = ALARM_FORM;
    form.replace("%LOMCALARM%", String(alarm_low));
    form.replace("%HIMCALARM%", String(alarm_high));
    server.sendContent(form);

    form = THEME_FORM;

    String themeOptions = COLOR_THEMES;
    themeOptions.replace(">" + String(themeColor) + "<",
                         " selected>" + String(themeColor) + "<");
    form.replace("%THEME_OPTIONS%", themeOptions);
    form.replace("%UTCOFFSET%", String(UtcOffset));
    String isUseSecurityChecked = "";
    if (IS_BASIC_AUTH) { isUseSecurityChecked = "checked='checked'"; }
    form.replace("%IS_BASICAUTH_CHECKED%", isUseSecurityChecked);
    form.replace("%USERID%", String(www_username));
    form.replace("%STATIONPASSWORD%", String(www_password));

    server.sendContent(form);

    html = getFooter();
    server.sendContent(html);
    server.sendContent("");
    server.client().stop();
    digitalWrite(externalLight, HIGH);
}

void displayMessage(String message) {
    digitalWrite(externalLight, LOW);

    server.sendHeader("Cache-Control", "no-cache, no-store");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    String html = getHeader();
    server.sendContent(String(html));
    server.sendContent(String(message));
    html = getFooter();
    server.sendContent(String(html));
    server.sendContent("");
    server.client().stop();

    digitalWrite(externalLight, HIGH);
}

void redirectHome() {
    // Send them back to the Root Directory
    server.sendHeader("Location", String("/"), true);
    server.sendHeader("Cache-Control", "no-cache, no-store");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.send(302, "text/plain", "");
    server.client().stop();
}

String getHeader() {
  return getHeader(false);
}

String getHeader(boolean refresh) {
    String menu = WEB_ACTIONS;

    String html = "<!DOCTYPE HTML>";
    html += "<html><head>";
    html += "<style>table {  }table, th, td {  border: 1px solid black;  border-collapse: collapse;}th, td {  padding: 15px;  text-align: left;}#t01 tr:nth-child(even) {  background-color: #eee;";
    html += "}#t01 tr:nth-child(odd) { background-color: #fff;}#t01 th {  background-color: grey;  color: white;}</style>";
    html += "<title>Air Quality Monitor</title>"; //<link rel='icon' href='data:;base64,='>"; // dv
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    if (refresh) { html += "<meta http-equiv=\"refresh\" content=\"30\">"; }
    html += "<link rel='stylesheet' "
            "href='https://www.w3schools.com/w3css/4/w3.css'>";
    html +=
        "<link rel='stylesheet' href='https://www.w3schools.com/lib/w3-theme-" +
        themeColor + ".css'>";
    html += "<link rel='stylesheet' "
            "href='https://cdnjs.cloudflare.com/ajax/libs/font-awesome/4.7.0/css/font-awesome.min.css'>";
    html += "</head><body>";
    html += "<nav class='w3-sidebar w3-bar-block w3-card' "
            "style='margin-top:88px' id='mySidebar'>";
    html += "<div class='w3-container w3-theme-d2'>";
    html += "<span onclick='closeSidebar()' class='w3-button "
            "w3-display-topright w3-large'><i class='fa fa-times'></i></span>";
    html += "<div class='w3-cell w3-left w3-xxxlarge' style='width:60px'><i "
            "class='fa fa-cube'></i></div>";
    html += "<div class='w3-padding'>Menu</div></div>";
    html += menu;
    html += "</nav>";
    html += "<header class='w3-top w3-bar w3-theme'><button class='w3-bar-item "
            "w3-button w3-xxxlarge w3-hover-theme' onclick='openSidebar()'><i "
            "class='fa fa-bars'></i></button><h2 class='w3-bar-item'>Air Quality Monitor</h2></header>"; // dv
    html += "<script>";
    html += "function "
            "openSidebar(){document.getElementById('mySidebar').style.display='"
            "block'}function "
            "closeSidebar(){document.getElementById('mySidebar').style.display="
            "'none'}closeSidebar();";
    html += "</script>";
    html += "<br><div class='w3-container w3-large' style='margin-top:88px'>";
    return html;
}

String getFooter() {
    int8_t rssi = getWifiQuality();
    Serial.print("Signal Strength (RSSI): ");
    Serial.print(rssi);
    Serial.println("%");
    String html = "<br><br><br>";
    html += "</div>";
    html += "<footer class='w3-container w3-bottom w3-theme w3-margin-top'>";
    if (lastReportStatus != "") {
        html += "<i class='fa fa-external-link'></i> Report Status: " +
                lastReportStatus + "<br>";
    }
    html += "<i class='fa fa-paper-plane-o'></i> Version: " + String(VERSION) +
            "<br>";
    html += "<i class='fa fa-rss'></i> Signal Strength: ";
    html += String(rssi) + "%";
    html += "</footer>";
    html += "</body></html>";
    return html;
}

void sound_alarm(uint16_t count, uint16_t on_dur, uint16_t off_dur) {
    for (int i = 0; i < count; i++) {
        digitalWrite(ALARM_PIN, 1);
        delay(on_dur);
        digitalWrite(ALARM_PIN, 0);
        delay(off_dur);
    }
}

void display_air_quality() {
    digitalWrite(externalLight, LOW);
    struct sps30_measurement *m = &data_sample.m;

    String html = "";

    server.sendHeader("Cache-Control", "no-cache, no-store");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    server.sendContent(String(getHeader(true)));

    String displayTime = timeClient.getAmPmHours() + ":" +
                         timeClient.getMinutes() + ":" +
                         timeClient.getSeconds() + " " + timeClient.getAmPm();
    if (IS_24HOUR) {
        displayTime = timeClient.getHours() + ":" + timeClient.getMinutes() +
                      ":" + timeClient.getSeconds();
    }

    html +=
        "<div class='w3-cell-row' style='width:100%'><h2>Time: " + displayTime +
        "</h2></div><div class='w3-cell-row'>";
    html += "<div class='w3-cell w3-container' style='width:100%'><p>";
    html += "<table id=t01>";
    html += "<tr>";
    html += "<th>Size</th><th>Count</th><th>ug/m^3</th>"; // dv
    html += "</tr><tr>";
    html += "<td>PM<sub>0.5</sub></td><td>" + String(m->nc_0p5) + "</td><td>--</td>";
    html += "</tr><tr>";
    html += "<td>PM<sub>1.0</sub></td><td>" + String(m->nc_1p0 - m->nc_0p5) + "</td><td>" +  String(m->mc_1p0) + "</td>";
    html += "</tr><tr>";
    html += "<td>PM<sub>2.5</sub></td><td>" + String(m->nc_2p5 - m->nc_1p0) + "</td><td>" +  String(m->mc_2p5 - m->mc_1p0) + "</td>";
    html += "</tr><tr>";
    html += "<td>PM<sub>4.0</sub></td><td>" + String(m->nc_4p0 - m->nc_2p5) + "</td><td>" +  String(m->mc_4p0 - m->mc_2p5) + "</td>";
    html += "</tr><tr>";
    html += "<td>PM<sub>10</sub></td><td>" + String(m->nc_10p0 - m->nc_4p0) + "</td><td>" +  String(m->mc_10p0 - m->mc_4p0) + "</td>";
    html += "</tr></table>";
    html += "average particle size (um) : " + String(m->typical_particle_size) +
            "<br>";                                        // dv
    html += "<br>";                                        // dv
    html += "TVOC (ppb) : " + String(TVOC) + "<br>";       // dv
    html += "CO2eq (ppm) : " + String(CO2eq) + "<br>";     // dv
    html += "<br>";                                        // dv
    html += "indoor temp (C) : " + String(tempC) + "<br>"; // dv
    html += "indoor RH (%) : " + String(RH) + "<br>";      // dv
    html += "<hr>";

    html += "</p></div></div>";

    server.sendContent(html); // spit out what we got
    html = "";

    if (DISPLAYWEATHER) {
        if (weatherClient.getCity(0) == "") {
            html += "<p>Please <a href='/configureweather'>Configure "
                    "Weather</a> API</p>";
            if (weatherClient.getError() != "") {
                html += "<p>Weather Error: <strong>" +
                        weatherClient.getError() + "</strong></p>";
            }
        } else {
            html += "<div class='w3-cell-row' style='width:100%'><h2>" +
                    weatherClient.getCity(0) + ", " +
                    weatherClient.getCountry(0) +
                    "</h2></div><div class='w3-cell-row'>";
            html +=
                "<div class='w3-cell w3-left w3-medium' style='width:120px'>";
            html += "<img src='http://openweathermap.org/img/w/" +
                    weatherClient.getIcon(0) + ".png' alt='" +
                    weatherClient.getDescription(0) + "'><br>";
            html += weatherClient.getHumidity(0) + "% Humidity<br>";
            html += weatherClient.getWind(0) + " <span class='w3-tiny'>" +
                    getSpeedSymbol() + "</span> Wind<br>";
            html += "</div>";
            html += "<div class='w3-cell w3-container' style='width:100%'><p>";
            html += weatherClient.getCondition(0) + " (" +
                    weatherClient.getDescription(0) + ")<br>";
            html +=
                weatherClient.getTempRounded(0) + getTempSymbol(true) + "<br>";
            html += "<a href='https://www.google.com/maps/@" +
                    weatherClient.getLat(0) + "," + weatherClient.getLon(0) +
                    ",10000m/data=!3m1!1e3' target='_BLANK'><i class='fa "
                    "fa-map-marker' style='color:red'></i> Map It!</a><br>";
            html += "</p></div></div>";
        }

        server.sendContent(html); // spit out what we got
        html = "";                // fresh start
    }

    server.sendContent(String(getFooter()));
    server.sendContent("");
    server.client().stop();
    digitalWrite(externalLight, HIGH);
}

void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 0, "Wifi Manager");
  display.drawString(64, 10, "Please connect to AP");
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 23, myWiFiManager->getConfigPortalSSID());
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 42, "To setup Wifi connection");
  display.display();

  Serial.println("Wifi Manager");
  Serial.println("Please connect to AP");
  Serial.println(myWiFiManager->getConfigPortalSSID());
  Serial.println("To setup Wifi Configuration");
  flashLED(20, 50);
}

void flashLED(int number, int delayTime) {
  for (int inx = 0; inx < number; inx++) {
      delay(delayTime);
      digitalWrite(externalLight, LOW);
      delay(delayTime);
      digitalWrite(externalLight, HIGH);
      delay(delayTime);
  }
}

void drawTinyAQ(OLEDDisplay*        display,
               OLEDDisplayUiState* state,
               int16_t             x,
               int16_t             y) {

    struct sps30_measurement* m = &data_sample.m;


    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(ArialMT_Plain_10);

    display->drawString(x+0, 10, "pm1.0");
    display->drawString(x+32, 10, String(m->mc_1p0));
    display->drawString(x+68, 10, "pm2.5");
    display->drawString(x+100, 10, String(m->mc_2p5 - m->mc_1p0));
    display->drawString(x+0, 20, "pm4.0");
    display->drawString(x+32, 20, String(m->mc_4p0 - m->mc_2p5));
    display->drawString(x+68, 20, "pm10.");
    display->drawString(x+100, 20, String(m->mc_10p0 - m->mc_4p0));
    display->drawString(x+0, 30, "avg um");
    display->drawString(x+38, 30, String(m->typical_particle_size));
    display->drawString(x+62, 30, "tvocppb");
    display->drawString(x+103, 30, String(TVOC)); //
    //  display->drawString( 0, 40, "aaaaaaaaaaaaaaaaaaaaaaaaaa");// 4th and 5th
    //  line render in area below horizontal line display->drawString( 0, 50,
    //  "bbbbbbbbbbbbbbbbbbbbbbbbbb");
    /*
      String displayTime = timeClient.getAmPmHours() + ":" +
      timeClient.getMinutes() + ":" + timeClient.getSeconds(); if (IS_24HOUR) {
        displayTime = timeClient.getHours() + ":" + timeClient.getMinutes() +
      ":" + timeClient.getSeconds();
      }
      display->setFont(ArialMT_Plain_16);
      display->drawString(64 + x, 0 + y, OctoPrintHostName);
      display->setFont(ArialMT_Plain_24);
      display->drawString(64 + x, 17 + y, displayTime);
    */
}

void drawPM(OLEDDisplay*        display,
              OLEDDisplayUiState* state,
              int16_t             x,
              int16_t             y,
              String              pm,
              String              count,
              String              ug) {
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(ArialMT_Plain_24);
    display->drawString(x + 0, 0, "PM");
    display->drawString(x + 0, 20, pm);
    display->setFont(ArialMT_Plain_16);
    display->drawString(x + 45, 0, "#");
    display->drawString(x + 45, 20, "ug");
    display->drawString(x + 75, 0, count);
    display->drawString(x + 75, 20, ug);
}

void drawPM05(OLEDDisplay*        display,
              OLEDDisplayUiState* state,
              int16_t             x,
              int16_t             y) {
            struct sps30_measurement *m =&data_sample.m;

    drawPM(display, state, x, y, "0.5", String(m->nc_0p5), String("--"));
}
void drawPM10(OLEDDisplay*        display,
              OLEDDisplayUiState* state,
              int16_t             x,
              int16_t             y) {
            struct sps30_measurement *m =&data_sample.m;

    drawPM(display,
           state,
           x,
           y,
           "1.0",
           String(m->nc_1p0 - m->nc_0p5),
           String(m->mc_1p0));
}
void drawPM25(OLEDDisplay*        display,
              OLEDDisplayUiState* state,
              int16_t             x,
              int16_t             y) {
            struct sps30_measurement *m =&data_sample.m;
    drawPM(display,
           state,
           x,
           y,
           "2.5",
           String(m->nc_2p5 - m->nc_1p0),
           String(m->mc_2p5 - m->mc_1p0));
}
void drawPM40(OLEDDisplay*        display,
              OLEDDisplayUiState* state,
              int16_t             x,
              int16_t             y) {
            struct sps30_measurement *m =&data_sample.m;
    drawPM(display,
           state,
           x,
           y,
           "4.0",
           String(m->nc_4p0 - m->nc_2p5),
           String(m->mc_4p0 - m->mc_2p5));
}
void drawPM100(OLEDDisplay*        display,
               OLEDDisplayUiState* state,
               int16_t             x,
               int16_t             y) {
            struct sps30_measurement *m =&data_sample.m;
    drawPM(display,
           state,
           x,
           y,
           "10",
           String(m->nc_10p0 - m->nc_4p0),
           String(m->mc_10p0 - m->mc_4p0));
}

void drawWeather(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_24);
  display->drawString(0 + x, 0 + y, weatherClient.getTempRounded(0) + getTempSymbol());
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_24);

  display->setFont(ArialMT_Plain_16);
  display->drawString(0 + x, 24 + y, weatherClient.getCondition(0));
  display->setFont((const uint8_t*)Meteocons_Plain_42);
  display->drawString(86 + x, 0 + y, weatherClient.getWeatherIcon(0));
}

String getTempSymbol() {
  return getTempSymbol(false);
}

String getTempSymbol(boolean forHTML) {
  String rtnValue = "F";
  if (IS_METRIC) {
    rtnValue = "C";
  }
  if (forHTML) {
    rtnValue = "&#176;" + rtnValue;
  } else {
    rtnValue = "°" + rtnValue;
  }
  return rtnValue;
}

String getSpeedSymbol() {
  String rtnValue = "mph";
  if (IS_METRIC) {
    rtnValue = "kph";
  }
  return rtnValue;
}

String zeroPad(int value) {
  String rtnValue = String(value);
  if (value < 10) {
    rtnValue = "0" + rtnValue;
  }
  return rtnValue;
}

void drawHeaderOverlay(OLEDDisplay *display, OLEDDisplayUiState* state) {
  display->setColor(WHITE);
  display->setFont(ArialMT_Plain_16);
  String displayTime = timeClient.getAmPmHours() + ":" + timeClient.getMinutes();
  if (IS_24HOUR) {
    displayTime = timeClient.getHours() + ":" + timeClient.getMinutes();
  }
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0, 48, displayTime);

  if (!IS_24HOUR) {
    String ampm = timeClient.getAmPm();
    display->setFont(ArialMT_Plain_10);
    display->drawString(39, 54, ampm);
  }

  display->setFont(ArialMT_Plain_16);
  display->setTextAlignment(TEXT_ALIGN_LEFT);

  /* CGR Put battery percentage here !!!! */
  String percent = String(50) + "%";
  display->drawString(64, 48, percent);

  // Draw indicator to show next update
  int updatePos = (50.0 / float(100)) * 128;
  display->drawRect(0, 44, 128, 2);
  /*
  display->drawHorizontalLine(0, 42, updatePos);
  display->drawHorizontalLine(0, 43, updatePos);
  display->drawHorizontalLine(0, 44, updatePos);
  display->drawHorizontalLine(0, 45, updatePos);
  */
  drawRssi(display);
}

void drawRssi(OLEDDisplay* display) {
    int8_t quality = getWifiQuality();
    for (int8_t i = 0; i < 4; i++) {
        for (int8_t j = 0; j < 3 * (i + 2); j++) {
            if (quality > i * 25 || j == 0) {
                display->setPixel(114 + 4 * i, 63 - j);
            }
        }
    }
}

// converts the dBm to a range between 0 and 100%
int8_t getWifiQuality() {
    int32_t dbm = WiFi.RSSI();
    if (dbm <= -100) {
        return 0;
    } else if (dbm >= -50) {
        return 100;
    } else {
        return 2 * (dbm + 100);
    }
}

void writeSettings() {
  // Save decoded message to SPIFFS file for playback on power up.
  File f = SPIFFS.open(CONFIG, "w");
  if (!f) {
    Serial.println("File open failed!");
  } else {
    Serial.println("Saving settings now...");
    f.println("UtcOffset=" + String(UtcOffset));
    f.println("refreshRate=" + String(minutesBetweenDataRefresh));
    f.println("themeColor=" + themeColor);
    f.println("IS_BASIC_AUTH=" + String(IS_BASIC_AUTH));
    f.println("www_username=" + String(www_username));
    f.println("www_password=" + String(www_password));
    f.println("is24hour=" + String(IS_24HOUR));
    f.println("invertDisp=" + String(INVERT_DISPLAY));
    f.println("isWeather=" + String(DISPLAYWEATHER));
    f.println("weatherKey=" + WeatherApiKey);
    f.println("CityID=" + String(CityIDs[0]));
    f.println("isMetric=" + String(IS_METRIC));
    f.println("language=" + String(WeatherLanguage));
    f.println("himcalarm=" + String(alarm_high));
    f.println("lomcalarm=" + String(alarm_low));
  }
  f.close();
  readSettings();
  timeClient.setUtcOffset(UtcOffset);
}

void readSettings() {
    if (SPIFFS.exists(CONFIG) == false) {
        Serial.println("Settings File does not yet exists.");
        writeSettings();
        return;
    }
    File   fr = SPIFFS.open(CONFIG, "r");
    String line;
    while (fr.available()) {
        line = fr.readStringUntil('\n');

        if (line.indexOf("himcalarm=") >= 0) {
            alarm_high =
                line.substring(line.lastIndexOf("himcalarm=") + 10).toFloat();
            Serial.println("himcalarm=" + String(alarm_high));
        }
        if (line.indexOf("lomcalarm=") >= 0) {
            alarm_low =
                line.substring(line.lastIndexOf("lomcalarm=") + 10).toFloat();
            Serial.println("lomcalarm=" + String(alarm_low));
        }

        if (line.indexOf("UtcOffset=") >= 0) {
            UtcOffset =
                line.substring(line.lastIndexOf("UtcOffset=") + 10).toFloat();
            Serial.println("UtcOffset=" + String(UtcOffset));
        }

        if (line.indexOf("refreshRate=") >= 0) {
            minutesBetweenDataRefresh =
                line.substring(line.lastIndexOf("refreshRate=") + 12).toInt();
            Serial.println("minutesBetweenDataRefresh=" +
                           String(minutesBetweenDataRefresh));
        }
        if (line.indexOf("themeColor=") >= 0) {
            themeColor = line.substring(line.lastIndexOf("themeColor=") + 11);
            themeColor.trim();
            Serial.println("themeColor=" + themeColor);
        }
        if (line.indexOf("IS_BASIC_AUTH=") >= 0) {
            IS_BASIC_AUTH =
                line.substring(line.lastIndexOf("IS_BASIC_AUTH=") + 14).toInt();
            Serial.println("IS_BASIC_AUTH=" + String(IS_BASIC_AUTH));
        }
        if (line.indexOf("www_username=") >= 0) {
            String temp =
                line.substring(line.lastIndexOf("www_username=") + 13);
            temp.trim();
            temp.toCharArray(www_username, sizeof(temp));
            Serial.println("www_username=" + String(www_username));
        }
        if (line.indexOf("www_password=") >= 0) {
            String temp =
                line.substring(line.lastIndexOf("www_password=") + 13);
            temp.trim();
            temp.toCharArray(www_password, sizeof(temp));
            Serial.println("www_password=" + String(www_password));
        }
        if (line.indexOf("is24hour=") >= 0) {
            IS_24HOUR =
                line.substring(line.lastIndexOf("is24hour=") + 9).toInt();
            Serial.println("IS_24HOUR=" + String(IS_24HOUR));
        }
        if (line.indexOf("invertDisp=") >= 0) {
            INVERT_DISPLAY =
                line.substring(line.lastIndexOf("invertDisp=") + 11).toInt();
            Serial.println("INVERT_DISPLAY=" + String(INVERT_DISPLAY));
        }

        if (line.indexOf("isWeather=") >= 0) {
            DISPLAYWEATHER =
                line.substring(line.lastIndexOf("isWeather=") + 10).toInt();
            Serial.println("DISPLAYWEATHER=" + String(DISPLAYWEATHER));
        }
        if (line.indexOf("weatherKey=") >= 0) {
            WeatherApiKey =
                line.substring(line.lastIndexOf("weatherKey=") + 11);
            WeatherApiKey.trim();
            Serial.println("WeatherApiKey=" + WeatherApiKey);
        }
        if (line.indexOf("CityID=") >= 0) {
            CityIDs[0] =
                line.substring(line.lastIndexOf("CityID=") + 7).toInt();
            Serial.println("CityID: " + String(CityIDs[0]));
        }
        if (line.indexOf("isMetric=") >= 0) {
            IS_METRIC =
                line.substring(line.lastIndexOf("isMetric=") + 9).toInt();
            Serial.println("IS_METRIC=" + String(IS_METRIC));
        }
        if (line.indexOf("language=") >= 0) {
            WeatherLanguage = line.substring(line.lastIndexOf("language=") + 9);
            WeatherLanguage.trim();
            Serial.println("WeatherLanguage=" + WeatherLanguage);
        }
    }
    fr.close();
    weatherClient.updateWeatherApiKey(WeatherApiKey);
    weatherClient.updateLanguage(WeatherLanguage);
    weatherClient.setMetric(IS_METRIC);
    weatherClient.updateCityIdList(CityIDs, 1);
    timeClient.setUtcOffset(UtcOffset);
}

int getMinutesFromLastRefresh() {
    int minutes = (timeClient.getCurrentEpoch() - lastEpoch) / 60;
    return minutes;
}

int getMinutesFromLastDisplay() {
    int minutes = (timeClient.getCurrentEpoch() - displayOffEpoch) / 60;
    return minutes;
}

/* web logging code */
void handleWebRequests(){
  if(loadFromSpiffs(server.uri())) return;
  String message = "File Not Detected\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " NAME:"+server.argName(i) + "\n VALUE:" + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
  Serial.println(message);
}

bool loadFromSpiffs(String path){
  String dataType = "text/plain";
  if(path.endsWith("/")) path += "index.htm";

  if(path.endsWith(".src")) path = path.substring(0, path.lastIndexOf("."));
  else if(path.endsWith(".html")) dataType = "text/html";
  else if(path.endsWith(".htm")) dataType = "text/html";
  else if(path.endsWith(".css")) dataType = "text/css";
  else if(path.endsWith(".js")) dataType = "application/javascript";
  else if(path.endsWith(".png")) dataType = "image/png";
  else if(path.endsWith(".gif")) dataType = "image/gif";
  else if(path.endsWith(".jpg")) dataType = "image/jpeg";
  else if(path.endsWith(".ico")) dataType = "image/x-icon";
  else if(path.endsWith(".xml")) dataType = "text/xml";
  else if(path.endsWith(".pdf")) dataType = "application/pdf";
  else if(path.endsWith(".zip")) dataType = "application/zip";
  File dataFile = SPIFFS.open(path.c_str(), "r");
  if (server.hasArg("download")) dataType = "application/octet-stream";
  if (server.streamFile(dataFile, dataType) != dataFile.size()) {
  }

  dataFile.close();
  return true;
}

void handle_quality() {

    /* pass along the battery voltage */
    float volts =
        (float) analogRead(A0) / 57.8; /* this divisor is a bit empirical */
    struct sps30_measurement* m = &data_sample.m;

    String quality = String(m->mc_1p0) + "," + String(m->mc_2p5 - m->mc_1p0) +
                     "," + String(m->mc_4p0 - m->mc_2p5) + "," +
                     String(m->mc_10p0 - m->mc_4p0) + "," +
                     String(data_sample.TVOC) + "," +
                     String(data_sample.CO2eq) + "," + String(volts);
    server.send(200,
                "text/plain",
                quality); // Send air quality data to client ajax request
}

/* Send the whole data set on this request,
 * and we do it incrementally */
void handle_daily() {
    int                                 i = 0;
    struct SPIFFSLogData<data_sample_t> sample[5]; /* just one sample at a time */
    const time_t                        now       = time(nullptr);
    size_t                              row_count = logger.rowCount(now);
    size_t                              row = 0;
    struct sps30_measurement*           m            = &sample[0].data.m;
    size_t                              buff_to_fill = row_count * 60;
    /* this is how we parse args --    int start =
     * atoi(server.arg("start").c_str()); */

    Serial.printf("Want to send %zu data points to web page\n",  row_count);

    /* send the speculative header */
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/plain", "\r\n");

    String full = String(row_count);
    server.sendContent(full);

    /* get the raw data */
    while (logger.readRows(sample, now, row, 1) && (row < row_count)) {

        //Serial.printf("Read back row %zu\n", row);
        full = "," + String(m->mc_1p0) + "," + String(m->mc_2p5 - m->mc_1p0) +
               "," + String(m->mc_4p0 - m->mc_2p5) + "," +
               String(m->mc_10p0 - m->mc_4p0) + "," +
               String(sample[0].data.TVOC) + "," +
               String(sample[0].data.CO2eq) + "," +
               String(sample[0].data.volts) + "," +
               String(sample[0].timestampUTC);

        //Serial.println("String is " + full + "\n");
        server.sendContent(full);
        row++; /* get the next row */
    }
}
/* Send the last week's data data set on this request,
 * and we do it incrementally */
void handle_weeks_data() {
    struct SPIFFSLogData<data_sample_t> h_sample[5]; /* just one sample at a time */
    int                       i            = 0;
    const time_t              now          = time(nullptr);
    size_t                    row_count    = 0;
    struct sps30_measurement* m            = &h_sample[0].data.m;
    size_t                    buff_to_fill = row_count * 60;
    /* this is how we parse args --    int start =
     * atoi(server.arg("start").c_str()); */

    row_count = 0;
    for (i = -DAYS_IN_WEEK; i <= 0; i++) {
        time_t start = now + i * SECS_IN_DAY;
        row_count += hist_logger.rowCount(start);
        /* this should give the count for the day */
        Serial.printf("Day (%d) total count %d\n", i, row_count);
    }

    /* send the speculative header */
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/plain", "\r\n");

    String full = String(row_count);
    server.sendContent(full);

    for (i = -DAYS_IN_WEEK; i <= 0; i++) {
        /* get the raw data */
        time_t start = now + i * SECS_IN_DAY;
        size_t row       = 0;
        size_t day_count = hist_logger.rowCount(start);

        Serial.printf("Day (%d) samples %d\n", i, day_count);

        /* get the raw data */
        while (hist_logger.readRows(h_sample, start, row, 1) && (row < day_count)) {

            // Serial.printf("Read back row %zu\n", row);
            full = "," + String(m->mc_1p0) + "," +
                   String(m->mc_2p5 - m->mc_1p0) + "," +
                   String(m->mc_4p0 - m->mc_2p5) + "," +
                   String(m->mc_10p0 - m->mc_4p0) + "," +
                   String(h_sample[0].data.TVOC) + "," +
                   String(h_sample[0].data.CO2eq) + "," +
                   String(h_sample[0].data.volts) + "," +
                   String(h_sample[0].timestampUTC);

            // Serial.println("String is " + full + "\n");
            server.sendContent(full);
            row++; /* get the next row */
        }
    }
}

/* Send the last week's data data set on this request,
 * and we do it incrementally */
void handle_years_data() {
    struct SPIFFSLogData<data_sample_t> h_sample[5]; /* just one sample at a time */
    int                       i            = 0;
    const time_t              now          = time(nullptr);
    size_t                    row_count    = 0;
    struct sps30_measurement* m            = &h_sample[0].data.m;
    size_t                    buff_to_fill = row_count * 60;
    /* this is how we parse args --    int start =
     * atoi(server.arg("start").c_str()); */

    row_count = 0;
    for (i = -DAYS_IN_YEAR; i <= 0; i++) {
        time_t start = now + i * SECS_IN_DAY;
        row_count += (hist_logger.rowCount(start) > 0) ? 1 : 0;
        /* this should give the count for the day */
//        Serial.printf("Day (%d) total count %d\n", i, row_count);
    }

    /* send the speculative header */
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/plain", "\r\n");

    String full = String(row_count);
    server.sendContent(full);

    for (i = -DAYS_IN_YEAR; i <= 0; i++) {
        /* get the raw data */
        time_t start = now + i * SECS_IN_DAY;
        size_t row       = 0;
        size_t day_count = hist_logger.rowCount(start);

//
        Serial.printf("Day (%d) samples %d\n", i, day_count);

        /* get the raw data */
        if ((day_count > 0) && (hist_logger.readRows(h_sample, start, row, 1))) {

            // Serial.printf("Read back row %zu\n", row);
            full = "," + String(m->mc_1p0) + "," +
                   String(m->mc_2p5 - m->mc_1p0) + "," +
                   String(m->mc_4p0 - m->mc_2p5) + "," +
                   String(m->mc_10p0 - m->mc_4p0) + "," +
                   String(h_sample[0].data.TVOC) + "," +
                   String(h_sample[0].data.CO2eq) + "," +
                   String(h_sample[0].data.volts) + "," +
                   String(h_sample[0].timestampUTC);

            // Serial.println("String is " + full + "\n");
            server.sendContent(full);
            row++; /* get the next row */
        }
    }
}

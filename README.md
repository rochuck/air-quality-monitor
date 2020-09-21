# Air Quality Monitor 

Based on:
* ESP-8266
* SPS-30 Sensirion particle detector
* SVM-30 VOC sensor

## Features:
* Displays air quality
* Display time and weather
* Fully configurable from the web interface (not required to update Settings.h)
* Supports OTA (loading firmware over WiFi connection on same LAN)
* Basic Authentication to protect your settings

## Required Parts:
* Air Quality Monitor Board (TBA)
* 0.96" OLED I2C 128x64 Display (12864) SSD1306:  https://amzn.to/3cyJekU
* (optional) 1.3" I2C OLED Display: https://amzn.to/2IP0gRU (must uncomment #define DISPLAY_SH1106 in the Settings.h to use the 1.3" SSH1106 display)  
* SVM-30 Digikey 1649-1120-nd
* SPS-30 Digikey 1649-1106-nd

## Compiling and Loading Air quality monitor board
It is recommended to use Arduino IDE an VSCode.  You will need to configure Arduino IDE to work with the Wemos board and USB port and installed the required USB drivers etc.  
* Enter http://arduino.esp8266.com/stable/package_esp8266com_index.json into Additional Board Manager URLs field. You can add multiple URLs, separating them with commas.  This will add support for the Wemos D1 Mini to Arduino IDE.
* Open Boards Manager from Tools > Board menu and install esp8266 Core platform version 2.5.2
* Select Board:  "Generic ESP8266"
* Set 4M SPIFFS -- this project uses SPIFFS for saving and reading configuration settings.

## Loading Supporting Library Files in Arduino
Use the Arduino guide for details on how to installing and manage libraries https://www.arduino.cc/en/Guide/Libraries  
**Packages** -- the following packages and libraries are used (download and install):  
ESP8266WiFi.h  
ESP8266WebServer.h  
WiFiManager.h --> https://github.com/tzapu/WiFiManager  
ESP8266mDNS.h  
ArduinoOTA.h  --> Arduino OTA Library  
"SSD1306Wire.h" --> https://github.com/ThingPulse/esp8266-oled-ssd1306  
"OLEDDisplayUi.h"  
"sps30.h" --> https://github.com/Sensirion/arduino-sps


## Initial Configuration
All settings may be managed from the Web Interface, however, you may update the **Settings.h** file manually -- but it is not required.  There is also an option to display current weather when the print is off-line.  

* Optional OpenWeatherMap API Key -- if you want current weather when not printing.  Get the api key from: https://openweathermap.org/  

NOTE: The settings in the Settings.h are the default settings for the first loading. After loading you will manage changes to the settings via the Web Interface. If you want to change settings again in the settings.h, you will need to erase the file system on the Wemos or use the “Reset Settings” option in the Web Interface.  

## Web Interface
The Printer Monitor uses the **WiFiManager** so when it can't find the last network it was connected to 
it will become a **AP Hotspot** -- connect to it with your phone and you can then enter your WiFi connection information.

After connecting to your WiFi network it will display the IP addressed assigned to it and that can be 
used to open a browser to the Web Interface.  **Everything** can be configured there.

<!--p align="center">
  <img src="/images/shot_01.png" width="200"/>
  <img src="/images/shot_02.png" width="200"/>
  <img src="/images/shot_03.png" width="200"/>
  <img src="/images/shot_04.png" width="200"/>
</p> -->

## Contributors
David Viberg -- Principal Engineer for board layout and initial software development
Chuck Rohs -- Chief S/W polisher

/* The MIT License (MIT)

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

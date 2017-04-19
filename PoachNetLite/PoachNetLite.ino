// PoachNet Lite

#include "Adafruit_FONA.h"
#include <Adafruit_GPS.h>
#include <SoftwareSerial.h>
#include <Adafruit_SleepyDog.h>

#define FONA_RST 4
#define FONA_RI  7
#define FONA_TX  8
#define FONA_RX  9
#define GPSECHO true

#define FONA_GPIO 11
#define GPS_GPIO 12

// number of MS the Feather will sleep in between SMS messages
uint32_t sleepTimeMS = 150000;

// number of MS without a cell connection until the Feather gives up sending an SMS
uint32_t gpsTimeoutMS = 60000;

// toggle on/off sending SMS messages
uint8_t sendSMSFlag = 1;

// 10-digit phone number to which SMS messages will be sent
char phoneNumber[11] = "8125895915";

// address of the Bluemix server for POST request
char url[80] = "data.cs.purdue.edu:5656";

Adafruit_FONA fona = Adafruit_FONA(FONA_RST);

SoftwareSerial fonaSS = SoftwareSerial(FONA_TX, FONA_RX);
SoftwareSerial *fonaSerial = &fonaSS;

Adafruit_GPS GPS(&Serial1);
HardwareSerial mySerial = Serial1;

uint32_t timer;

void startGPS()
{
  digitalWrite(GPS_GPIO, LOW); // enable GPS
  GPS.begin(9600);
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_1HZ); // 1 Hz update rate
  GPS.sendCommand(PGCMD_ANTENNA);
  //sGPS.fix = false;
  timer = millis();
}

void startFona()
{
  digitalWrite(FONA_GPIO, LOW); // enable fona cell
  fonaSerial->begin(4800);
  if (!fona.begin(*fonaSerial)) {
    if (sendSMS)
      return;
  }
}

void setup()
{
  Watchdog.disable();
  
  // start serial connection for debugging
  Serial.begin(115200);

  // wait for serial to connect before executing (for debug only)
  while (!Serial);

  // GPIO pin 11 controls power to Feather's cell module
  pinMode(FONA_GPIO, OUTPUT);

  // GPIO pin 12 controls power to GPS module
  pinMode(GPS_GPIO, OUTPUT);

  startGPS();
  startFona();
}

void goToSleep()
{
  fona.enableGPRS(false);

  // disable fona cell before sleeping by toggling
  digitalWrite(FONA_GPIO, HIGH);
  delay(500);
  digitalWrite(FONA_GPIO, LOW);
  delay(1500);
  digitalWrite(FONA_GPIO, HIGH);

  digitalWrite(GPS_GPIO, HIGH); // disable GPS before sleeping

  // low-power sleeps for as long as possible on each call to Watchdog.sleep()
  uint32_t sleepMS = 0;
  while (sleepMS < sleepTimeMS) {
    sleepMS += Watchdog.sleep();
  }

  startGPS();
  startFona();
}

void sendSMS(String s)
{
  // convert string to char[] for use with sendSMS
  char msg[141];
  s.toCharArray(msg, min(s.length() + 1, 140));
  if (sendSMSFlag)
    fona.sendSMS(phoneNumber, msg);
}

void loop()
{
  if (GPS.fix) {
    fona.enableGPRS(true);
    String longitude = String(GPS.longitudeDegrees, 4),
           latitude = String(GPS.latitudeDegrees, 4),
           speed = String(GPS.speed), angle = String(GPS.angle);

    String msg = "From GPS\nlat/lon: " + latitude + ", " + longitude
                 + "\nspeed: " + speed + "\nangle: " + angle;
    sendSMS(msg);

    // format POST data as JSON
    String postData = "{\"long\":\"" + longitude
                      + "\", \"lat\":\"" + latitude
                      + "\", \"speed\":\"" + speed
                      + "\", \"angle\":\"" + angle
                      + "\"}";
    char data[postData.length() + 1];
    postData.toCharArray(data, postData.length() + 1);

    // post the data
    fona.HTTP_POST_start(url, F("text/plain"), (uint8_t *) data,
                         strlen(data), NULL, NULL);
    goToSleep();
  } else if (millis() - timer >= gpsTimeoutMS) {
    String msg, latitude, longitude;
    uint16_t returncode;
    char replybuffer[255];
    if (fona.enableGPRS(true) && fona.getGSMLoc(&returncode, replybuffer, 250)) {
      char *tok = strtok(replybuffer, ",");
      longitude = String(tok);
      tok = strtok(NULL, ",");
      latitude = String(tok);
      msg = "From cell\nlat/lon: " + latitude
            + ", " + longitude;

      String postData = "{\"long\":\"" + longitude
                        + "\", \"lat\":\"" + latitude
                        + "\"}";

      char data[postData.length() + 1];
      postData.toCharArray(data, postData.length() + 1);

      fona.HTTP_POST_start(url, F("text/plain"), (uint8_t *) data,
                           strlen(data), NULL, NULL);
    } else {
      msg += "No GPS/GPRS available";
    }

    sendSMS(msg);
    goToSleep();
  }

  GPS.read();
  if (GPS.newNMEAreceived()) {
    if (!GPS.parse(GPS.lastNMEA()))
      return;
  }
}

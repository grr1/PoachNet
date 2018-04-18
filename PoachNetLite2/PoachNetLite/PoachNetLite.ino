// official main file

// software for PoachNet Lite

#include "Adafruit_FONA.h"
#include <Adafruit_GPS.h>
#include <Adafruit_SleepyDog.h>
#include <EEPROM.h>
#include <setjmp.h>

#define FONA_RST 4
#define FONA_RI  7
#define FONA_TX  8
#define FONA_RX  9
#define GPSECHO true

#define MAX_ARGS 3
#define MAX_PHONE_NUMS 4
#define PHONE_NUM_LEN 10
#define MAX_GET_NUM_SMS_TRIES 5
#define MAX_TXT_STR_SIZE 16
#define GPS_DATA_SIZE 15
#define EEPROM_SIZE 1024 // this is a fixed size for the hardware we are using currently

#define RESET_BUTTON_SEND_GPIO A3
#define BATT_LED A1 // LED that blinks once for each 10% in battery life available
#define RESET_LED A2 // LED that lights up to confirm reset button pressed
#define RESET_BUTTON_GPIO A0 // check if reset button is pressed (to wipe data)
#define FONA_GPIO 11 // control pwr to fona
#define GPS_GPIO 12 // control pwr to gps

// texting commands
const char *SNOOZE_CMD = "snooze";
const char *TOGGLE_TEXT_CMD = "texting";
const char *STATUS_CMD = "status";
const char *RESET_CMD = "reset";
const char *ADD_DEL_PHONE_CMD = "phone";
const char *NAME_CMD = "name";

#define DEFAULT_SNOOZE_MINS 5 // if no arg is specified in a SNOOZE command; a big number for default (e.g., 60 mins) is bad since that would immobilize the device until the time expires
#define MAX_SNOOZE 255 // max snooze currently 255 mins since snooze time is stored in 1 byte
#define MATED_VALUE 222 // random value chosen to represent mated status

// define meanings of certain EEPROM addresses:

// Addr of boolean- Has PNL been sent the activation message yet? This starts at false (0) and will only be set once (1 byte)
#define EEPROM_MATED_FLAG_ADDR 896

// Number of minutes PNL will sleep before waking up to send a text (1 byte)
#define EEPROM_NEXT_ALARM_ADDR 897

// Addr of boolean- should each device send a text when it wakes up? (4 bytes currently)
// each phone number has its own flag, hence 4 flags in a row right now
#define EEPROM_TEXTS_FLAG_ADDR 901

// Addr of boolean- should the device post to the server when it wakes up? (1 byte)
#define EEPROM_SERVER_FLAG_ADDR 905

// The 8-character magic string is uniquely generated for each PNL device.  It is only useful if, e.g.,
// the phone used to control PNL is lost/stolen and the owner can no longer communicate with PNL.  The owner
// can get a new phone and even if their phone number is different, can send the magic string to PNL.  PNL
// will verify the magic string and wipe all trusted numbers currently on file, adding the new number as the
// only one it trusts
#define EEPROM_MAGIC_STRING_ADDR 908

// The magic string is 8 characters.  This gives many options for the magic string so that it cannot be reasonably guessed by an attacker.
#define EEPROM_MAGIC_STRING_LEN 8

// Address of number of phone numbers currently trusted (1 byte)
#define EEPROM_NUM_PHONE_NUMS_ADDR 916

// Address of phone numbers PNL will accept commands from.  Starting at this address, there are 4 10-byte
// numbers (3 for area code + 7 for rest of phone number) stored sequentially that represent the up-to-4
// accepted phone numbers.  If there are less than 4, those slots will be zeroed out (i.e., 10 zeros in
// a row) (40 bytes)
#define EEPROM_PHONE_NUMS_ADDR 917

// length of device name in bytes
#define EEPROM_DEVICE_NAME_LEN 957

// Address of phone's nickname given by user (maximum of MAX_DEVICE_NAME_LEN bytes)
#define EEPROM_DEVICE_NAME 958

// max size of device name in bytes
#define MAX_DEVICE_NAME_LEN MAX_TXT_STR_SIZE

// Address of the first log entry.  Storing logs on PNL works by using a majority of space on the EEPROM
// to store a 5-byte sequence (4-byte timestamp plus 1-byte error value) that corresponds to the errors enum.
// When the alloted space is filled, the oldest log is deleted first.
#define EEPROM_FIRST_LOG_ADDR 0

// when the fona is turned on for the first time, the magic address will probably not contain the magic value.
// PoachNet does some initialization of the EEPROM bytes then writes the magic value to the magic address to
// signal initialization has been completed.  This way the initialization is only done once.  Once the device
// is mated, there is no need for this anymore, hence MAGIC_ADDR may eventually be overwritten by logs, which
// is fine.
#define MAGIC_ADDR 890
#define MAGIC_VALUE 111

// number of MS the Feather will sleep in between SMS messages
// Note: add 90000 MS to whatever time you actually want to sleep
// Seems to be a bug in the library?
uint32_t sleepTimeMS = 390000;

// number of MS without a cell connection until the Feather gives up sending an SMS
// NOTE: if you change this to a number > 65K, remember to change data type to uint32_t
uint16_t gpsTimeoutMS = 60000;

bool debugFlag = false, matedFlag = false, sendToServerFlag = true;

uint8_t sendSMSFlags[MAX_PHONE_NUMS];

// MAX_PHONE_NUMS 10-digit phone numbers (+ null term) from which PNL will accept commands.
// phoneNumbers[0] is the phone PNL was activated with and is the one to which messages (e.g., GPS coordinates) are sent.
char phoneNumbers[MAX_PHONE_NUMS][PHONE_NUM_LEN + 1];
uint8_t numPhoneNumbers;

// Bluemix website prefix to which data will be posted
const char *url = "poachnetcs.mybluemix.net/gps/";

// Sending this string to PoachNet for the first time activates/mates with that phone number
// i.e., PNL stores the phone number in EEPROM as a phone number it will accept commands from
// (note: should be lowercase because PNL will convert user message to lowercase before comparing)
const char *activateMsg = "activate";

// the magic string (password) that, when texted to the device, will remove all trusted phone numbers and
// add only the number from which this password was texted.
char passwd[EEPROM_MAGIC_STRING_LEN + 1];

Adafruit_FONA fona = Adafruit_FONA(FONA_RST);
SoftwareSerial fonaSS = SoftwareSerial(FONA_TX, FONA_RX);
SoftwareSerial *fonaSerial = &fonaSS;
Adafruit_GPS GPS(&Serial1);

// used to determine how long to wait for GPS to get a fix on location
uint32_t timer;

const char *onStr = "on";
const char *offStr = "off";
const char *cellStr = "cell";
const char *gpsStr = "GPS";
const char *googleMapsURL = "https://maps.google.com/maps?q=";
const char *knotsStr = " knots";
const char *activatedResp = "Activated. Recovery code: ";
const char *noLocAvailable = "No location data";
const char *speedStr = "\nspeed: ";
const char *okAddDelPhone = "OK. Current phones\n";
const char *okSnooze = "OK. Snooze time ";
const char *minsStr = " mins";
const char *okTextingStr = "OK. Texting ";
const char *removeStr = "remove";
const char *deleteStr = "delete";
const char *errorStr = "error";
const char *welcomeToPoachNet = "Welcome to PoachNet";
const char *addStr = "add";
const char *fromCell = "From cell:\n";
const char *fromGPS = "From GPS:\n";
const char *nameChangeOK = "OK";
const char *nameStr = "Name: ";
const char *resetMsg = "Device was reset";

char fonaIMEI[20];
char fonaName[MAX_TXT_STR_SIZE + 1];
uint8_t fonaNameLen;

jmp_buf soft_reset;

// use this function to help detect stack smashing into other memory sections
extern unsigned int __bss_end;
extern unsigned int __heap_start;
extern void *__brkval;
uint16_t checkFreeSRAM() {
  uint8_t newVariable;
  // heap is empty, use bss as start memory address
  if ((uint16_t)__brkval == 0) {
    return (((uint16_t)&newVariable) - ((uint16_t)&__bss_end));
    // use heap end as the start of the memory address
  } else {
    return (((uint16_t)&newVariable) - ((uint16_t)__brkval));
  }
};

void start_GPS()
{
  digitalWrite(GPS_GPIO, LOW); // enable GPS
  GPS.begin(9600);
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_1HZ); // 1 Hz update rate
  GPS.sendCommand(PGCMD_ANTENNA);
  GPS.fix = false;
  delay(1000);
  timer = millis();
}

void start_fona()
{
  digitalWrite(FONA_GPIO, LOW); // enable fona cell
  fonaSerial->begin(4800);
  fona.begin(*fonaSerial);
}

void write_little_endian(int address, uint16_t value)
{
  EEPROM.write(address, value % 255);
  EEPROM.write(address + 1, value / 255);
}

void generate_new_password()
{
  // use time as random seed for magic reset string
  randomSeed(millis());
  for (uint8_t i = 0; i < EEPROM_MAGIC_STRING_LEN; i++) {
    passwd[i] = random('0', ']');
    EEPROM.write(EEPROM_MAGIC_STRING_ADDR + i, passwd[i]);
  }
  // null-term password
  passwd[EEPROM_MAGIC_STRING_LEN] = 0;
}

// used to reset variables to their original values. e.g., after PNL is activated
// and the oyster farmer puts PNL back underwater, any modifications they made
// such as "snooze 1000" will be reset to "snooze 5" for the next time it activates.
// Does not erase any recognized phone numbers.
// index is the 0-indexed phone number in the list of recognized numbers that sent
// the reset command.  If index is -1, all phone numbers will have their texts flag reset
void reset_vars(int8_t index)
{
  // initialize time until next alarm/wake-up to 5 minutes
  EEPROM.write(EEPROM_NEXT_ALARM_ADDR, DEFAULT_SNOOZE_MINS);

  // device should text by default. Reset flag to true for current device
  // or all devices if index is -1
  for (uint8_t i = 0; i < numPhoneNumbers; i++) {
    if (index == -1 || index == i)
      set_text_flag(i, true);
  }

  // device should send info to server by default. Set flag to true
  EEPROM.write(EEPROM_SERVER_FLAG_ADDR, 1);

  // make sure to change variables in the program, too
  sleepTimeMS = 90000 + DEFAULT_SNOOZE_MINS * 60000; // 5 minutes currently
  sendToServerFlag = true;
}

// updates fonaName and fonaNameLen variables with values from EEPROM
void get_device_name()
{
  fonaNameLen = EEPROM.read(EEPROM_DEVICE_NAME_LEN);
  for (uint8_t i = 0; i < fonaNameLen; i++) {
    fonaName[i] = EEPROM.read(EEPROM_DEVICE_NAME + i);
  }
  fonaName[fonaNameLen] = 0;
}

// updates the user-given name of the device
void set_device_name(char *newName)
{
  strncpy(fonaName, newName, MAX_DEVICE_NAME_LEN);
  fonaName[MAX_DEVICE_NAME_LEN] = 0; // make sure name is null-terminated
  fonaNameLen = strlen(fonaName);
  EEPROM.write(EEPROM_DEVICE_NAME_LEN, fonaNameLen);
  for (uint8_t i = 0; i < fonaNameLen; i++) {
    EEPROM.write(EEPROM_DEVICE_NAME + i, fonaName[i]);
  }
}

// updates matedFlag with value from EEPROM
void get_mated_flag()
{
  matedFlag = EEPROM.read(EEPROM_MATED_FLAG_ADDR) == MATED_VALUE ? true : false;
}

void set_num_phones(uint8_t numPhones)
{
  numPhoneNumbers = numPhones;
  EEPROM.write(EEPROM_NUM_PHONE_NUMS_ADDR, numPhoneNumbers);
}

void get_num_phones()
{
  numPhoneNumbers = EEPROM.read(EEPROM_NUM_PHONE_NUMS_ADDR);
}

void get_server_flag()
{
  sendToServerFlag = EEPROM.read(EEPROM_SERVER_FLAG_ADDR);
}

void get_sleep_time()
{
  sleepTimeMS = 90000 + EEPROM.read(EEPROM_NEXT_ALARM_ADDR) * 60000;
}

void get_recognized_phones()
{
  for (uint8_t i = 0; i < MAX_PHONE_NUMS; i++) {
    for (uint8_t j = 0; j < PHONE_NUM_LEN; j++)
      phoneNumbers[i][j] = EEPROM.read(EEPROM_PHONE_NUMS_ADDR + i * PHONE_NUM_LEN + j);

    // null-terminate each phone number
    phoneNumbers[i][PHONE_NUM_LEN] = 0;
  }
}

void get_texting_flags()
{
  for (uint8_t i = 0; i < MAX_PHONE_NUMS; i++)
    sendSMSFlags[i] = EEPROM.read(EEPROM_TEXTS_FLAG_ADDR + i);
}

void get_password()
{
  for (uint8_t i = 0; i < EEPROM_MAGIC_STRING_LEN; i++)
    passwd[i] = EEPROM.read(EEPROM_MAGIC_STRING_ADDR + i);
  passwd[EEPROM_MAGIC_STRING_LEN] = 0;
}

// reads values from EEPROM to set up program variables
void init_vars()
{
  // completely unmates and wipes vars on the fona
  /*EEPROM.write(EEPROM_NUM_PHONE_NUMS_ADDR, 0);
    EEPROM.write(MAGIC_ADDR, 0);
    EEPROM.write(EEPROM_MATED_FLAG_ADDR, 0);
    reset_logs();*/

  fona.getIMEI(fonaIMEI);
  get_mated_flag();

  if (!matedFlag) {
    // this stuff runs only once ever, the first time the device is turned on.
    // makes sure certain EEPROM addresses are initialized properly out of the factory
    if (EEPROM.read(MAGIC_ADDR) != MAGIC_VALUE) {
      set_num_phones(0);

      // don't need to generate new password until activation message appears

      // reset_vars takes care of resetting some more things
      reset_vars(-1); // -1 means clear modified info about all devices

      // initially set name as IMEI number
      set_device_name(fonaIMEI);

      // write the magic value to magic address.  This means the magic string
      // will not be regenerated on another startup before PNL is mated
      // the magic value in the magic address can be overwritten later
      // (it doesn't matter once PNL is mated)
      EEPROM.write(MAGIC_ADDR, MAGIC_VALUE);
    }
  } else {
    get_recognized_phones();
    get_texting_flags();
    get_password();
  }

  get_device_name();
  get_num_phones();
  get_server_flag();
  get_sleep_time();
}

void to_lower_case(char *s)
{
  while (*s) {
    if (*s >= 'A' && *s <= 'Z')
      *s = *s - 'A' + 'a';
    s++;
  }
}

// tries to find phoneNumber in the current list of recognized phone numbers
// if found, returns its index: [0 through MAX_PHONE_NUMS - 1]
// else returns -1
int8_t find_phone_number(char *phoneNumber)
{
  phoneNumber[PHONE_NUM_LEN] = 0;
  for (int i = 0; i < numPhoneNumbers; i++) {
    if (!strcmp(phoneNumber, phoneNumbers[i]))
      return i;
  }
  return -1;
}

// appends all currently recognized phone numbers to end of string s
void append_remaining_phone_nums(char *s)
{
  for (uint8_t i = 0;;) {
    strcat(s, phoneNumbers[i]);
    if (++i >= numPhoneNumbers)
      return;
    strcat(s, "\n");
  }
}

// sets phone # index to the given phone number
void set_phone_number(uint8_t index, char *phoneNum)
{
  for (uint8_t i = 0; i < PHONE_NUM_LEN; i++) {
    phoneNumbers[index][i] = phoneNum[i];
    EEPROM.write(EEPROM_PHONE_NUMS_ADDR + index * PHONE_NUM_LEN + i, phoneNumbers[index][i]);
  }
  phoneNumbers[index][PHONE_NUM_LEN] = 0; // null-terminate

  // by default enable texting to this device
  set_text_flag(index, true);
}

void set_mated_flag(bool val)
{
  matedFlag = val;
  EEPROM.write(EEPROM_MATED_FLAG_ADDR, val ? MATED_VALUE : 0);
}

// used for both activate and reactivate requests
void handle_activate(char *response, char *smsSender)
{
  // make current sender the only recognized phone number
  set_num_phones(1);

  // save phone number as #0
  set_phone_number(0, smsSender);

  // by default, enable texting for this device (first and only device)
  set_text_flag(0, true);

  generate_new_password();
  strcpy(response, activatedResp);
  strcat(response, passwd);

  // device is now mated. do this before sendSMS in case something weird happens and sendSMS crashes
  set_mated_flag(true);

  // note: sendSMS must be after set_mated_flag(true), as matedFlag is checked in sendSMS
  send_SMS(response, smsSender);
}

void handle_snooze(char *response, char args[MAX_ARGS][MAX_TXT_STR_SIZE], int8_t senderIndex, char *smsSender)
{
  uint8_t snoozeMins = DEFAULT_SNOOZE_MINS;

  // If user specifies a snooze interval, use it. Otherwise use DEFAULT_SNOOZE_MINS
  if (args[1]) {
    for (uint8_t i = 0; i < strlen(args[1]); i++) {
      if (!isdigit(args[1][i])) {
        return;
      }
    }
    snoozeMins = min((uint8_t)atoi(args[1]), MAX_SNOOZE);
  }

  EEPROM.write(EEPROM_NEXT_ALARM_ADDR, snoozeMins);
  sleepTimeMS = 90000 + snoozeMins * 60000;

  strcpy(response, okSnooze);
  // hardcoded 5: needs to be changed if/when default time is ever changed
  strcat(response, args[1] ? args[1] : "5");
  strcat(response, minsStr);

  // in case user has texting off, send them a message anyway (sendSMS doesn't check texting flag before sending)
  if (!sendSMSFlags[senderIndex])
    send_SMS(response, smsSender);

  // then send it to everyone so they know what's going on
  broadcast_SMS(response);
}

void set_text_flag(uint8_t senderIndex, bool flag)
{
  if (senderIndex >= 0 && senderIndex < MAX_PHONE_NUMS) {
    sendSMSFlags[senderIndex] = flag;
    EEPROM.write(EEPROM_TEXTS_FLAG_ADDR + senderIndex, flag);
  }
}

void handle_toggle_text(char *response, int8_t senderIndex, char args[MAX_ARGS][MAX_TXT_STR_SIZE], char *smsSender)
{
  if (!strcmp(args[1], offStr)) {
    set_text_flag(senderIndex, false);
    strcpy(response, okTextingStr);
    strcat(response, offStr);
  } else if (!strcmp(args[1], onStr)) {
    set_text_flag(senderIndex, true);
    strcpy(response, okTextingStr);
    strcat(response, onStr);
  } else {
    strcpy(response, errorStr);
  }
  send_SMS(response, smsSender);
}

void handle_change_name(char *newName, char *smsSender)
{
  set_device_name(newName);
  send_SMS(nameChangeOK, smsSender);
}

bool add_phone_number(char *phoneNum)
{
  // if max phones reached or phone already exists
  if (numPhoneNumbers == MAX_PHONE_NUMS || find_phone_number(phoneNum) >= 0) {
    return false;
  }

  // set_phone_number sets texting flag to true, also
  set_phone_number(numPhoneNumbers, phoneNum);
  set_num_phones(numPhoneNumbers + 1);
  return true;
}

void handle_add_del_phone(char *response, char args[MAX_ARGS][MAX_TXT_STR_SIZE], char *smsSender)
{
  // args[1] is add or remove/delete, args[2] is the phone number in question
  int8_t index = find_phone_number(args[2]);
  if (!strcmp(args[1], addStr)) {
    // try to add the phone
    if (!add_phone_number(args[2])) {
      strcpy(response, errorStr);
      send_SMS(response, smsSender);
      return;
    }

    // send a greeting to the new phone
    send_SMS(welcomeToPoachNet, phoneNumbers[numPhoneNumbers - 1]);
  } else if (!strcmp(args[1], removeStr) || !strcmp(args[1], deleteStr)) {
    if (index < 0 || numPhoneNumbers == 1) {
      strcpy(response, errorStr);
      send_SMS(response, smsSender);
      return;
    }

    for (uint8_t i = index; i < MAX_PHONE_NUMS - 1; i++) {
      for (uint8_t j = 0; j < PHONE_NUM_LEN; j++) {
        phoneNumbers[i][j] = phoneNumbers[i + 1][j];
        EEPROM.write(EEPROM_PHONE_NUMS_ADDR + i * PHONE_NUM_LEN + j, EEPROM.read(EEPROM_PHONE_NUMS_ADDR + (i + 1) * PHONE_NUM_LEN + j));
      }

      set_text_flag(i, EEPROM.read(EEPROM_TEXTS_FLAG_ADDR + i + 1));
    }
    numPhoneNumbers--;
  }

  // finish creating response message and send it
  strcpy(response, okAddDelPhone);
  append_remaining_phone_nums(response);
  send_SMS(response, smsSender);

  set_num_phones(numPhoneNumbers);
}

void tokenize_sms_by(char *smsContents, const char *delim, char tokens[MAX_ARGS][MAX_TXT_STR_SIZE])
{
  char *tok = strtok(smsContents, delim);
  for (uint8_t i = 0; i < MAX_ARGS; i++) {
    strncpy(tokens[i], tok, sizeof tokens[i]);
    to_lower_case(tokens[i]);
    tok = strtok(NULL, delim);
    if (!tok)
      break;
  }
}

void delete_SMS(uint16_t smsn)
{
  fona.deleteSMS(smsn);
}

// checks for command messages from trusted phone numbers
// partially copied from FONAtest file that comes with Arduino IDE
void check_messages()
{
  char smsContents[100], smsSenderBuf[PHONE_NUM_LEN + 3];
  int8_t smsnum = -1, smsn = 1;
  uint16_t smslen;

  // getNumSMS() returns -1 if the command to get # of texts fails
  // keep looping until it succeeds.  Sometimes when the phone first
  // starts, it takes a couple seconds for communication to be established.
  for (uint8_t i = 0; smsnum == -1; i++) {
    // if we can't get number of SMS after GET_MAX_NUM_SMS_TRIES tries, don't check messages
    if (i >= MAX_GET_NUM_SMS_TRIES)
      return;
    delay(1000);
    smsnum = fona.getNumSMS();
  }

  for (; smsn <= smsnum; smsn++) {
    int8_t res = 0;
    uint8_t i;
    for (i = 0; !res; i++) {
      if (i >= MAX_GET_NUM_SMS_TRIES) {
        break;
      }
      delay(1000);
      res = fona.readSMS(smsn, smsContents, sizeof smsContents - 5, &smslen);
    }
    if (i >= MAX_GET_NUM_SMS_TRIES)
      continue;

    // if the length is zero, its a special case where the index number is higher
    // so increase the max we'll look at
    if (!smslen) {
      smsnum++;
      continue;
    }

    // Next retrieve SMS sender address/phone number. Try a certain number of times before giving up
    i = 0;
    for (; i < MAX_GET_NUM_SMS_TRIES; i++) {
      if (fona.getSMSSender(smsn, smsSenderBuf, PHONE_NUM_LEN + 2))
        break;
      delay(1000);
    }

    // sometimes text messages show up that don't have an smsSender
    // or sometimes the fona can't retrieve a text message sender- just delete msg and continue
    if (i == MAX_GET_NUM_SMS_TRIES) {
      delete_SMS(smsn);
      continue;
    }

    // phone number will be "+1XXXXXXXXXX". We remove the +CountryCode part of the number by incrementing by 2
    // then add null terminator
    char *smsSender = &smsSenderBuf[2];
    smsSender[PHONE_NUM_LEN] = 0;

    if (!strcmp(smsContents, passwd)) {
      handle_activate(smsContents, smsSender);
      delete_SMS(smsn);
      continue;
    }

    // get the index of this phone number in our recognized numbers.
    // if it is not recognized, senderIndex will be -1
    int8_t senderIndex = find_phone_number(smsSender);

    // password was case-sensitive, but all other commands should be case insensitive
    to_lower_case(smsContents);

    // only activate if not already activated
    if (!strcmp(smsContents, activateMsg)) {
      if (!matedFlag) {
        handle_activate(smsContents, smsSender);
        delete_SMS(smsn);
      }
      continue;
    }

    // only let an unrecognized phone number send an activate or reactivate command
    if (senderIndex == -1) {
      delete_SMS(smsn);
      continue;
    }

    // tokenize SMS by space character
    char args[MAX_ARGS][MAX_TXT_STR_SIZE];
    tokenize_sms_by(smsContents, " ", args);

    if (!strcmp(args[0], SNOOZE_CMD))
      handle_snooze(smsContents, args, senderIndex, smsSender);
    else if (!strcmp(args[0], RESET_CMD))
      reset_vars(senderIndex);
    else if (args[1]) {
      if (!strcmp(args[0], TOGGLE_TEXT_CMD))
        handle_toggle_text(smsContents, senderIndex, args, smsSender);
      else if (!strcmp(args[0], NAME_CMD))
        handle_change_name(args[1], smsSender);
      else if (args[2] && strlen(args[2]) >= 10 && !strcmp(args[0], ADD_DEL_PHONE_CMD))
        handle_add_del_phone(smsContents, args, smsSender);

    }

    // delete text message from FONA since it's been handled
    delete_SMS(smsn);
  }
}

void wipe_eeprom()
{
  for (uint16_t i = 0; i < EEPROM_SIZE; i++)
    EEPROM.write(i, 0);
}

void check_reset_button()
{
  digitalWrite(RESET_BUTTON_SEND_GPIO, HIGH);
  delay(50);
  if (digitalRead(RESET_BUTTON_GPIO)) {
    wipe_eeprom();
    digitalWrite(RESET_LED, HIGH);
    delay(5000);
    digitalWrite(RESET_LED, LOW);

    // instead of calling setup(), use longjmp to prevent overflowing the stack
    // when setup() executes, all local vars will be updated to reflect eeprom being wiped
    // and eeprom will be reinitialized.. 2nd arg value doesn't matter at this point
    longjmp(soft_reset, 0);
  }
  digitalWrite(RESET_BUTTON_SEND_GPIO, LOW);
}

void setup()
{
  pinMode(BATT_LED, OUTPUT);
  pinMode(RESET_LED, OUTPUT);

  digitalWrite(BATT_LED, LOW);
  digitalWrite(RESET_LED, LOW);

  // stuff to do during debugging/development
  if (debugFlag) {
    // start serial connection for debugging
    Serial.begin(115200);

    // wait for serial to connect before executing
    while (!Serial);
  }

  setjmp(soft_reset);

  pinMode(RESET_BUTTON_GPIO, INPUT);
  pinMode(RESET_BUTTON_SEND_GPIO, OUTPUT);
  digitalWrite(RESET_BUTTON_SEND_GPIO, LOW);

  // poll reset button constantly => this means reset doesn't work when FONA is doing something besides this loop,
  // such as sending/receiving SMS, initializing itself, etc.
  check_reset_button();

  // GPIO pin 11 controls power to Feather's cell module
  pinMode(FONA_GPIO, OUTPUT);

  // GPIO pin 12 controls power to GPS module
  pinMode(GPS_GPIO, OUTPUT);

  start_fona();

  uint16_t vbat;
  if (fona.getBattPercent(&vbat)) {
    while (vbat >= 10) {
      digitalWrite(BATT_LED, HIGH);
      delay(250);
      digitalWrite(BATT_LED, LOW);
      delay(250);
      vbat -= 10;
    }
  }

  init_vars();
  start_GPS();
}

void go_to_sleep()
{
  // disable fona cell before sleeping by toggling
  digitalWrite(FONA_GPIO, HIGH);
  delay(500);
  digitalWrite(FONA_GPIO, LOW);
  delay(1500);
  digitalWrite(FONA_GPIO, HIGH);

  digitalWrite(GPS_GPIO, HIGH); // disable GPS before sleeping

  // low-power sleeps for as long as possible on each call to Watchdog.sleep() (probably around 8 seconds)
  // when it reaches the desired sleep time, it breaks out of the loop
  uint32_t sleepMS = 0;
  while (sleepMS < sleepTimeMS)
    sleepMS += Watchdog.sleep();

  start_fona();
  start_GPS();
}

// sends a text to every phone registered with this device that has opted to receive texts
void broadcast_SMS(const char *msg)
{
  for (uint8_t i = 0; i < numPhoneNumbers; i++) {
    if (sendSMSFlags[i])
      send_SMS(msg, phoneNumbers[i]);
  }
}

// sends a text with message msg to the given number
void send_SMS(char *msg, char phoneNumber[PHONE_NUM_LEN + 1])
{
  fona.sendSMS(phoneNumber, msg);
}

// msg is the message sent to via text. parse lat and lon out of it
// coordinatesFrom is the name of the device the coordinates came from. Either "GPS" or "cell"
void post_to_url(char *msg, const char* coordinatesFrom)
{
  char *lat, *lon;
  while (*msg != '=') {
    msg++;
  }
  msg++;
  lon = msg;
  while (*msg != ',')
    msg++;
  *msg = 0;
  msg++;
  lat = msg;
  while (*msg && *msg != '\n')
    msg++;
  *msg = 0;

  uint16_t statuscode;
  int16_t len;

  // format the data as "lat,lon"
  char urlWithCoords[strlen(url) + 2 * GPS_DATA_SIZE + strlen(fonaIMEI) + strlen(coordinatesFrom) + strlen(fonaName) + 5];
  strcpy(urlWithCoords, url);
  strcat(urlWithCoords, lat);
  strcat(urlWithCoords, ",");
  strcat(urlWithCoords, lon);
  strcat(urlWithCoords, ",");
  strcat(urlWithCoords, fonaIMEI);
  strcat(urlWithCoords, ",");
  strcat(urlWithCoords, coordinatesFrom);
  strcat(urlWithCoords, ",");
  strcat(urlWithCoords, fonaName);

  if (sendToServerFlag) {
    fona.HTTP_POST_start(urlWithCoords, F("text/plain"), (uint8_t *)" ", 1, &statuscode, (uint16_t *)&len);
  }
}

void loop()
{
  if (GPS.fix) {
    check_messages();

    char msg[strlen(nameStr) + strlen(fonaName) + strlen(fromGPS) + strlen(googleMapsURL) + 3 * GPS_DATA_SIZE + strlen(speedStr) + strlen(knotsStr) + 3];
    char gpsData[GPS_DATA_SIZE];
    strcpy(msg, nameStr);
    strcat(msg, fonaName);
    strcat(msg, "\n");
    strcat(msg, fromGPS);
    strcat(msg, googleMapsURL);

    // append latitude to msg
    String dataItem = String(GPS.latitudeDegrees, 4);
    dataItem.toCharArray(gpsData, sizeof gpsData);
    strcat(msg, gpsData);
    strcat(msg, ",");

    // append longitude to msg
    dataItem = String(GPS.longitudeDegrees, 4);
    dataItem.toCharArray(gpsData, sizeof gpsData);
    strcat(msg, gpsData);

    // append speed to msg
    strcat(msg, speedStr);
    dataItem = String(GPS.speed);
    dataItem.toCharArray(gpsData, sizeof gpsData);
    strcat(msg, gpsData);
    strcat(msg, knotsStr);

    broadcast_SMS(msg);

    // post the data
    if (fona.enableGPRS(true)) {
      // post_to_url clobbers msg so do it after broadcast_sms
      post_to_url(msg, gpsStr);
    }
    go_to_sleep();
  } else if (millis() - timer >= gpsTimeoutMS) {
    check_messages();

    char msg[strlen(nameStr) + strlen(fonaName) + strlen(fromCell) + strlen(googleMapsURL) + 2 * GPS_DATA_SIZE + 3];
    uint16_t returncode;
    if (fona.enableGPRS(true) && fona.getGSMLoc(&returncode, msg, sizeof msg - 5) && returncode == 0) {
      char lon[GPS_DATA_SIZE], lat[GPS_DATA_SIZE];
      char *tok = strtok(msg, ",");
      strcpy(lon, tok);
      tok = strtok(NULL, ",");
      strcpy(lat, tok);

      strcpy(msg, nameStr);
      strcat(msg, fonaName);
      strcat(msg, "\n");
      strcat(msg, fromCell);
      strcat(msg, googleMapsURL);
      strcat(msg, lat);
      strcat(msg, ",");
      strcat(msg, lon);

      broadcast_SMS(msg);

      // post_to_url clobbers msg, so do it after broadcast_sms
      post_to_url(msg, cellStr);
      fona.enableGPRS(false);
    } else {
      strcpy(msg, nameStr);
      strcat(msg, fonaName);
      strcat(msg, "\n");
      strcpy(msg, noLocAvailable);
      broadcast_SMS(msg);
    }

    go_to_sleep();
  }

  GPS.read();
  if (GPS.newNMEAreceived()) {
    GPS.parse(GPS.lastNMEA());
  }
}

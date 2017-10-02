// software for PoachNet Lite

#include "Adafruit_FONA.h"
#include <Adafruit_GPS.h>
#include <Adafruit_SleepyDog.h>
#include <EEPROM.h>

#define FONA_RST 4
#define FONA_RI  7
#define FONA_TX  8
#define FONA_RX  9
#define GPSECHO true

// enum of errors that will be logged during operation
enum errors {
  NO_GPS_GPRS_AVAIL,
  FONA_DISABLE_GPRS,
  FONA_ENABLE_GPRS,
  SEND_SMS,
  HTTP_POST,
  SNOOZE_NOT_DIGIT,
  DELETE_SMS,
  READ_SMS,
  GET_BATT_PERCENT,
  NOT_VALID_PHONE_NUM
};

#define MAX_ARGS 5
#define MAX_PHONE_NUMS 4
#define PHONE_NUM_LEN 10
#define MAX_GET_NUM_SMS_TRIES 5
#define MAX_TXT_STR_SIZE 15
#define GPS_DATA_SIZE 15

#define LOG_ENTRY_SIZE 5 // size of a log entry in number of bytes
#define MAX_LOGS (EEPROM_MATED_FLAG_ADDR - (EEPROM_MATED_FLAG_ADDR % LOG_ENTRY_SIZE))

// GPIO pins that control power to GPS and FONA cell modules
#define FONA_GPIO 11
#define GPS_GPIO 12

// texting commands
#define SNOOZE_CMD "snooze"
#define GET_LOGS_CMD "logs"
#define TOGGLE_TEXT_CMD "texting"
#define STATUS_CMD "status"
#define RESET_CMD "reset"
#define ADD_DEL_PHONE_CMD "phone"

#define DEFAULT_SNOOZE_MINS 5 // if no arg is specified in a SNOOZE command; a big number for default (e.g., 60 mins) is bad since that would immobilize the device until the time expires
#define DEFAULT_ALARM_MINS 5
#define MAX_SNOOZE 1440 // max snooze currently 24 hours
#define MATED_VALUE 222

// define meanings of certain EEPROM addresses

// Addr of boolean- Has PNL been sent the activation message yet? This starts at false (0) and will only be set once (1 byte)
#define EEPROM_MATED_FLAG_ADDR 896

// Number of minutes until PNL should wake up again and send a text message (2 bytes, little endian)
#define EEPROM_NEXT_ALARM_ADDR 897

// Number of times PNL has woken up (i.e., to send a text/post to Bluemix) since the last reset (2 bytes, little endian)
#define EEPROM_NUM_WAKES_ADDR 899

// Addr of boolean- should each device send a text when it wakes up? (4 bytes currently)
// each phone number has its own flag, hence 4 flags in a row right now
#define EEPROM_TEXTS_FLAG_ADDR 901

// Addr of boolean- should the device post to the server when it wakes up? (1 byte)
#define EEPROM_SERVER_FLAG_ADDR 905

// Number of logs (i.e., 1-byte error values + 4-byte timestamp) currently in PNL.  The logs can be cleared via SMS command,
// in which case EEPROM_NUM_LOGS is set to 0 (2 bytes, little endian)
#define EEPROM_NUM_LOGS_ADDR 906

// The 8-character magic string is uniquely generated for each PNL device.  It is only useful if, e.g.,
// the phone used to control PNL is lost/stolen and the owner can no longer communicate with PNL.  The owner
// can get a new phone and even if their phone number is different, can send the magic string to PNL.  PNL
// will verify the magic string and wipe all trusted numbers currently on file, adding the new number as the
// only one it trusts
#define EEPROM_MAGIC_STRING_ADDR 908

// The magic string is 8 characters.  This gives so many options for the magic string that
// it could not be guessed by an attacker.
#define EEPROM_MAGIC_STRING_LEN 8

// Address of number of phone numbers currently trusted (1 byte)
#define EEPROM_NUM_PHONE_NUMS_ADDR 916

// Address of phone numbers PNL will accept commands from.  Starting at this address, there are 4 10-byte
// numbers (3 for area code + 7 for rest of phone number) stored sequentially that represent the up-to-4
// accepted phone numbers.  If there are less than 4, those slots will be zeroed out (i.e., 10 zeros in
// a row) (40 bytes)
#define EEPROM_PHONE_NUMS_ADDR 917

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
uint32_t gpsTimeoutMS = 60000;

uint8_t debugFlag = 0, matedFlag = 0, sendToServerFlag = 1;

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
const char *activateMsg = "activate poachnet";

// the magic string (password) that, when texted to the device, will remove all trusted phone numbers and
// add only the number from which this password was texted.
char passwd[EEPROM_MAGIC_STRING_LEN + 1];

Adafruit_FONA fona = Adafruit_FONA(FONA_RST);
SoftwareSerial fonaSS = SoftwareSerial(FONA_TX, FONA_RX);
SoftwareSerial *fonaSerial = &fonaSS;
Adafruit_GPS GPS(&Serial1);

uint32_t timer;

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
  for (int i = 0; i < EEPROM_MAGIC_STRING_LEN; i++) {
    passwd[i] = random('0', ']');
    EEPROM.write(EEPROM_MAGIC_STRING_ADDR + i, passwd[i]);
  }
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
  write_little_endian(EEPROM_NEXT_ALARM_ADDR, DEFAULT_ALARM_MINS);

  // device should text by default. Reset flag to true for current device
  // or all devices if index is -1
  for (uint8_t i = 0; i < numPhoneNumbers; i++) {
    if (index == -1 || index == i)
      set_text_flag(i, true);
  }

  // device should send info to server by default. Set flag to true
  EEPROM.write(EEPROM_SERVER_FLAG_ADDR, 1);

  // reset num of wakeups so far to 0
  EEPROM.write(EEPROM_NUM_WAKES_ADDR, 0);

  // make sure to change variables in the program, too
  sleepTimeMS = 90000 + DEFAULT_ALARM_MINS * 60000; // 5 minutes currently
  sendToServerFlag = 1;
}

// converts char to number, assuming c is '0' to '9'
uint8_t val(char c)
{
  return c - '0';
}

// adds a log with the error code and time stamp from fona cell connection
// if logs are full, it just wraps around like a circular buffer and overwrites oldest logs first
void log_error(enum errors error)
{
  // format is "YY/MM/DD,hh:mm:ss+XX"
  // i.e., "year/month/day,hour:min:sec+DontKnow"
  if (debugFlag) {
    Serial.println("error: ");
    Serial.println(error);
  }

  char buf[25];
  fona.getTime(buf, sizeof buf);

  uint16_t numLogs = EEPROM.read(EEPROM_NUM_LOGS_ADDR) + EEPROM.read(EEPROM_NUM_LOGS_ADDR + 1) * 255;

  uint16_t currLogAddr = (numLogs * LOG_ENTRY_SIZE) % MAX_LOGS;
  EEPROM.write(currLogAddr, val(buf[3]) * 10 + val(buf[4])); // month
  EEPROM.write(currLogAddr + 1, val(buf[6]) * 10 + val(buf[7])); // day
  EEPROM.write(currLogAddr + 2, val(buf[9]) * 10 + val(buf[10])); // hour
  EEPROM.write(currLogAddr + 3, val(buf[12]) * 10 + val(buf[13])); // minute
  EEPROM.write(currLogAddr + 4, error);

  write_little_endian(EEPROM_NUM_LOGS_ADDR, numLogs + 1);
}

void reset_logs()
{
  // reset num of logs so far to 0
  EEPROM.write(EEPROM_NUM_LOGS_ADDR, 0);
  EEPROM.write(EEPROM_NUM_LOGS_ADDR + 1, 0);
}

// reads values from EEPROM to set up program variables
void init_vars()
{
  // completely unmates and wipes vars on the fona
  /*EEPROM.write(EEPROM_NUM_PHONE_NUMS_ADDR, 0);
  EEPROM.write(MAGIC_ADDR, 0);
  EEPROM.write(EEPROM_MATED_FLAG_ADDR, 0);
  reset_logs();*/

  matedFlag = EEPROM.read(EEPROM_MATED_FLAG_ADDR);

  if (matedFlag != MATED_VALUE) {
    // this stuff runs only once ever, the first time the device is turned on.
    // makes sure EEPROM values are initialized properly out of the factory
    if (EEPROM.read(MAGIC_ADDR) != MAGIC_VALUE) {
      EEPROM.write(EEPROM_NUM_PHONE_NUMS_ADDR, 0);

      for (int i = 0; i < MAX_PHONE_NUMS * PHONE_NUM_LEN; i++)
        EEPROM.write(EEPROM_PHONE_NUMS_ADDR + i, 0);

      generate_new_password();
      reset_vars(-1);

      // write the magic value to magic address.  This means the magic string
      // will not be regenerated on another startup before PNL is mated
      // the magic value in the magic address can be overwritten later
      // (it doesn't matter once PNL is mated)
      EEPROM.write(MAGIC_ADDR, MAGIC_VALUE);
    }
  } else {
    // otherwise read out the phone numbers stored in EEPROM
    for (int i = 0; i < MAX_PHONE_NUMS; i++) {
      for (int j = 0; j < PHONE_NUM_LEN; j++)
        phoneNumbers[i][j] = EEPROM.read(EEPROM_PHONE_NUMS_ADDR + i * PHONE_NUM_LEN + j);

      // null terminate each phone number string
      phoneNumbers[i][PHONE_NUM_LEN] = 0;
    }

    // then get the magic string/password from EEPROM
    for (int i = 0; i < EEPROM_MAGIC_STRING_LEN; i++)
      passwd[i] = EEPROM.read(EEPROM_MAGIC_STRING_ADDR + i);
  }

  numPhoneNumbers = EEPROM.read(EEPROM_NUM_PHONE_NUMS_ADDR);
  for (uint8_t i = 0; i < numPhoneNumbers; i++)
    sendSMSFlags[i] = EEPROM.read(EEPROM_TEXTS_FLAG_ADDR);
  sendToServerFlag = EEPROM.read(EEPROM_SERVER_FLAG_ADDR);
  sleepTimeMS = 90000 + (EEPROM.read(EEPROM_NEXT_ALARM_ADDR) + 255 * EEPROM.read(EEPROM_NEXT_ALARM_ADDR + 1)) * 60000;
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
uint8_t find_phone_number(char phoneNumber[])
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
  uint8_t i = 0;
  for (;;) {
    strcat(s, phoneNumbers[i]);
    if (++i >= numPhoneNumbers)
      return;
    strcat(s, "\n");
  }
}

void handle_reactivate(char *response, char *smsSender)
{
  // make current sender the only recognized phone number
  numPhoneNumbers = 1;
  EEPROM.write(EEPROM_NUM_PHONE_NUMS_ADDR, numPhoneNumbers);

  for (int i = 0; i < PHONE_NUM_LEN; i++) {
    phoneNumbers[0][i] = smsSender[i];
    EEPROM.write(EEPROM_PHONE_NUMS_ADDR + i, phoneNumbers[0][i]);
  }

  // make sure mated flag is set
  matedFlag = MATED_VALUE;
  EEPROM.write(EEPROM_MATED_FLAG_ADDR, matedFlag);

  generate_new_password();

  // reply to user with a new password
  strcpy(response, "PoachNet has reactivated to this device. Your new recovery code is: ");
  strcat(response, passwd);
  send_SMS(response, smsSender);
}

void handle_activate(char *response, char *smsSender)
{
  // only send message if the phone hasn't already been mated/activated
  if (matedFlag != MATED_VALUE) {
    const char *tmp = "Thank you for activating PoachNet. Your recovery code is: ";
    int len = strlen(tmp);
    strcpy(response, tmp);
    for (uint8_t i = 0; i < EEPROM_MAGIC_STRING_LEN; i++)
      response[i + len] = EEPROM.read(EEPROM_MAGIC_STRING_ADDR + i);
    response[EEPROM_MAGIC_STRING_LEN + len] = 0;

    for (uint8_t i = 0; i < PHONE_NUM_LEN; i++) {
      EEPROM.write(EEPROM_PHONE_NUMS_ADDR + i, smsSender[i]);
      phoneNumbers[numPhoneNumbers][i] = smsSender[i];
    }

    // by default, enable texting for this device
    EEPROM.write(EEPROM_TEXTS_FLAG_ADDR + numPhoneNumbers, 1);

    numPhoneNumbers++;
    EEPROM.write(EEPROM_NUM_PHONE_NUMS_ADDR, numPhoneNumbers);

    matedFlag = MATED_VALUE;
    EEPROM.write(EEPROM_MATED_FLAG_ADDR, matedFlag);

    // note: sendSMS must be after matedFlag = MATED_VALUE, as this is checked for in sendSMS
    send_SMS(response, smsSender);
  }
}

void handle_snooze(char *response, char args[MAX_ARGS][MAX_TXT_STR_SIZE], int8_t senderIndex, char *smsSender)
{
  uint16_t snoozeMins = DEFAULT_SNOOZE_MINS;

  // If user specifies a snooze interval, use it. Otherwise use DEFAULT_SNOOZE_MINS
  if (args[1]) {
    for (unsigned int i = 0; i < strlen(args[1]); i++) {
      if (!isdigit(args[1][i])) {
        log_error(SNOOZE_NOT_DIGIT);
        return;
      }
    }
    snoozeMins = min((uint16_t)atoi(args[1]), MAX_SNOOZE);
  }

  write_little_endian(EEPROM_NEXT_ALARM_ADDR, snoozeMins);
  sleepTimeMS = 90000 + snoozeMins * 60000;

  strcpy(response, "Snooze time is now ");
  strcat(response, args[1] ? args[1] : "5");
  strcat(response, " minutes");
  if (!sendSMSFlags[senderIndex])
    send_SMS(response, smsSender);
  broadcast_SMS(response);
}

void handle_get_logs(char *smsSender)
{
  uint16_t numLogs = EEPROM.read(EEPROM_NUM_LOGS_ADDR) + EEPROM.read(EEPROM_NUM_LOGS_ADDR + 1) * 255;
  if (numLogs > MAX_LOGS)
    numLogs = MAX_LOGS;

  char logs[numLogs * LOG_ENTRY_SIZE + 1];
  logs[numLogs * LOG_ENTRY_SIZE] = 0;
  if (!numLogs)
    strcpy(logs, "No logs!");
  else {
    for (unsigned int i = 0; i < numLogs; i++) {
      for (int j = 0; j < LOG_ENTRY_SIZE; j++)
        logs[i] = EEPROM.read(i * LOG_ENTRY_SIZE + j) + '!';
    }
  }
  send_SMS(logs, smsSender);
}

void set_text_flag(int8_t senderIndex, bool flag)
{
  if (senderIndex >= 0 && senderIndex < MAX_PHONE_NUMS) {
    sendSMSFlags[senderIndex] = flag;
    EEPROM.write(EEPROM_TEXTS_FLAG_ADDR + senderIndex, flag);
  }
}

void handle_toggle_text(char *response, int8_t senderIndex, char args[MAX_ARGS][MAX_TXT_STR_SIZE], char *smsSender)
{
  char tmp[] = "Texting is now ";
  if (!strcmp(args[1], "off")) {
    set_text_flag(senderIndex, false);
    strcpy(response, tmp);
    strcat(response, "off");
  } else if (!strcmp(args[1], "on")) {
    set_text_flag(senderIndex, true);
    strcpy(response, tmp);
    strcat(response, "on");
  } else {
    strcpy(response, "Error in input");
  }
  send_SMS(response, smsSender);
}

/*void handle_status(char *response, char *smsSender)
  {
  // get battery %
  uint16_t vbat;
  if (!fona.getBattPercent(&vbat))
    log_error(GET_BATT_PERCENT);
  char batPercent[15];
  itoa(vbat, batPercent, 10);

  strcpy(response, "Battery: ");
  strcat(response, batPercent);
  strcat(response, "%");

  send_SMS(response, smsSender);
  }*/

void handle_add_del_phone(char *response, char args[MAX_ARGS][MAX_TXT_STR_SIZE], char *smsSender)
{
  // args[1] is add or remove/delete, args[2] is the phone number in question
  int8_t index = find_phone_number(args[2]);
  if (!strcmp(args[1], "add")) {
    if (numPhoneNumbers == MAX_PHONE_NUMS) {
      strcpy(response, "Phone limit exceeded. Not added");
      send_SMS(response, smsSender);
      return;
    }

    if (index >= 0) {
      // found a matching phone number - do not add a repeat
      strcpy(response, "That phone is already recognized");
      send_SMS(response, smsSender);
      return;
    }

    // add the phone number and enable text alerts to that number
    for (uint8_t i = 0; i < PHONE_NUM_LEN; i++) {
      phoneNumbers[numPhoneNumbers][i] = args[2][i];
      EEPROM.write(EEPROM_PHONE_NUMS_ADDR + numPhoneNumbers * PHONE_NUM_LEN + i, args[2][i]);
    }

    // by default, enable texting for this number
    set_text_flag(numPhoneNumbers, true);

    char msg[] = "You've been added to PoachNet";
    send_SMS(msg, phoneNumbers[numPhoneNumbers]);

    phoneNumbers[numPhoneNumbers][PHONE_NUM_LEN] = 0;
    numPhoneNumbers++;
  } else if (!strcmp(args[1], "remove") || !strcmp(args[1], "delete")) {
    if (index < 0 || numPhoneNumbers == 1) {
      strcpy(response, "That phone cannot be deleted");
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
  strcpy(response, "OK. Current phones are: ");
  append_remaining_phone_nums(response);
  send_SMS(response, smsSender);

  EEPROM.write(EEPROM_NUM_PHONE_NUMS_ADDR, numPhoneNumbers);
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
  if (!fona.deleteSMS(smsn))
    log_error(DELETE_SMS);
}

// checks for command messages from trusted phone numbers
// partially copied from FONAtest file that comes with Arduino IDE
void check_messages()
{
  char smsContents[255], smsSenderBuf[PHONE_NUM_LEN + 3];
  int8_t smsnum = -1, smsn = 1;
  uint16_t smslen;

  // getNumSMS() returns -1 if the command to get # of texts fails
  // keep looping until it succeeds.  Sometimes when the phone first
  // starts, it takes a couple seconds for communication to be established.
  for (uint8_t i = 0; smsnum == -1; i++) {
    // if we can't get number of SMS after GET_MAX_NUM_SMS_TRIES tries, don't check messages
    if (i == MAX_GET_NUM_SMS_TRIES)
      return;
    delay(5000);
    smsnum = fona.getNumSMS();
  }

  for (; smsn <= smsnum; smsn++) {
    // Retrieve SMS sender address/phone number. Try a certain number of times before giving up
    for (int8_t i = 0; i < MAX_GET_NUM_SMS_TRIES; i++) {
      if (fona.getSMSSender(smsn, smsSenderBuf, PHONE_NUM_LEN + 2))
        break;
      // sometimes error text messages show up that don't have an smsSender
      if (i == MAX_GET_NUM_SMS_TRIES - 1) {
        delete_SMS(smsn);
        continue;
      }
      delay(1000);
    }

    // phone number will be "+1XXXXXXXXXX". We remove the +CountryCode part of the number by incrementing by 2
    // then add null terminator
    char *smsSender = &smsSenderBuf[2];
    smsSender[PHONE_NUM_LEN] = 0;

    if (!fona.readSMS(smsn, smsContents, 250, &smslen)) {  // pass in buffer and max len
      log_error(READ_SMS);
      break;
    }

    // if the length is zero, its a special case where the index number is higher
    // so increase the max we'll look at
    if (!smslen) {
      smsnum++;
      continue;
    }

    // buffer to store response to a text message
    char response[100];

    if (!strcmp(smsContents, passwd)) {
      handle_reactivate(response, smsSender);
      delete_SMS(smsn);
      continue;
    }

    // get the index of this phone number in our recognized numbers.
    // if it is not recognized, senderIndex will be -1
    int8_t senderIndex = find_phone_number(smsSender);

    // password was case-sensitive, but all other commands should be case insensitive
    to_lower_case(smsContents);

    if (!strcmp(smsContents, activateMsg)) {
      handle_activate(response, smsSender);
      delete_SMS(smsn);
      continue;
    }

    // only let an unrecognized phone number send an activate or reactivate command
    if (senderIndex == -1) {
      delete_SMS(smsn);
      continue;
    }

    char args[MAX_ARGS][MAX_TXT_STR_SIZE];
    tokenize_sms_by(smsContents, " ", args);

    if (!strcmp(args[0], SNOOZE_CMD))
      handle_snooze(response, args, senderIndex, smsSender);
    else if (!strcmp(args[0], RESET_CMD))
      reset_vars(senderIndex);
    else if (!strcmp(args[0], GET_LOGS_CMD))
      handle_get_logs(smsSender);
    else if (args[1] && !strcmp(args[0], TOGGLE_TEXT_CMD))
      handle_toggle_text(response, senderIndex, args, smsSender);
    else if (args[1] && args[2] && strlen(args[2]) >= 10 && !strcmp(args[0], ADD_DEL_PHONE_CMD))
      handle_add_del_phone(response, args, smsSender);
    /* else if (!strcmp(args[0], STATUS_CMD))
      handle_status(response, smsSender);*/

    // delete text message from FONA since it's already processed
    delete_SMS(smsn);
  }
}

void setup()
{
  //Watchdog.disable();

  // stuff to do during debugging/development
  if (debugFlag) {
    // start serial connection for debugging
    Serial.begin(115200);

    // wait for serial to connect before executing
    while (!Serial);
  }

  // GPIO pin 11 controls power to Feather's cell module
  pinMode(FONA_GPIO, OUTPUT);

  // GPIO pin 12 controls power to GPS module
  pinMode(GPS_GPIO, OUTPUT);

  start_fona();
  init_vars();
  check_messages();
  delay(20000);
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
  check_messages();
}

// sends a text to every phone registered with this device that has opted to receive texts
void broadcast_SMS(char *msg)
{
  for (uint8_t i = 0; i < numPhoneNumbers; i++) {
    if (sendSMSFlags[i])
      send_SMS(msg, phoneNumbers[i]);
  }
}

// sends a text with message msg to the given number
void send_SMS(char *msg, char phoneNumber[PHONE_NUM_LEN + 1])
{
  //if (!fona.sendSMS(phoneNumber, msg))
  //  log_error(SEND_SMS);
  fona.sendSMS(phoneNumber, msg);
}

// coordinatesFrom is the name of the device the coordinates came from. Either "GPS" or "cell"
void post_to_url(char *latitude, char *longitude, const char* coordinatesFrom)
{
  uint16_t statuscode;
  int16_t len;

  char fonaIMEI[20];
  fona.getIMEI(fonaIMEI);

  // format the data as "lat,lon"
  char urlWithCoords[strlen(url) + strlen(coordinatesFrom) + 2 * GPS_DATA_SIZE + 27];
  strcpy(urlWithCoords, url);
  strcat(urlWithCoords, latitude);
  strcat(urlWithCoords, ",");
  strcat(urlWithCoords, longitude);
  strcat(urlWithCoords, ",");
  strcat(urlWithCoords, fonaIMEI);
  strcat(urlWithCoords, ",");
  strcat(urlWithCoords, coordinatesFrom);

  if (sendToServerFlag) {
    if (!fona.HTTP_POST_start(urlWithCoords, F("text/plain"), (uint8_t *)" ", 1, &statuscode, (uint16_t *)&len))
      log_error(HTTP_POST);
  }
}

// formats a text message as:
// From XYZ:
// lat: X
// lon: X
// etc... for each item in args[]
void format_text_msg(char *msg, const char *prefix, const char *args[])
{
  strcpy(msg, prefix);
  while (*args) {
    strcat(msg, *args++);
    strcat(msg, ": ");
    strcat(msg, *args++);
    if (*args)
      strcat(msg, "\n");
  }
}

void loop()
{
  if (GPS.fix) {
    char latitude[GPS_DATA_SIZE], longitude[GPS_DATA_SIZE], speed[GPS_DATA_SIZE + 6];
    String lat = String(GPS.latitudeDegrees, 4);
    String lon = String(GPS.longitudeDegrees, 4);
    String spd = String(GPS.speed);
    lat.toCharArray(latitude, sizeof latitude);
    lon.toCharArray(longitude, sizeof longitude);
    spd.toCharArray(speed, sizeof speed);
    strcat(speed, " knots");

    char msg[100];
    const char *args[] = {"lat", latitude, "lon", longitude, "speed", speed, NULL};
    format_text_msg(msg, "From GPS\n", args);

    // post the data
    if (fona.enableGPRS(true)) {
      post_to_url(latitude, longitude, "GPS");
    } else {
      log_error(FONA_ENABLE_GPRS);
    }
    broadcast_SMS(msg);
    go_to_sleep();
  } else if (millis() - timer >= gpsTimeoutMS) {
    char msg[100];
    char replybuffer[255];
    if (fona.enableGPRS(true) && fona.getGSMLoc(NULL, replybuffer, sizeof replybuffer - 5)) {
      char longitude[GPS_DATA_SIZE], latitude[GPS_DATA_SIZE];

      char *tok = strtok(replybuffer, ",");
      strcpy(longitude, tok);
      tok = strtok(NULL, ",");
      strcpy(latitude, tok);

      const char *args[] = {"lat", latitude, "lon", longitude, NULL};
      format_text_msg(msg, "From cell\n", args);

      post_to_url(latitude, longitude, "cell");
    } else {
      format_text_msg(msg, "No GPS/GPRS available", {NULL});
      log_error(NO_GPS_GPRS_AVAIL);
    }

    broadcast_SMS(msg);
    go_to_sleep();
  }

  GPS.read();
  if (GPS.newNMEAreceived()) {
    GPS.parse(GPS.lastNMEA());
  }
}

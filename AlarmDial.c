#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"

// UART parameters for communication with the modem
// The modem needs to have been set to these values permanently as well (not handled by this program)
#define UART_ID uart0
#define BAUD_RATE 9600
#define DATA_BITS 8
#define STOP_BITS 1
#define PARITY UART_PARITY_NONE

// wait time for reading next character in microseconds, choose according to BAUD_RATE: 9/BAUD_RATE*1E6*1.5 (safety margin)
#define CHAR_INTERVAL_US 1500

#define UART_TX_PIN 0
#define UART_RX_PIN 1

// this sets the maximum allowable message length
#define max_str_l 200
#define LF '\x0A'
#define CR '\x0D'

// controls the printing of debugging messages on the USB interface
//#define DEBUG

// what GPIO pins to use to interface with the alarm system
#define GPIO_PIN_FIRST 2
#define GPIO_NUMBER_PINS 3

// GPIO pin for configuration reset
#define GPIO_PIN_PW_RESET 5

// time intervals for regular actions

// CPSI modem status check
// 2419200 sec is four weeks
#define CPSI_CHECK_INTERVAL_US 2419200000000
//#define CPSI_CHECK_INTERVAL_US 120000000

// CREG network registration check
// 28800 sec is eight hours
#define CREG_CHECK_INTERVAL_US 28800000000
//#define CREG_CHECK_INTERVAL_US 30000000

// CMGD incoming SMS buffer deletion
// 86400 sec is 24 hours
#define CMGD_INTERVAL_US 86400000000
//#define CMGD_INTERVAL_US 45000000

// maps incoming modem message strings into numerical values
#define OK      0
#define ERROR   1
#define CPSI    2
#define CREG    3
#define CPMS    4
#define CSQ     5
#define CMGD    6
#define CMGS    7
#define CMTI    8
#define CMGR    9
#define CLCC    10
#define UNKNOWN 11
#define MAX_MSG 12

#ifdef DEBUG
const char* const command_code_map[MAX_MSG] = { "OK",    \
                                                "ERROR", \
                                                "CPSI",  \
                                                "CREG",  \
                                                "CPMS",  \
                                                "CSQ",   \
                                                "CMGD",  \
                                                "CMGS",  \
                                                "CMTI",  \
                                                "CMGR",  \
                                                "CLCC",  \
                                                "UNKNOWN" };
#endif

// names the multi-stage actions
#define MULTI_STAGE_RECEIVED_SIGNAL_REQUEST 1
#define MULTI_STAGE_RECEIVED_TEL_NO         2
#define MULTI_STAGE_RECEIVED_PW             3
#define MULTI_STAGE_RECEIVED_PIN_ACTION     4
#define MULTI_STAGE_RECEIVED_MSG            5
#define MULTI_STAGE_SEND_SIGNAL_LEVEL       6
#define MULTI_STAGE_SEND_STATUS_MSG         7
#define MULTI_STAGE_RECEIVED_DEFAULTS       8
#define MULTI_STAGE_INVALID_COMMAND         9
#define MULTI_STAGE_MAX_ACTIONS		    10

// flash storage area for configuration
#define FLASH_TARGET_OFFSET (512 * 1024)
#define FLASH_SETTINGS_BYTES 1024

// ring buffer for interrupt handler
#define RX_BUFFER_SIZE 10000
char rx_buffer[RX_BUFFER_SIZE];
int rx_buffer_read_position = 0;
int rx_buffer_entries = 0;
int rx_buffer_write_position = 0;
int rx_buffer_number_lf = 0;

// interrupt handler with simple ring buffer for incoming characters from modem
// flags arrival of LF to main loop (complete message has arrived for processing)
// no overflow checking - buffer size is gargantuan for the data flow
void uart_rx_interrupt_handler() {
  char chr;

  while (uart_is_readable(UART_ID)) {
    chr = uart_getc(UART_ID);
    rx_buffer_entries++;
    rx_buffer[rx_buffer_write_position++] = chr;
    if (chr == LF) rx_buffer_number_lf++;
    if (rx_buffer_write_position == RX_BUFFER_SIZE) rx_buffer_write_position = 0;
  }
}

// reads a complete (i.e., LF-terminated) message from modem, or returns with 1 if no complete message arrives within specified timeout
// this function is only called when interrupt handler is not installed
// used for modem initialisation only
int read_message(char* message, uint32_t wait_us) {
  char chr;
  int success = 1;
  int l = 0;

  message[0] = 0;
  while (uart_is_readable_within_us(UART_ID, l ? (uint32_t)CHAR_INTERVAL_US : wait_us)) {
    chr = uart_getc(UART_ID);
    if ((chr != LF) && (chr != CR) && (l < max_str_l-1))
      message[l++] = chr;
    if (chr == LF) {
      message[l] = 0;
      success = 0;
      break;
    }
  }

  return success;
}

// writes a command (or data such as SMS text) to the modem
void write_command(char* command) {
  int l = 0;

  while ((l < max_str_l) && command[l])
    uart_putc_raw(UART_ID, command[l++]);
}

// writes a command to the modem and checks for a pre-deterimed response
// returns 0 upon success, or 1 if the required response has not arrived within specified timeout upon specified repeats
// all data other than the required response arriving from the modem in the meantime is discarded
// this function is only called when interrupt handler is not installed
// used for modem initialisation only
int write_command_with_response_check(char* command, char* target_response, char* response, uint32_t wait_us, int repeat) {
  int success = 1;
  int i = 0;
  int result;

  while ((i++ < repeat) && success) {
    while (uart_is_readable_within_us(UART_ID, 0))
      uart_getc(UART_ID);
    write_command(command);
    do {
      result = read_message(response, wait_us);
      if (!strncmp(response, target_response, strlen(target_response)))
        success = 0;
    } while (!result && success); 
  }

  return success;
}

// instructs the modem to send message as SMS
void send_sms(char* tel_no, char* message) {
  char msg[max_str_l];

  sprintf(msg, "AT+CMGS=\"%s\"\r", tel_no);
  write_command(msg);
  sleep_ms(500);
  sprintf(msg, "%s\x1A", message);
  write_command(msg);
}

// initialises the modem
// interrupt handler should not be installed when invoking this function
// no error checking implemented - unclear what we could sensibly do in an embedded system if an error occurred
void initialise_modem(void) {
  int result;
  char response[max_str_l];

#ifdef DEBUG
  printf("Entering modem initialisation\n");
#endif
  result = write_command_with_response_check("ATE0\r", "OK", response, (uint32_t)120000000, 3);
#ifdef DEBUG
  printf("ATE0 returned: %i %s\n", result, response);
#endif
  result = write_command_with_response_check("AT&D0\r", "OK", response, (uint32_t)9000000, 3);
#ifdef DEBUG
  printf("AT&D0 returned: %i %s\n", result, response);
#endif
  result = write_command_with_response_check("ATV1\r", "OK", response, (uint32_t)9000000, 3);
#ifdef DEBUG
  printf("ATV1 returned: %i %s\n", result, response);
#endif
  result = write_command_with_response_check("AT+CGEREP=0,0;+CVHU=0;+CLIP=0;+CLCC=1\r", "OK", response, (uint32_t)36000000, 3);
#ifdef DEBUG
  printf("CGEREP=0,0;CVHU=0;CLIP=0;CLCC=1 returned: %i %s\n", result, response);
#endif
  result = write_command_with_response_check("AT+CNMP=2;+CSCS=\"IRA\";+CMGF=1;+CNMI=2,1\r", "OK", response, (uint32_t)36000000, 3);
#ifdef DEBUG
  printf("CNMP=2;CSCS=IRA;CMGF=1;CNMI=2,1 returned: %i %s\n", result, response);
#endif
  result = write_command_with_response_check("AT+CPMS=\"SM\",\"SM\",\"SM\"\r", "OK", response, (uint32_t)9000000, 3);
#ifdef DEBUG
  printf("CPMS=SM,SM,SM returned: %i %s\n", result, response);
#endif
  result = write_command_with_response_check("AT+CMGD=0,4\r", "OK", response, (uint32_t)9000000, 3);
#ifdef DEBUG
  printf("CMGD=0,4 returned: %i %s\n", result, response);
#endif
  result = write_command_with_response_check("AT+CPMS=\"ME\",\"ME\",\"ME\"\r", "OK", response, (uint32_t)9000000, 3);
#ifdef DEBUG
  printf("CPMS=ME,ME,ME returned: %i %s\n", result, response);
#endif
  result = write_command_with_response_check("AT+CMGD=0,4\r", "OK", response, (uint32_t)9000000, 3);
#ifdef DEBUG
  printf("CMGD=0,4 returned: %i %s\n", result, response);
  printf("Exiting modem initialisation\n");
#endif
}

int main(void) {
// variables for LED action control
  const uint LED_PIN = 25;
  bool led_onoff = false;
  absolute_time_t last_led_switch_time;

// work variables
  int i, j, k, l;
  char chr;
  char str[max_str_l];

// variables to communicate through handling levels of multi-stage actions
  int multi_stage_handling_type = 0;
  char multi_stage_message[MULTI_STAGE_MAX_ACTIONS][max_str_l];

// variables relating to messages and data received from modem
  bool received[MAX_MSG];
  char received_response[MAX_MSG][max_str_l];
  char received_sms_text[max_str_l];
  bool recognised_instruction;

// variables relating to GPIO input pins (alarm system connections)
  bool last_status[GPIO_NUMBER_PINS] = { false, false, false };
  bool status;
  absolute_time_t last_status_check_time;

// variables relating to GPIO input pin for password reset
  absolute_time_t last_passw_reset_time, last_passw_reset_check_time;

// variables indicating status of actions
  bool awaiting_response[MAX_MSG];
  bool received_sms = false;

// variables storing event times to control timeouts
  absolute_time_t current_time, last_creg_check_time, last_cpsi_check_time, last_cmgd_time;
  absolute_time_t initiate_time[MAX_MSG];

// variables relating to interrupt control
  int uart_irq;
  uint32_t interrupts;

// variables for configuration storage in flash memory
  uint8_t* flash_target_contents;
  char flash_settings[FLASH_SETTINGS_BYTES];
  uint8_t checksum;
  bool store_new_flash_settings = false;

// default configuration
  const char* const default_passw = "674358";
  const char* const default_tel_no = "+447700900000";
  const bool default_send_sms_on_change[GPIO_NUMBER_PINS] = { true, true, true };
  const char* const default_sms_on_fall[GPIO_NUMBER_PINS] = { "Intruder alarm triggered", "Alarm system armed", "Panic button pressed" };
  const char* const default_sms_on_rise[GPIO_NUMBER_PINS] = { "Intruder alarm cleared", "Alarm system disarmed", "Panic button cleared" };

// current configuration
  char passw[7];
  char tel_no[50];
  bool send_sms_on_change[GPIO_NUMBER_PINS];
  char sms_on_fall[GPIO_NUMBER_PINS][50];
  char sms_on_rise[GPIO_NUMBER_PINS][50];

  stdio_init_all();

#ifdef DEBUG
// give some time to connect to USB interface
  sleep_ms(10000);
  printf("Starting up\n");
#endif

#ifdef DEBUG
// check if there was a reboot by the watchdog
  if (watchdog_caused_reboot())
    printf("Rebooted by watchdog\n");
  else
    printf("Clean boot, not from watchdog\n");
#endif

// configure UART for communication with modem
  uart_init(UART_ID, BAUD_RATE);
  gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
  uart_set_hw_flow(UART_ID, false, false);
  uart_set_format(UART_ID, DATA_BITS, STOP_BITS, PARITY);
  uart_set_fifo_enabled(UART_ID, true);

// configure LED
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);

// configure GPIO pins for interfacing with the alarm system
  for (i = 0; i < GPIO_NUMBER_PINS; i++) {
    gpio_init(GPIO_PIN_FIRST + i);
    gpio_set_dir(GPIO_PIN_FIRST + i, GPIO_IN);
    gpio_pull_up(GPIO_PIN_FIRST + i);
  }

// configure GPIO pin for password reset
  gpio_init(GPIO_PIN_PW_RESET);
  gpio_set_dir(GPIO_PIN_PW_RESET, GPIO_IN);
  gpio_pull_up(GPIO_PIN_PW_RESET);

// restore settings from flash, if not available schedule storage of defaults
#ifdef DEBUG
  printf("Read configuration stored in flash memory\n");
#endif
  flash_target_contents = (uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET);
  memcpy(flash_settings, flash_target_contents, FLASH_SETTINGS_BYTES);
  checksum = 0;
  for (i = 1; i < FLASH_SETTINGS_BYTES; i++)
    checksum = checksum + flash_settings[i];
// uncomment following line to force saving (new) defaults, run once, then comment out again
//    checksum = 0;
  if (checksum != flash_settings[0]) {
#ifdef DEBUG
    printf("Flash configuration checksum mismatch, will save defaults\n");
#endif
    strcpy(passw, default_passw);
    strcpy(tel_no, default_tel_no);
    for (i = 0; i < GPIO_NUMBER_PINS; i++) {
      strcpy(sms_on_fall[i], default_sms_on_fall[i]);
      strcpy(sms_on_rise[i], default_sms_on_rise[i]);
      send_sms_on_change[i] = default_send_sms_on_change[i];
    }
    store_new_flash_settings = true;
  }
  else {
#ifdef DEBUG
    printf("Applying settings from flash memory\n");
#endif
    l = 0;
    i = 0;
    do {
      passw[i++] = flash_settings[++l];
    } while (flash_settings[l]);
    i = 0;
    do {
      tel_no[i++] = flash_settings[++l];
    } while (flash_settings[l]);
    for (i = 0; i < GPIO_NUMBER_PINS; i++) {
      j = 0;
      do {
        sms_on_fall[i][j++] = flash_settings[++l];
      } while (flash_settings[l]);
    }
    for (i = 0; i < GPIO_NUMBER_PINS; i++) {
      j = 0;
      do {
        sms_on_rise[i][j++] = flash_settings[++l];
      } while (flash_settings[l]);
    }
    for (i = 0; i < GPIO_NUMBER_PINS; i++)
      send_sms_on_change[i] = flash_settings[++l];
#ifdef DEBUG
    printf("Applied the following settings from flash memory:\n");
    printf("Password: %s\n", passw);
    printf("Telephone number: %s\n", tel_no);
    for (i = 0; i < GPIO_NUMBER_PINS; i++) {
      printf("SMS on fall for pin %1d: %s\n", i, sms_on_fall[i]);
      printf("SMS on rise for pin %1d: %s\n", i, sms_on_rise[i]);
      printf("Send SMS on change for pin %1d: %s\n", i, send_sms_on_change[i] ? "Yes" : "No");
    }
#endif
  }

// reboot modem and give it some time to start up 
#ifdef DEBUG
  printf("Reboot the modem, sleep a bit, then initialise modem\n");
#endif
  sleep_ms(10000);
  write_command("AT+CRESET\r");
  sleep_ms(30000);
  initialise_modem();

// initialise regular modem checks, GPIO checking interval, LED blinking interval
  current_time = get_absolute_time();
  last_status_check_time = current_time;
  last_passw_reset_time = current_time;
  last_passw_reset_check_time = current_time;
  last_led_switch_time = current_time;
  last_creg_check_time = current_time;
  last_cpsi_check_time = current_time;
  last_cmgd_time = current_time;

// initialise incoming modem message and action flags
  for (i = 0; i < MAX_MSG; i++) {
    received[i] = 0;
    awaiting_response[i] = false;
    initiate_time[i] = current_time;
  }

// disable FIFO buffer, the interrupt handler works better without it
  uart_set_fifo_enabled(UART_ID, false);
// install interrupt handler
  uart_irq = UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;
  irq_set_exclusive_handler(uart_irq, uart_rx_interrupt_handler);
  irq_set_enabled(uart_irq, true);
  uart_set_irq_enables(UART_ID, true, false);

// enable watchdog with timeout of 8 seconds
  watchdog_enable((uint32_t)8000, false);

// main loop
  while (true) {

// store one time for one loop traversal
    current_time = get_absolute_time();
    watchdog_update();

// if the ring buffer has a message (a LF has arrived), then read one message
    if (rx_buffer_number_lf > 0) {
      str[0] = 0;
      l = 0;
      do {
        chr = rx_buffer[rx_buffer_read_position++];
        rx_buffer_entries--;
        if (rx_buffer_read_position == RX_BUFFER_SIZE)
          rx_buffer_read_position = 0;
        if ((chr != LF) && (chr != CR) && (l < max_str_l-1))
          str[l++] = chr;
        if (chr == LF) rx_buffer_number_lf--;
      } while ((chr != LF) && (l < max_str_l-1) && (rx_buffer_entries > 0));
// make message into a string
      str[l] = '\0';
// if there is a nonempty message, determine the type and set the action flag
      if (l > 0) {
        if (!strncmp(str, "OK", 2)) {
          received[OK] = true;
        }
        else if (!strncmp(str, "ERROR", 5)) {
          received[ERROR] = true;
#ifdef DEBUG
          printf("Received ERROR\n");
#endif
        }
        else if (!strncmp(str, "+CPSI", 5)) {
          received[CPSI] = true;
          strcpy(received_response[CPSI], str);
        }
        else if (!strncmp(str, "+CREG", 5)) {
          received[CREG] = true;
          strcpy(received_response[CREG], str);
        }
        else if (!strncmp(str, "+CPMS", 5)) {
          received[CPMS] = true;
          strcpy(received_response[CPMS], str);
        }
        else if (!strncmp(str, "+CSQ", 4)) {
          received[CSQ] = true;
          strcpy(received_response[CSQ], str);
        }
        else if (!strncmp(str, "+CMGD", 5)) {
          received[CMGD] = true;
          strcpy(received_response[CMGD], str);
        }
        else if (!strncmp(str, "+CMGS", 5)) {
          received[CMGS] = true;
          strcpy(received_response[CMGS], str);
        }
        else if (!strncmp(str, "+CMTI", 5)) {
          received[CMTI] = true;
          strcpy(received_response[CMTI], str);
        }
        else if (!strncmp(str, "+CMGR", 5)) {
          received[CMGR] = true;
          strcpy(received_response[CMGR], str);
        }
        else if (!strncmp(str, "+CLCC", 5)) {
          received[CLCC] = true;
          strcpy(received_response[CLCC], str);
        }
        else if (str[0] == '>')
          ;
        else if (str[0] == '\0')
          ;
// this is the catchall for modem messages relating to commands (starting with "+")
        else if (str[0] == '+') {
          received[UNKNOWN] = true;
          strcpy(received_response[UNKNOWN], str);
        }
// at this point we only have non-command related data from the modem, such as incoming SMS text
        else {
          if (awaiting_response[CMGR]) {
            received_sms = true;
            strcpy(received_sms_text, str);
          }
#ifdef DEBUG
          if (!awaiting_response[CMGR]) printf("Received unprocessed non-command string: %s\n", str);
#endif
        }
      }
    }

// if there is pending action, we want to block new actions (e.g., defer the regular checks)
// for that, we collect any specific awaiting_response entries into the UNKNOWN entry, which will be used for blocking new action
    awaiting_response[UNKNOWN] = false;
    for (i = 0; i < MAX_MSG-1; i++)
      awaiting_response[UNKNOWN] = awaiting_response[UNKNOWN] || awaiting_response[i];

// regular modem modem status check, including reset if necessary
    if ((absolute_time_diff_us(last_cpsi_check_time, current_time) > (int64_t)CPSI_CHECK_INTERVAL_US) && !awaiting_response[UNKNOWN]) {
#ifdef DEBUG
      printf("Initiating regular modem status check\n");
#endif
      write_command("AT+CPSI?\r");
      initiate_time[CPSI] = current_time;
      awaiting_response[CPSI] = true;
      awaiting_response[UNKNOWN] = true;
      last_cpsi_check_time = current_time;
    }
    if (received[CPSI] && awaiting_response[CPSI]) {
#ifdef DEBUG
      printf("Received CPSI: %s\n", received_response[CPSI]);
#endif
      received[CPSI] = false;
      awaiting_response[CPSI] = false;
      if (strstr(received_response[CPSI], "Online") != NULL) {
// if the modem is online, send a status message via SMS
        sprintf(multi_stage_message[MULTI_STAGE_SEND_STATUS_MSG], "Modem check: %s", &received_response[CPSI][7]);
        multi_stage_handling_type = MULTI_STAGE_SEND_STATUS_MSG;
        initiate_time[OK] = current_time;
        awaiting_response[OK] = true;
        awaiting_response[UNKNOWN] = true;
      }
      else {
// if the modem is not online, reboot (modem is reset upon boot)
#ifdef DEBUG
        printf("Rebooting...\n");
        sleep_ms(1000);
#endif
        watchdog_enable((uint32_t)1, false);
        sleep_ms(5);
#ifdef DEBUG
        printf("This point should never be reached due to the watchdog\n");
#endif
        while(true);
      }
    }
    else if (received[CPSI]) {
      received[CPSI] = false;
#ifdef DEBUG
      printf("Received unexpected CPSI\n");
#endif
    }

// regular network registration check, don't action response
    if ((absolute_time_diff_us(last_creg_check_time, current_time) > (int64_t)CREG_CHECK_INTERVAL_US) && !awaiting_response[UNKNOWN]) {
#ifdef DEBUG
      printf("Initiating regular CREG\n");
#endif
      write_command("AT+CREG?\r");
      initiate_time[CREG] = current_time;
      awaiting_response[CREG] = true;
      awaiting_response[UNKNOWN] = true;
      last_creg_check_time = current_time;
    }
    if (received[CREG] && awaiting_response[CREG]) {
#ifdef DEBUG
      printf("Received CREG: %s\n", received_response[CREG]);
#endif
      received[CREG] = false;
      awaiting_response[CREG] = false;
      initiate_time[OK] = current_time;
      awaiting_response[OK] = true;
      awaiting_response[UNKNOWN] = true;
    }
    else if (received[CREG]) {
      received[CREG] = false;
#ifdef DEBUG
      printf("Received unexpected CREG\n");
#endif
    }
             
// process CMTI (modem signalling incoming SMS)
    if (received[CMTI] && !awaiting_response[UNKNOWN]) {
#ifdef DEBUG
      printf("Received CMTI: %s\n", received_response[CMTI]);
#endif
      received[CMTI] = false;
// we want to process the SMS, so need to read it out from the modem first
      sprintf(str, "AT+CMGR=%s\r", &received_response[CMTI][12]);
      write_command(str);
      initiate_time[CMGR] = current_time;
      awaiting_response[CMGR] = true;
      awaiting_response[UNKNOWN] = true;
    }

// process CLCC (modem signalling incoming voice call)
    if (received[CLCC] && !awaiting_response[UNKNOWN]) {
#ifdef DEBUG
      printf("Received CLCC: %s\n", received_response[CLCC]);
#endif
      received[CLCC] = false;
// hang up call
#ifdef DEBUG
      printf("Hanging up\n");
#endif
      write_command("AT+CHUP\r");
      initiate_time[OK] = current_time;
      awaiting_response[OK] = true;
      awaiting_response[UNKNOWN] = true;
    }

// process CMGR (SMS read-out from modem)
    if (received[CMGR] && awaiting_response[CMGR] && received_sms) {
#ifdef DEBUG
      printf("Received CMGR: %s\n", received_response[CMGR]);
#endif
      received[CMGR] = false;
      awaiting_response[CMGR] = false;
      received_sms = false;
      initiate_time[OK] = current_time;
      awaiting_response[OK] = true;
      awaiting_response[UNKNOWN] = true;

// now figure out what the SMS is instructing us to do, if any
      recognised_instruction = !strncmp(received_sms_text, passw, sizeof(passw) - 1);

// did we receive a signal level request?
      sprintf(str, "%s Signal?", passw);
      if (!strncmp(received_sms_text, str, sizeof(passw) + sizeof(" Signal?") - 2)) {
#ifdef DEBUG
        printf("Received signal level request\n");
#endif
// we need to wait for the OK from the modem first before we can handle this any further, so signal to the OK processing
        multi_stage_handling_type = MULTI_STAGE_RECEIVED_SIGNAL_REQUEST;
        recognised_instruction = false;
      }

// did we receive a new telephone number?
      sprintf(str, "%s TelephoneNumber!", passw);
      j = sizeof(passw) + sizeof(" TelephoneNumber!") - 2;
      if (!strncmp(received_sms_text, str, j)) {
#ifdef DEBUG
        printf("Received telephone number change request\n");
#endif
// we need to wait for the OK from the modem first before we can respond to the request, so signal to the OK processing
        multi_stage_handling_type = MULTI_STAGE_RECEIVED_TEL_NO;
// extract the new number and apply if valid, signal result to OK processing
        strcpy(str, &received_sms_text[j]);
// the following line performs some rudimentary check for a valid UK number, adapt for your country, uncomment and uncomment the lines below
//        if (!strncmp(str, "+44", 3) && (strlen(str) > 12)) {
#ifdef DEBUG
          printf("Changing telephone number to: %s\n", str);
#endif
          strncpy(tel_no, str, 49);
          store_new_flash_settings = true;
          strcpy(multi_stage_message[MULTI_STAGE_RECEIVED_TEL_NO], "Ok. Changed telephone number");
// uncomment the following seven lines if you have implemented a number format check above
//        }
//        else {
//#ifdef DEBUG
//          printf("Received invalid telephone number\n");
//#endif
//          strcpy(multi_stage_message[MULTI_STAGE_RECEIVED_TEL_NO], "Error. Invalid telephone number (needs to start with +44 and contain at least 13 characters)");
//        }
        recognised_instruction = false;
      }

// did we receive a new password?
      sprintf(str, "%s Password!", passw);
      j = sizeof(passw) + sizeof(" Password!") - 2;
      if (!strncmp(received_sms_text, str, j)) {
#ifdef DEBUG
        printf("Received password change request\n");
#endif
// we need to wait for the OK from the modem first before we can respond to the request, so signal to the OK processing
        multi_stage_handling_type = MULTI_STAGE_RECEIVED_PW;
// extract the new password and apply if valid, signal result to OK processing
        strcpy(str, &received_sms_text[j]);
        if (strlen(str) == 6) {
#ifdef DEBUG
          printf("Changing password to: %s\n", str);
#endif
          strcpy(passw, str);
          store_new_flash_settings = true;
          strcpy(multi_stage_message[MULTI_STAGE_RECEIVED_PW], "Ok. Changed password");
        }
        else {
#ifdef DEBUG
          printf("Received invalid password\n");
#endif
          strcpy(multi_stage_message[MULTI_STAGE_RECEIVED_PW], "Error. Invalid password (needs to be 6 characters)");
        }
        recognised_instruction = false;
      }

// did we receive a change to SMS action rules?
      sprintf(str, "%s SMSonInput!", passw);
      j = sizeof(passw) + sizeof(" SMSonInput!") - 2;
      if (!strncmp(received_sms_text, str, j)) {
#ifdef DEBUG
        printf("Received request to toggle action on input change\n");
#endif
// we need to wait for the OK from the modem first before we can respond to the request, so signal to the OK processing
        multi_stage_handling_type = MULTI_STAGE_RECEIVED_PIN_ACTION;
        i = received_sms_text[j] - '1';
        if ((i >= 0) && (i < GPIO_NUMBER_PINS) && (received_sms_text[j+1] == '\0')) {
#ifdef DEBUG
          printf("Changing action on input change of pin: %1d\n", i);
#endif
// extract the pin where SMS triggering should be changed and apply if valid, signal result to OK processing
          send_sms_on_change[i] = !send_sms_on_change[i];
          store_new_flash_settings = true;
          sprintf(multi_stage_message[MULTI_STAGE_RECEIVED_PIN_ACTION], "Ok. Input %1d will %strigger SMS from now on", i + 1, send_sms_on_change[i] ? "" : "not ");
        }
        else {
#ifdef DEBUG
          printf("Received invalid input change action request\n");
#endif
          sprintf(multi_stage_message[MULTI_STAGE_RECEIVED_PIN_ACTION], "Error. Invalid input number (must be 1-%1d)", GPIO_NUMBER_PINS);
        }
        recognised_instruction = false;
      }

// did we receive a request to change a message text?
      sprintf(str, "%s MessageText!", passw);
      j = sizeof(passw) + sizeof(" MessageText!") - 2;
      if (!strncmp(received_sms_text, str, j)) {
#ifdef DEBUG
        printf("Received request to change a message text\n");
#endif
// we need to wait for the OK from the modem first before we can respond to the request, so signal to the OK processing
        multi_stage_handling_type = MULTI_STAGE_RECEIVED_MSG;
        k = received_sms_text[j] - '1';
        l = 0;
        if (!strncmp(&received_sms_text[j+2], "On!", 3))
          l = 1;
        else if (!strncmp(&received_sms_text[j+2], "Off!", 4))
          l = 2;
        if (received_sms_text[j+1] != '!')
          l = 0;
        if ((k >= 0) && (k < GPIO_NUMBER_PINS) && l) {
          if (l == 1) {
            strncpy(sms_on_fall[k], &received_sms_text[j+5], sizeof(sms_on_fall[k])-1);
#ifdef DEBUG
            printf("Changing message for pin %1d on fall to: \"%s\"\n", k, sms_on_fall[k]);
#endif
            sprintf(multi_stage_message[MULTI_STAGE_RECEIVED_MSG], "Ok. New message for input %1d activating: \"%s\"", k + 1, sms_on_fall[k]);
          }
          else {
            strncpy(sms_on_rise[k], &received_sms_text[j+6], sizeof(sms_on_rise[k])-1);
#ifdef DEBUG
            printf("Changing message for pin %1d on rise to: \"%s\"\n", k, sms_on_rise[k]);
#endif
            sprintf(multi_stage_message[MULTI_STAGE_RECEIVED_MSG], "Ok. New message for input %1d deactivating: \"%s\"", k + 1, sms_on_rise[k]);
          }
          store_new_flash_settings = true;
        }
        else {
#ifdef DEBUG
          printf("Received invalid request to change a message\n");
#endif
          sprintf(multi_stage_message[MULTI_STAGE_RECEIVED_MSG], "Error. Invalid message change request");
        }
        recognised_instruction = false;
      }

// did we receive a request to reset settings to defaults?
      sprintf(str, "%s Defaults!", passw);
      j = sizeof(passw) + sizeof(" Defaults!") - 2;
      if (!strncmp(received_sms_text, str, j)) {
#ifdef DEBUG
        printf("Received request to reset settings to defaults\n");
#endif
// we need to wait for the OK from the modem first before we can respond to the request, so signal to the OK processing
        multi_stage_handling_type = MULTI_STAGE_RECEIVED_DEFAULTS;
#ifdef DEBUG
        printf("Resetting settings to defaults\n");
#endif
        sprintf(multi_stage_message[MULTI_STAGE_RECEIVED_DEFAULTS], "Ok. Resetting settings to defaults");
        strcpy(passw, default_passw);
        strcpy(tel_no, default_tel_no);
        for (i = 0; i < GPIO_NUMBER_PINS; i++) {
          strcpy(sms_on_fall[i], default_sms_on_fall[i]);
          strcpy(sms_on_rise[i], default_sms_on_rise[i]);
          send_sms_on_change[i] = default_send_sms_on_change[i];
        }
        store_new_flash_settings = true;
        recognised_instruction = false;
      }

// we received the correct password but no recognised instruction, so send a response to that
      if (recognised_instruction) {
#ifdef DEBUG
        printf("Received correct password but no valid instruction: %s\n", received_sms_text);
#endif
// we need to wait for the UK from the modem first before we can respond to the request
        multi_stage_handling_type = MULTI_STAGE_INVALID_COMMAND;
        sprintf(multi_stage_message[MULTI_STAGE_INVALID_COMMAND], "Invalid instruction");
      }
    } else if (received[CMGR] && !awaiting_response[CMGR]) {
      received[CMGR] = false;
      received_sms = false;
#ifdef DEBUG
      printf("Received unexpected CMGR\n");
#endif
    } else if (!awaiting_response[CMGR] && received_sms) {
      received_sms = false;
#ifdef DEBUG
      printf("Received unexpected SMS\n");
#endif
    }

// process CSQ (readout of signal level from modem)
    if (received[CSQ] && awaiting_response[CSQ]) {
#ifdef DEBUG
      printf("Received CSQ: %s\n", received_response[CSQ]);
#endif
      received[CSQ] = false;
      awaiting_response[CSQ] = false;
      j = strlen(received_response[CSQ]) - 1;
      for (i = 0; (received_response[CSQ][6+i] != ',') && (i < j); i++)
        str[i] = received_response[CSQ][6+i];
      str[i] = 0;
      sprintf(multi_stage_message[MULTI_STAGE_SEND_SIGNAL_LEVEL], "Signal quality is %s", str);
// we need to wait for the OK from the modem first before we can respond to the request, so signal to the OK processing
      multi_stage_handling_type = MULTI_STAGE_SEND_SIGNAL_LEVEL;
      initiate_time[OK] = current_time;
      awaiting_response[OK] = true;
      awaiting_response[UNKNOWN] = true;
    }
    else if (received[CSQ]) {
      received[CSQ] = false;
#ifdef DEBUG
      printf("Received unexpected CSQ\n");
#endif
    }

// regular message deletion
    if ((absolute_time_diff_us(last_cmgd_time, current_time) > (int64_t)CMGD_INTERVAL_US) && !awaiting_response[UNKNOWN]) {
#ifdef DEBUG
      printf("Initiate regular message deletion\n");
#endif
      write_command("AT+CMGD=0,4\r");
      initiate_time[OK] = current_time;
      awaiting_response[OK] = true;
      awaiting_response[UNKNOWN] = true;
      last_cmgd_time = current_time;
    }

// process CMGS (modem response to sending SMS)
    if (received[CMGS] && awaiting_response[CMGS]) {
#ifdef DEBUG
      printf("Received CMGS: %s\n", received_response[CMGS]);
#endif
      received[CMGS] = false;
      awaiting_response[CMGS] = false;
      initiate_time[OK] = current_time;
      awaiting_response[OK] = true;
      awaiting_response[UNKNOWN] = true;
    }
    else if (received[CMGS]) {
      received[CMGS] = false;
#ifdef DEBUG
      printf("Received unexpected CMGS\n");
#endif
    }

// process OK (modem response to pretty much any instruction)
// we need to handle the multi-stage actions here too, as signalled
    if (received[OK] && awaiting_response[OK]) {
#ifdef DEBUG
      printf("Received OK\n");
#endif
      received[OK] = false;
      awaiting_response[OK] = false;
// is multi-stage action a signal level request?
      if (multi_stage_handling_type == MULTI_STAGE_RECEIVED_SIGNAL_REQUEST) {
        write_command("AT+CSQ\r");
        initiate_time[CSQ] = current_time;
        awaiting_response[CSQ] = true;
        awaiting_response[UNKNOWN] = true;
        multi_stage_handling_type = 0;
      }
// send SMS for all other multi-stage actions
      else if (multi_stage_handling_type) {
#ifdef DEBUG
        printf("Sending SMS: %s\n", multi_stage_message[multi_stage_handling_type]);
#endif
        send_sms(tel_no, multi_stage_message[multi_stage_handling_type]);
        initiate_time[CMGS] = current_time;
        awaiting_response[CMGS] = true;
        awaiting_response[UNKNOWN] = true;
        multi_stage_handling_type = 0;
      }
    }
    else if (received[OK]) {
      received[OK] = false;
#ifdef DEBUG
      printf("Received unexpected OK\n");
#endif
    }

// process unknown modem message
    if (received[UNKNOWN] && !awaiting_response[UNKNOWN]) {
#ifdef DEBUG
      printf("Received unknown modem message: %s\n", received_response[UNKNOWN]);
#endif
      received[UNKNOWN] = false;
// receiving such an SMS will be confusing for the general user, uncomment following five lines only if you can interpret such an SMS
//      sprintf(str, "Unknown modem message: %s", received_response[UNKNOWN]);
//      send_sms(tel_no, str);
//      initiate_time[CMGS] = current_time;
//      awaiting_response[CMGS] = true;
//      awaiting_response[UNKNOWN] = true;
    }

// check for timeouts
    for (i = 0; i < MAX_MSG-1; i++) {
      if ((absolute_time_diff_us(initiate_time[i], current_time) > ((i == OK) ? (int64_t)60000000 : (int64_t)9000000)) && \
          awaiting_response[i]) {
#ifdef DEBUG
        printf("Timeout %s\n", command_code_map[i]);
#endif
        awaiting_response[i] = false;
        if (i == CMGR) multi_stage_handling_type = 0;
      }
    }

// check GPIO pins, action with SMS if change detected
    if ((absolute_time_diff_us(last_status_check_time, current_time) > 1000000) && !awaiting_response[UNKNOWN]) {
      last_status_check_time = current_time;
      for (i = 0; i < GPIO_NUMBER_PINS; i++) {
        status = !gpio_get(GPIO_PIN_FIRST + i);
        if (status != last_status[i]) {
#ifdef DEBUG
          printf("%s\n", status ? sms_on_fall[i] : sms_on_rise[i]);
#endif
          last_status[i] = !last_status[i];
          if (send_sms_on_change[i]) {
            send_sms(tel_no, status ? sms_on_fall[i] : sms_on_rise[i]);
            initiate_time[CMGS] = current_time;
            awaiting_response[CMGS] = true;
            awaiting_response[UNKNOWN] = true;
          }
        }
      }
    }

// check GPIO pin for password reset
    if ((absolute_time_diff_us(last_passw_reset_check_time, current_time) > 1000000) && \
        (absolute_time_diff_us(last_passw_reset_time, current_time) > 10000000) && !awaiting_response[UNKNOWN]) {
      last_passw_reset_check_time = current_time;
      if (!gpio_get(GPIO_PIN_PW_RESET)) {
        last_passw_reset_time = current_time;
#ifdef DEBUG
        printf("Password reset triggered by GPIO %i\n", GPIO_PIN_PW_RESET);
#endif
        strcpy(passw, default_passw);
        store_new_flash_settings = true;
        send_sms(tel_no, "Password reset to default");
        initiate_time[CMGS] = current_time;
        awaiting_response[CMGS] = true;
        awaiting_response[UNKNOWN] = true;
      }
    }


// loop slowdown
    sleep_ms(10);

// LED blinking to signal all is working
    if (absolute_time_diff_us(last_led_switch_time, current_time) > 1000000) {
      last_led_switch_time = current_time;
      gpio_put(LED_PIN, led_onoff);
      led_onoff = !led_onoff;
    }

// save new flash settings if necessary
    if (store_new_flash_settings && !awaiting_response[UNKNOWN]) {
#ifdef DEBUG
      printf("Saving new flash settings\n");
#endif
      l = 1;
      for (i = 0; i < sizeof(passw); i++)
        flash_settings[l++] = passw[i];
      j = strlen(tel_no);
      for (i = 0; i <= j; i++)
        flash_settings[l++] = tel_no[i];
      for (i = 0; i < GPIO_NUMBER_PINS; i++) {
        k = strlen(sms_on_fall[i]);
        for (j = 0; j <= k; j++)
          flash_settings[l++] = sms_on_fall[i][j];
      }
      for (i = 0; i < GPIO_NUMBER_PINS; i++) {
        k = strlen(sms_on_rise[i]);
        for (j = 0; j <= k; j++)
          flash_settings[l++] = sms_on_rise[i][j];
      }
      for (i = 0; i < GPIO_NUMBER_PINS; i++)
        flash_settings[l++] = send_sms_on_change[i];
      checksum = 0;
      for (i = 1; i < FLASH_SETTINGS_BYTES; i++)
        checksum = checksum + flash_settings[i];
      flash_settings[0] = checksum;

      interrupts = save_and_disable_interrupts();
      flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
      flash_range_program(FLASH_TARGET_OFFSET, flash_settings, FLASH_SETTINGS_BYTES);
      restore_interrupts(interrupts);

      store_new_flash_settings = false;
#ifdef DEBUG
      printf("Saved new flash settings\n");
#endif
    }
  }
}

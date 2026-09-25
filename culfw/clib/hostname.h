#ifndef _HOSTNAME_H_
#define _HOSTNAME_H_

#include <stdint.h>

/* The device's host name (HAS_HOSTNAME): sent to the DHCP server as
   option 12, so the router lists the device under it. Stored in
   EE_HOSTNAME; unset or invalid, it is BOARD_NAME-XXXXXX from the MAC. */

void hostname_load(void);                     // EEPROM -> RAM
const char *hostname_get(void);
uint8_t hostname_valid(const char *s, uint16_t len);   // RFC 1123 label
void hostname_store(const char *s, uint8_t len);       // len 0: default

/* Rih / Wih<name> */
void hostname_read(void);
void hostname_write(char *in);

#endif

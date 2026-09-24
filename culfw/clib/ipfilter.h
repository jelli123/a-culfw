#ifndef _IPFILTER_H_
#define _IPFILTER_H_

#include <stdint.h>

/* Whitelist of the addresses and networks whose TCP and ICMP packets the
   stack answers at all. An empty list lets everyone in. */

#define IPFILTER_MAX 4

typedef struct {
  uint8_t ip[4];
  uint8_t prefix;                     // 0..32
} ipfilter_entry;

void ipfilter_load(void);             // EEPROM -> RAM
void ipfilter_clear(void);            // allow everyone again

/* Parses "192.168.1.0/24, 10.0.0.5" (',' or ' ' between entries, no
   prefix: /32). Returns the number of entries, or -1. */
int8_t ipfilter_parse(const char *s, uint16_t len, ipfilter_entry *e);
void ipfilter_store(const ipfilter_entry *e, uint8_t n);

uint8_t ipfilter_get(ipfilter_entry *e);      // the active list
uint8_t ipfilter_match(const ipfilter_entry *e, uint8_t n,
                       const uint8_t ip[4]);   // n == 0: everyone

/* Rif / Wif<list> */
void ipfilter_read(void);
void ipfilter_write(char *in);

#endif

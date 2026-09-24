/*
 * IP whitelist for the uIP stack (UIP_CONF_IP_FILTER).
 *
 * uip_process() asks uip_ip_allowed() for every TCP and ICMP packet and
 * drops it silently when the sender is not on the list: no RST, no echo
 * reply. UDP is not filtered - uIP hands it to its own connections only
 * (DHCP, NTP), and the servers behind those need not be on the list.
 *
 * EE_IP_FILTER holds a marker and IPFILTER_MAX entries of address and
 * prefix length. Without the marker, or with no valid entry, everyone is
 * allowed. The list is kept in RAM, as every packet is checked.
 */

#include <string.h>
#include <avr/pgmspace.h>

#include "board.h"
#include "display.h"
#include "fncollection.h"
#include "ipfilter.h"

#define FILTER_SET  0xA5              // any other marker: no list

static ipfilter_entry list[IPFILTER_MAX];
static uint8_t list_n;

void
ipfilter_load(void)
{
  list_n = 0;
  if(erb(EE_IP_FILTER) != FILTER_SET)
    return;
  for(uint8_t i = 0; i < IPFILTER_MAX; i++) {
    uint8_t *ee = EE_IP_FILTER + 1 + i * 5;
    uint8_t prefix = erb(ee + 4);
    if(prefix > 32)                   // unused
      continue;
    for(uint8_t j = 0; j < 4; j++)
      list[list_n].ip[j] = erb(ee + j);
    list[list_n++].prefix = prefix;
  }
}

void
ipfilter_store(const ipfilter_entry *e, uint8_t n)
{
  for(uint8_t i = 0; i < IPFILTER_MAX; i++) {
    uint8_t *ee = EE_IP_FILTER + 1 + i * 5;
    for(uint8_t j = 0; j < 4; j++)
      ewb(ee + j, i < n ? e[i].ip[j] : 0);
    ewb(ee + 4, i < n ? e[i].prefix : 0xff);
  }
  ewb(EE_IP_FILTER, n ? FILTER_SET : 0);
  ipfilter_load();
}

void
ipfilter_clear(void)
{
  ewb(EE_IP_FILTER, 0);
  list_n = 0;
}

uint8_t
ipfilter_get(ipfilter_entry *e)
{
  memcpy(e, list, list_n * sizeof(*e));
  return list_n;
}

uint8_t
ipfilter_match(const ipfilter_entry *e, uint8_t n, const uint8_t ip[4])
{
  if(!n)
    return 1;
  for(uint8_t i = 0; i < n; i++) {
    uint8_t bits = e[i].prefix, j = 0;
    for(; bits >= 8; j++, bits -= 8)
      if(ip[j] != e[i].ip[j])
        break;
    if(bits >= 8)
      continue;                       // a whole byte differed
    if(bits && ((ip[j] ^ e[i].ip[j]) & (0xff00 >> bits) & 0xff))
      continue;
    return 1;
  }
  return 0;
}

/* Called by uip_process(); srcipaddr is in network byte order. */
uint8_t
uip_ip_allowed(const void *srcipaddr)
{
  return ipfilter_match(list, list_n, srcipaddr);
}

static uint8_t
is_sep(char c)
{
  return c == ',' || c == ' ';
}

/* One decimal number up to max; advances *s. */
static uint8_t
parse_num(const char **s, const char *end, uint8_t max, uint8_t *v)
{
  uint16_t n = 0;
  uint8_t digits = 0;
  while(*s < end && **s >= '0' && **s <= '9' && digits < 3) {
    n = n * 10 + *(*s)++ - '0';
    digits++;
  }
  if(!digits || n > max)
    return 0;
  *v = n;
  return 1;
}

int8_t
ipfilter_parse(const char *s, uint16_t len, ipfilter_entry *e)
{
  const char *end = s + len;
  uint8_t n = 0;

  for(;;) {
    while(s < end && is_sep(*s))
      s++;
    if(s == end)
      return n;
    if(n == IPFILTER_MAX)
      return -1;

    ipfilter_entry *x = &e[n];
    for(uint8_t i = 0; i < 4; i++) {
      if(!parse_num(&s, end, 255, &x->ip[i]))
        return -1;
      if(i < 3 && (s == end || *s++ != '.'))
        return -1;
    }
    x->prefix = 32;
    if(s < end && *s == '/') {
      s++;
      if(!parse_num(&s, end, 32, &x->prefix))
        return -1;
    }
    if(s < end && !is_sep(*s))
      return -1;

    // host bits set (192.168.1.5/24) are cleared: the network is meant
    for(uint8_t i = 0; i < 4; i++) {
      int8_t bits = x->prefix - 8 * i;
      if(bits <= 0)
        x->ip[i] = 0;
      else if(bits < 8)
        x->ip[i] &= 0xff00 >> bits;
    }
    n++;
  }
}

void
ipfilter_read(void)
{
  if(!list_n)
    DS_P(PSTR("all"));
  for(uint8_t i = 0; i < list_n; i++) {
    if(i)
      DC(',');
    for(uint8_t j = 0; j < 4; j++) {
      if(j)
        DC('.');
      DU(list[i].ip[j], 1);
    }
    DC('/');
    DU(list[i].prefix, 1);
  }
}

/* Wif<list>; an empty list allows everyone. Takes effect at once, so over
   the network it can lock out the sender - USB still works. */
void
ipfilter_write(char *in)
{
  ipfilter_entry e[IPFILTER_MAX];
  int8_t n = ipfilter_parse(in + 3, strlen(in + 3), e);
  if(n < 0) {
    DS_P(PSTR("invalid list"));
    DNL();
    return;
  }
  ipfilter_store(e, n);
}

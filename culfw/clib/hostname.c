/*
 * Host name of the device (HAS_HOSTNAME).
 *
 * One DNS label (RFC 1123): letters, digits and '-', not first or last,
 * 1 to EE_HOSTNAME_SIZE - 1 characters. It is what the DHCP client sends as
 * option 12; most routers then resolve it and list the device by it.
 * Without a valid stored name the default is BOARD_NAME-XXXXXX, the last
 * three MAC bytes - distinct for each device.
 */

#include "board.h"

#ifdef HAS_HOSTNAME

#include <string.h>
#include <avr/pgmspace.h>

#include "display.h"
#include "fncollection.h"
#include "hostname.h"

static char name[EE_HOSTNAME_SIZE];

static uint8_t
is_label_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-';
}

uint8_t
hostname_valid(const char *s, uint16_t len)
{
  if(!len || len >= EE_HOSTNAME_SIZE || s[0] == '-' || s[len - 1] == '-')
    return 0;
  for(uint16_t i = 0; i < len; i++)
    if(!is_label_char(s[i]))
      return 0;
  return 1;
}

void
hostname_load(void)
{
  uint8_t len = 0;
  while(len < EE_HOSTNAME_SIZE - 1 &&
        (name[len] = erb(EE_HOSTNAME + len)) != 0)
    len++;
  name[len] = 0;
  if(hostname_valid(name, len))
    return;

  static const char hex[] = "0123456789abcdef";
  char *p = name;
  const char *b = BOARD_NAME;
  while(*b)
    *p++ = *b++;
  *p++ = '-';
  for(uint8_t i = 3; i < 6; i++) {
    uint8_t d = erb(EE_MAC_ADDR + i);
    *p++ = hex[d >> 4];
    *p++ = hex[d & 0xf];
  }
  *p = 0;
}

const char *
hostname_get(void)
{
  return name;
}

void
hostname_store(const char *s, uint8_t len)
{
  for(uint8_t i = 0; i < len; i++)
    ewb(EE_HOSTNAME + i, s[i]);
  ewb(EE_HOSTNAME + len, 0);          // len 0: no name, the default again
  hostname_load();
}

void
hostname_read(void)
{
  display_string(name);
}

/* Wih<name>; Wih alone returns to the default. The DHCP server sees the
   new name with the next lease, after a restart at the latest. */
void
hostname_write(char *in)
{
  uint16_t len = strlen(in + 3);
  if(len && !hostname_valid(in + 3, len)) {
    DS_P(PSTR("invalid name"));
    DNL();
    return;
  }
  hostname_store(in + 3, len);
}

#endif

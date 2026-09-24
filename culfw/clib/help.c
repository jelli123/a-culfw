/*
 * Short descriptions of the commands, for ?? and ?<letter> (HAS_HELP).
 *
 * Only commands in fntab are listed, so a build shows what it has. A bare ?
 * is not handled here: its "? (? is unknown) Use one of ..." answer stays as
 * it was, as FHEM takes the command letters from it.
 */

#include <avr/pgmspace.h>

#include "board.h"
#include "display.h"
#include "ttydata.h"
#include "help.h"
#ifdef USE_HW_AUTODETECT
#include "hw_autodetect.h"
#include "multi_CC.h"
#endif

typedef struct {
  char name;
  const char *text;
} t_help;

static const char h_A[] PROGMEM = "AskSin (HomeMatic): Ar receive, As<hex> send";
static const char h_B[] PROGMEM = "B00 restart, B01 restart into the bootloader";
static const char h_b[] PROGMEM = "wM-Bus: brs / brt receive S / T mode, bss<hex> / bst<hex> send";
static const char h_C[] PROGMEM = "CC1101 registers: C<reg> read, C99 all, Cw<reg><val> write (hex)";
static const char h_E[] PROGMEM = "RWE SmartHome: Er receive, Es<hex> send";
static const char h_e[] PROGMEM = "factory reset of the settings, then restart (ex: no restart)";
static const char h_F[] PROGMEM = "FS20: F<housecode 4><address 2><command 2>[<extension 2>] send (hex)";
static const char h_f[] PROGMEM = "FastRF: fr init, fs<data> send";
static const char h_G[] PROGMEM = "G<hex> raw send, see the culfw command reference";
static const char h_h[] PROGMEM = "Hoermann: h<hex> send";
static const char h_i[] PROGMEM = "Intertechno: is<bits> send (ish HomeEasy, ise HomeEasy EU), isr<n> repeats,\r\n"
                                  "   if<hex> frequency, ix frequency from the EEPROM";
static const char h_K[] PROGMEM = "K<hex> raw send (KS300 / S300)";
static const char h_k[] PROGMEM = "Kopp Free Control: ks<data> / kt<data> send";
static const char h_L[] PROGMEM = "Maico: Lr receive, Ls<hex> send";
static const char h_l[] PROGMEM = "LED D1: l00 off, l01 on, l02 heartbeat (kept in the EEPROM)";
static const char h_M[] PROGMEM = "M<hex> raw send (EM)";
static const char h_m[] PROGMEM = "free memory";
static const char h_N[] PROGMEM = "native mode: Nr<n> receive, Nx off";
static const char h_O[] PROGMEM = "OneWire: Oi init, Oc ROM codes, Or / Ow read / write, OH HMS emulation";
static const char h_R[] PROGMEM = "R<addr> read the EEPROM (hex). Network: Ria address, Rin netmask,\r\n"
                                  "   Rig gateway, Rid DHCP, Rim MAC, RiN NTP server, Rio NTP offset,\r\n"
                                  "   Rip TCP port"
#ifdef HAS_IP_FILTER
                                  ", Rif IP whitelist"
#endif
                                  ;
static const char h_T[] PROGMEM = "FHT: T<housecode 4><address 2><command 2>[<argument 2>] send (hex)";
static const char h_t[] PROGMEM = "time since start, in 1/125 s (hex)";
static const char h_U[] PROGMEM = "Uniroll: U<hex> send";
static const char h_V[] PROGMEM = "firmware version and board";
static const char h_W[] PROGMEM = "W<addr><val> write the EEPROM (hex). Network: Wia / Win / Wig <ip>,\r\n"
                                  "   Wid<0|1> DHCP, Wim<mac>, WiN<ip> NTP server, Wio<hex> NTP offset,\r\n"
                                  "   Wip<port>; they apply after a restart (B00)"
#ifdef HAS_IP_FILTER
                                  ".\r\n   Wif<list> IP whitelist, at once (Wif alone clears it)"
#endif
                                  ;
static const char h_X[] PROGMEM = "X<hex> reception reports (X21 in FHEM); X alone: setting and duty cycle credit";
static const char h_x[] PROGMEM = "x<hex> CC1101 output power";
static const char h_Y[] PROGMEM = "Somfy RTS: Ys<hex> send, Yr<n> repeats, Yt<us> symbol width,\r\n"
                                  "   Yx frequency from the EEPROM";
static const char h_Z[] PROGMEM = "MAX!: Zr receive, Zs<hex> send, Zf<hex> send fast, Za<addr> auto-ack,\r\n"
                                  "   Zw<addr> fake wall thermostat, Zx off";
static const char h_z[] PROGMEM = "Z-Wave: zr receive, zm monitor, zs<hex> send, zi<home id><node id> set IDs";
static const char h_star[] PROGMEM = "*<cmd> the command for the next radio module (**<cmd> the one after)";

static const t_help help_tab[] PROGMEM = {
  { 'A', h_A }, { 'B', h_B }, { 'b', h_b }, { 'C', h_C }, { 'E', h_E },
  { 'e', h_e }, { 'F', h_F }, { 'f', h_f }, { 'G', h_G }, { 'h', h_h },
  { 'i', h_i }, { 'K', h_K }, { 'k', h_k }, { 'L', h_L }, { 'l', h_l },
  { 'M', h_M }, { 'm', h_m }, { 'N', h_N }, { 'O', h_O }, { 'R', h_R },
  { 'T', h_T }, { 't', h_t }, { 'U', h_U }, { 'V', h_V }, { 'W', h_W },
  { 'X', h_X }, { 'x', h_x }, { 'Y', h_Y }, { 'Z', h_Z }, { 'z', h_z },
  { '*', h_star },
  { 0, 0 }
};

extern const PROGMEM t_fntab fntab[];

/* In fntab, and the hardware for it found - as callfn(0) lists it. */
static uint8_t
available(char n)
{
#ifdef USE_HW_AUTODETECT
  if((n == '*' && !has_CC(CC1101.instance+1)) || (n == 'O' && !has_onewire()))
    return 0;
#endif
  for(uint8_t idx = 0; ; idx++) {
    char f = __LPM(&fntab[idx].name);
    if(!f)
      return 0;
    if(f == n)
      return 1;
  }
}

static void
show(char n, const char *text)
{
  DC(n);
  DS_P(PSTR("  "));
  DS_P(text);
  DNL();
}

void
help(const char *in)
{
  uint8_t all = (in[0] == '?');

  for(uint8_t idx = 0; ; idx++) {
    char n = __LPM(&help_tab[idx].name);
    if(!n)
      break;
    if((all || n == in[0]) && available(n)) {
      show(n, (const char *)__LPM_word(&help_tab[idx].text));
      if(!all)
        return;
    }
  }
  if(all) {
    DS_P(PSTR("?<letter> for one command"));
  } else {
    DS_P(PSTR("? no help for "));
    DC(in[0]);
  }
  DNL();
}

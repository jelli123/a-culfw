/*
 * Minimal configuration web page for the Ethernet devices.
 *
 * GET  /        shows the network settings stored in the EEPROM
 * POST /        validates and stores them, then restarts the device
 * POST /reboot  restarts the device
 *
 * One request is served at a time. The page is rendered into a static
 * buffer and sent in MSS sized pieces from there, so a retransmission can
 * send the same bytes again - uIP keeps no copy of sent data. A second
 * request arriving meanwhile gets a 503, answered from flash alone.
 *
 * Once a password is set, every request needs HTTP Basic authentication as
 * user "admin". The EEPROM keeps only a salted SHA-256 hash: any EEPROM
 * byte can be read with the R command over USB or the TCP port. Five wrong
 * passwords in a row lock the page for 30 seconds. Holding the button
 * (HTTPD_RESET_PIN) for 10 seconds while running removes the password and
 * the IP whitelist, as does the e factory reset.
 *
 * With HAS_IP_FILTER the page also edits the whitelist (clib/ipfilter.c).
 * A list that would shut out the computer saving it is refused.
 *
 * This protects the page, not the device: the TCP port takes every command,
 * W included, without a password, and HTTP carries the password in clear.
 * A POST whose Origin header names another host is refused as well, so a
 * web page elsewhere cannot make the browser reconfigure the device.
 */

#include <string.h>
#include <avr/eeprom.h>

#include "board.h"
#include "uip.h"
#include "timer.h"
#include "clock.h"
#include "fncollection.h"
#include "sha256.h"
#include "version.h"
#include "httpd.h"
#ifdef HAS_NTP
#include "ntp.h"
#endif
#ifdef HTTPD_RESET_PIN
#include "led.h"
#endif
#ifdef HAS_IP_FILTER
#include "ipfilter.h"
#endif
#ifdef HAS_FW_UPDATE
#include "fwupdate.h"
#endif
#ifdef HAS_HOSTNAME
#include "hostname.h"
#include "apps/dhcpc/dhcpc.h"
#endif
#ifdef USE_RF_MODE
#include "cc1100.h"
#include "fband.h"
#include "rf_mode.h"
#include "rf_send.h"
#ifdef USE_HW_AUTODETECT
#include "hw_autodetect.h"
#endif
#endif

#define RX_SIZE       1024            // request line, headers and body
#define TX_SIZE       8192            // the complete response, 5-6 KB now
#define REQ_TIMEOUT   (CLOCK_SECOND * 10)
#define REBOOT_DELAY  (CLOCK_SECOND * 3)

// EE_HTTPD_AUTH: marker, salt, first bytes of SHA-256(salt | password)
#define AUTH_SET      0xA5            // any other marker: no password
#define AUTH_SALT     4
#define AUTH_HASH     20
#define AUTH_USER     "admin"
#define AUTH_PW_MAX   32
#define AUTH_TRIES    5
#define AUTH_LOCK     (CLOCK_SECOND * 30)

#define RESET_HOLD    40              // httpd_periodic calls, 4 per second
#define RESET_BLINK   24

#define DUTY_MAX_MIN  60              // longest suspension of the 1 % limit
#define DUTY_DEF_MIN  5

static struct uip_conn *owner;        // the connection being served
static struct timer req_timer;

static char rx[RX_SIZE + 1];
static uint16_t rx_len;

static char tx[TX_SIZE];
static uint16_t tx_len;               // rendered bytes
static uint16_t tx_pos;               // acknowledged bytes
static uint16_t tx_chunk;             // bytes in flight

static uint8_t reboot_pending;
static struct timer reboot_timer;

static uint8_t auth_fails;
static struct timer auth_lock;

/* Basic authentication has no logout: the browser keeps sending what it
   was given. After "Log out", the next request with credentials from that
   address gets a 401 once, whatever it carries; the browser then drops
   them and asks again. */
static uint8_t logout_pending;
static uint8_t factory_pending;       // after the answer went out
static uint16_t logout_ip[2];

#ifdef HAS_FW_UPDATE
static uint8_t uploading;             // the owner's body goes to fwupdate
static uint32_t upload_left;
static uint8_t install_pending;
static struct timer install_timer;
#define INSTALL_DELAY (CLOCK_SECOND * 2)   // the answer goes out first
#endif

static const char busy[] =
  "HTTP/1.0 503 Service Unavailable\r\n"
  "Connection: close\r\nRetry-After: 1\r\n\r\n";

/* ------------------------------------------------------------------------
 * Output
 */

static void
out(const char *s)
{
  while(*s && tx_len < TX_SIZE)
    tx[tx_len++] = *s++;
}

static void
out_u(uint16_t v)
{
  char b[6];
  uint8_t i = sizeof(b);
  b[--i] = 0;
  do {
    b[--i] = '0' + v % 10;
    v /= 10;
  } while(v);
  out(b + i);
}

static void
out_u32(uint32_t v)
{
  char b[11];
  uint8_t i = sizeof(b);
  b[--i] = 0;
  do {
    b[--i] = '0' + v % 10;
    v /= 10;
  } while(v);
  out(b + i);
}

static void
out_ip(uint8_t *ee)
{
  for(uint8_t i = 0; i < 4; i++) {
    if(i)
      out(".");
    out_u(erb(ee + i));
  }
}

static void
out_ip_bytes(const uint8_t *ip)
{
  for(uint8_t i = 0; i < 4; i++) {
    if(i)
      out(".");
    out_u(ip[i]);
  }
}

static void
out_mac(void)
{
  static const char hex[] = "0123456789ABCDEF";
  char b[3] = { 0, 0, 0 };
  for(uint8_t i = 0; i < 6; i++) {
    uint8_t d = erb(EE_MAC_ADDR + i);
    if(i)
      out(":");
    b[0] = hex[d >> 4];
    b[1] = hex[d & 0xf];
    out(b);
  }
}

static void
out_header(const char *status)
{
  out("HTTP/1.0 ");
  out(status);
  out("\r\nContent-Type: text/html; charset=utf-8\r\n"
      "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
}

/* refresh_ip: 0 no reload, "" this page after 10 s, else that address.
   Used by the pages answering a POST that restarts the device; they also
   put / into the address bar at once. Otherwise reloading re-sends the
   POST: after the restart, with a password set, that was a 401, the
   browser asking for the password and sending the form again with it -
   and the device saving and restarting again. */
static void
out_head(const char *refresh_ip)
{
  out("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  if(refresh_ip) {
    out("<meta http-equiv=\"refresh\" content=\"10;url=");
    if(*refresh_ip) {
      out("http://");
      out(refresh_ip);
    }
    out("/\"><script>history.replaceState(null,'','/')</script>");
  }
  out("<title>");
#ifdef HAS_HOSTNAME
  out(hostname_get());
#else
  out(BOARD_NAME);
#endif
  out("</title><link rel=\"stylesheet\" href=\"/s.css?" VERSION "\">"
      "</head><body><header><h1>" BOARD_NAME "</h1><div class=\"i\">");
#ifdef HAS_HOSTNAME
  out(hostname_get());
  out(" &middot; ");
#endif
#ifdef FW_IMAGE_ID
  out(FW_NAME " " VERSION " &middot; " FW_IMAGE_ID "</div>");
#else
  out(FW_NAME " " VERSION "</div>");
#endif
  if(refresh_ip == 0 && erb(EE_HTTPD_AUTH) == AUTH_SET)
    out("<form method=\"post\" action=\"/logout\">"
        "<button class=\"s\">Log out</button></form>");
  out("</header>");
}

/* The stylesheet, cached by the browser: its URL carries the version. */
static const char css[] =
  ":root{--bg:#f3f4f6;--card:#fff;--fg:#1c2230;--mute:#667085;"
  "--line:#dde1e7;--acc:#2563c9;--err:#fdeceb;--errl:#d64545}"
  "@media(prefers-color-scheme:dark){:root{--bg:#14171c;--card:#1d2128;"
  "--fg:#e6e8eb;--mute:#98a2b3;--line:#2e3440;--acc:#6699f0;"
  "--err:#3b2020}}"
  "*{box-sizing:border-box}"
  "body{margin:0;font:15px/1.45 system-ui,-apple-system,'Segoe UI',Roboto,"
  "sans-serif;background:var(--bg);color:var(--fg)}"
  "header{display:flex;align-items:center;gap:.4em 1em;flex-wrap:wrap;"
  "padding:.7em 16px;background:var(--card);border-bottom:1px solid var(--line)}"
  "header h1{margin:0;font-size:1.2em}header .i{flex:1}"
  "header form button{margin:0}"
  "nav{display:flex;overflow-x:auto;padding:0 8px;background:var(--card);"
  "border-bottom:1px solid var(--line)}"
  "nav a{padding:.7em .9em;color:var(--mute);text-decoration:none;"
  "white-space:nowrap;border-bottom:2px solid transparent}"
  "nav a.on{color:var(--acc);border-color:var(--acc)}"
  "main{max-width:44em;margin:0 auto;padding:16px}"
  "section{background:var(--card);border:1px solid var(--line);"
  "border-radius:10px;padding:.1em 1.2em 1.2em;margin-bottom:16px}"
  ".js section{display:none}.js section.on{display:block}"
  "h2{font-size:1.05em;margin:1.1em 0 .5em}"
  "label{display:block;margin:.8em 0 .25em;font-weight:500}"
  "label.c{display:flex;gap:.5em;align-items:center;font-weight:400}"
  "input:not([type=checkbox]){width:100%;padding:.5em .6em;font:inherit;"
  "color:inherit;background:var(--bg);border:1px solid var(--line);"
  "border-radius:6px}"
  "input:focus{outline:2px solid var(--acc);outline-offset:-1px}"
  "button{margin-top:1em;padding:.55em 1.2em;font:inherit;color:#fff;"
  "background:var(--acc);border:1px solid var(--acc);border-radius:6px;"
  "cursor:pointer}"
  "button.s{color:var(--acc);background:transparent}"
  "button:disabled{opacity:.4;cursor:default}"
  "table{width:100%;border-collapse:collapse}"
  "td,th{padding:.4em .8em .4em 0;text-align:left;vertical-align:top;"
  "border-bottom:1px solid var(--line)}"
  "th{color:var(--mute);font-weight:500}"
  "td:first-child{color:var(--mute);white-space:nowrap}"
  ".i{color:var(--mute);font-size:.9em}"
  ".e{padding:.6em .8em;background:var(--err);border-left:4px solid "
  "var(--errl);border-radius:6px}"
  "progress{width:100%;height:.8em;margin-top:1em}"
  "summary{margin-top:.8em;color:var(--acc);cursor:pointer}";

static void
page_css(void)
{
  out("HTTP/1.0 200 OK\r\nContent-Type: text/css\r\n"
      "Cache-Control: max-age=604800\r\nConnection: close\r\n\r\n");
  out(css);
}

static void
out_field(const char *label, char name, uint8_t *ee_ip)
{
  char n[2] = { name, 0 };
  out("<label for=\"");
  out(n);
  out("\">");
  out(label);
  out("</label><input type=\"text\" id=\"");
  out(n);
  out("\" name=\"");
  out(n);
  out("\" value=\"");
  if(ee_ip)
    out_ip(ee_ip);
}

#ifdef HAS_NTP
static void
out_2(uint8_t v)
{
  if(v < 10)
    out("0");
  out_u(v);
}

/* The time from NTP in the time zone set (whole hours, no daylight saving
   time), as ntp_sec2tm() computes it for the log. */
static void
out_time(void)
{
  if(!ntp_synced) {
    out("no answer from the NTP server yet");
  } else {
    tm_t t;
    ntp_sec2tm(ntp_sec, &t);
    out("20");      out_2(t.tm_year);
    out("-");       out_2(t.tm_mon);
    out("-");       out_2(t.tm_mday);
    out(" ");       out_2(t.tm_hour);
    out(":");       out_2(t.tm_min);
    out(":");       out_2(t.tm_sec);
    out(ntp_gmtoff < 0 ? " (UTC-" : " (UTC+");
    out_u(ntp_gmtoff < 0 ? -ntp_gmtoff : ntp_gmtoff);
    out(")");
  }
}
#endif

/* n / 10^decimals, with that many decimals */
static void
out_fixed(uint32_t n, uint8_t decimals)
{
  uint16_t div = 1;
  for(uint8_t i = 0; i < decimals; i++)
    div *= 10;
  out_u(n / div);
  out(".");
  for(uint16_t d = div / 10; d; d /= 10)
    out_u(n / d % 10);
}

#ifdef SAM7
/* from CUBE*_flash.lds */
extern char _sfixed[], _efixed[], _srelocate[], _erelocate[], _ezero[];
extern char _flash_end[], _sstack[];

static void
out_kb(uint32_t bytes)
{
  out_fixed((bytes * 10 + 512) / 1024, 1);
  out(" KB");
}

/* The image is .fixed plus the initial values of .relocate behind it. */
static void
out_memory(void)
{
  uint32_t area = _flash_end - _sfixed;
  uint32_t image = (_efixed - _sfixed) + (_erelocate - _srelocate);
  uint32_t ram = _sstack - _srelocate;
  uint32_t ram_static = _ezero - _srelocate;

  out("<h2>Memory</h2><table><tr><td>Flash</td><td>firmware ");
  out_kb(image);
  out(" of ");
  out_kb(area);
  out(", free ");
  out_kb(area - image);
  out("</td></tr>");
#ifdef USE_DATAFLASH
  uint16_t pages, page_size, reserved;
  const char *name = dataflash_info(&pages, &page_size, &reserved);
  out("<tr><td>Dataflash</td><td>");
  if(!name) {
    out("none found");
  } else {
    out(name);
    out(", ");
    out_kb((uint32_t)pages * page_size);
    out(" (");
    out_u(pages);
    out(" pages of ");
    out_u(page_size);
    out(" bytes); pages 0-");
    out_u(reserved - 1);
    out(" set aside, the settings in page ");
    out_u(reserved - 1);
    out(", free ");
    out_kb((uint32_t)(pages - reserved) * page_size);
  }
  out("</td></tr>");
#endif
  out("<tr><td>RAM</td><td>static ");
  out_kb(ram_static);
  out(" of ");
  out_kb(ram);
  out(", the rest is stack</td></tr></table>");
}
#endif

#ifdef USE_RF_MODE
#ifdef HAS_MULTI_CC
#define RADIO_COUNT HAS_MULTI_CC
#else
#define RADIO_COUNT 1
#endif

/* In RF_mode_t order. */
static const char * const mode_name[] = {
  "off", "SlowRF (FS20, FHT, ...)", "AskSin (HomeMatic)", "MAX!",
  "Wireless M-Bus S", "Wireless M-Bus T", "Maico", "native 1", "native 2",
  "native 3", "Somfy RTS", "Intertechno", "RWE", "FastRF", "Z-Wave"
};

static uint8_t
radio_present(uint8_t i)
{
#ifdef USE_HW_AUTODETECT
  return has_CC(i);
#else
  return i < RADIO_COUNT;
#endif
}

static const char *
marc_state(uint8_t s)
{
  s &= 0x1f;
  if(s == 0x01)               return "idle";
  if(s >= 0x0d && s <= 0x0f)  return "receiving";
  if(s >= 0x13 && s <= 0x15)  return "transmitting";
  if(s == 0x11)               return "RX overflow";
  if(s == 0x16)               return "TX underflow";
  return "busy";              // calibrating, settling, ...
}

/* One row per module found. Frequency and state are read from the chip:
   every mode programs its own frequency. */
static void
out_radios(void)
{
  out("<h2>Radio modules</h2><table><tr><th>#</th><th>Band</th>"
      "<th>Frequency</th><th>Mode</th><th>State</th></tr>");
  uint8_t old = CC1101.instance;
  for(uint8_t i = 0; i < RADIO_COUNT; i++) {
    if(!radio_present(i))
      continue;
    CC1101.instance = i;
    uint32_t f = (uint32_t)cc1100_readReg(CC1100_FREQ2) << 16 |
                 (uint32_t)cc1100_readReg(CC1100_FREQ1) << 8 |
                 cc1100_readReg(CC1100_FREQ0);
    uint8_t state = cc1100_readReg(CC1100_MARCSTATE);
    CC1101.instance = old;

    uint8_t band = CC1101.frequencyMode[i];
    RF_mode_t mode = CC1101.RF_mode[i];
    out("<tr><td>");
    out_u(i);
    out("</td><td>");
    out(band == MODE_433_MHZ ? "433 MHz" : band == MODE_868_MHZ ? "868 MHz" : "?");
    out("</td><td>");
    out_fixed(((uint64_t)f * 26000 + 0x8000) >> 16, 3); // 26 MHz crystal, kHz
    out(" MHz</td><td>");
    out(mode < sizeof(mode_name) / sizeof(*mode_name) ? mode_name[mode] : "?");
    out("</td><td>");
    out(mode == RF_mode_off ? "off" : marc_state(state));
    out("</td></tr>");
  }
  out("</table>");
}

/* The duty cycle budget (credit_10ms) and the form to suspend it. */
static void
out_duty(void)
{
  out("<h2>Duty cycle (1 % rule)</h2><p>Budget: ");
  out_fixed(credit_10ms, 2);
  out(" s of ");
  out_fixed(MAX_CREDIT, 2);
  out(" s air time. It refills by 10 ms per second, i.e. 1 % of the time, "
      "and is shared by all modules. SlowRF, MAX! and Maico transmissions "
      "draw on it; the firmware does not limit the other modes.</p>");

  if(credit_suspend_s) {
    out("<p class=\"e\">The limit is suspended for another ");
    out_u(credit_suspend_s / 60);
    out(":");
    out_u(credit_suspend_s % 60 / 10);
    out_u(credit_suspend_s % 10);
    out(" min.</p><form method=\"post\" action=\"/duty\">"
        "<input type=\"hidden\" name=\"m\" value=\"0\">"
        "<button>Enforce the limit again</button></form>");
    return;
  }
  // novalidate: the browser would complain in its own language; the
  // server checks both fields anyway (handle_duty)
  out("<details><summary>Suspend the limit for debugging</summary>"
      "<form method=\"post\" action=\"/duty\" novalidate>"
      "<label for=\"m\">Minutes (1-");
  out_u(DUTY_MAX_MIN);
  out(")</label><input type=\"number\" id=\"m\" name=\"m\" min=\"1\" max=\"");
  out_u(DUTY_MAX_MIN);
  out("\" value=\"");
  out_u(DUTY_DEF_MIN);
  out("\"><p class=\"e\">Many bands allow only a limited duty cycle - in "
      "the EU, for instance, 1 % in 868.0-868.6 MHz (ERC Recommendation "
      "70-03, EN 300 220). Transmitting beyond it can break the law and "
      "disturbs other users of the band. You are responsible for observing "
      "the radio regulations of the country the device is operated in. "
      "The suspension ends by itself and with every restart. It fills the "
      "budget, and nothing is taken from it meanwhile.</p>"
      "<label class=\"c\"><input type=\"checkbox\" name=\"c\" value=\"1\" required> "
      "I will observe the radio regulations that apply here</label>"
      "<button>Suspend the limit</button></form></details>");
}
#endif

#ifdef HAS_FW_UPDATE
static void
out_hex32(uint32_t v)
{
  static const char hex[] = "0123456789abcdef";
  char b[9];
  for(uint8_t i = 0; i < 8; i++)
    b[i] = hex[(v >> (28 - 4 * i)) & 0xf];
  b[8] = 0;
  out(b);
}

/* The upload goes out as the raw file (XMLHttpRequest, for the progress
   bar) to POST /update, which answers in plain text; POST /install then
   copies it. Only with a password: the update can install anything. */
static void
out_update(void)
{
  out("<h2>Firmware update</h2>");
  if(erb(EE_HTTPD_AUTH) != AUTH_SET) {
    out("<p class=\"i\">Set a password for this page first: without one, "
        "anyone on the network could install firmware.</p>");
    return;
  }
  out("<p class=\"i\">Accepted: " FW_IMAGE_ID " images"
#ifdef FW_IMAGE_ALT
      ", and " FW_IMAGE_ALT " after a question - both keep the settings at "
      "the same place and detect the radio modules at start"
#endif
      ". Should an update be cut short, the device starts in the "
      "bootloader's USB drive by itself.</p>"
      "<form id=\"uf\"><input type=\"file\" id=\"ff\" accept=\".bin\">"
      "<button>Upload and check</button></form>"
      "<progress id=\"up\" max=\"1\" value=\"0\" hidden></progress>"
      "<p id=\"us\"></p>"
      "<form method=\"post\" action=\"/install\" id=\"ui\" hidden>"
      "<button>Install and restart</button></form>"
      "<script>"
      "function $(i){return document.getElementById(i)}"
      /* The image id is looked up in the file first: a wrong variant or
         an image without update support is refused at once, instead of
         after the whole upload. The device checks it again. */
      "$('uf').onsubmit=function(e){e.preventDefault();"
      "var f=$('ff').files[0],s=$('us'),r=new FileReader();if(!f)return;"
      "var k=$('uf').querySelector('button');k.disabled=1;"
      "$('ui').hidden=1;s.textContent='Reading the file...';"
      "r.onload=function(){var b=new Uint8Array(r.result),"
      "p='a-culfw-image:',n=p.length,i,j,id='';"
      "for(i=0;i+n<b.length&&!id;i++){"
      "for(j=0;j<n&&b[i+j]==p.charCodeAt(j);j++);"
      /* this script is in every image as well: only a name closed by ';'
         counts, not the literal above */
      "if(j==n){for(j=i+n;j<i+n+24&&b[j]!=59;j++)id+=String.fromCharCode(b[j]);"
      "if(b[j]!=59||!/^\\w+$/.test(id))id=''}}"
      "if(id!='" FW_IMAGE_ID "'"
#ifdef FW_IMAGE_ALT
      "&&!(id=='" FW_IMAGE_ALT "'&&confirm('This is a '+id+' image; this "
      "device runs " FW_IMAGE_ID ". Both keep the settings at the same place "
      "and detect the radio modules at start. Change to '+id+'?'))"
#endif
      "){s.textContent=id?'This is a '+id+"
      "' image; this device runs " FW_IMAGE_ID ".':"
      "'This file is no a-culfw image with update support.';k.disabled=0;return}"
      "up(f)};r.readAsArrayBuffer(f)};"
      "function up(f){"
      "var x=new XMLHttpRequest(),p=$('up'),s=$('us'),"
      "k=$('uf').querySelector('button');x.onloadend=function(){k.disabled=0};"
      "p.hidden=0;p.value=0;s.textContent='Uploading...';"
      "x.upload.onprogress=function(e){p.value=e.loaded/e.total;"
      "if(e.loaded==e.total)s.textContent='Sent; the device is still "
      "writing and checking it...'};"
      "x.onload=function(){p.hidden=1;s.textContent=x.responseText;"
      "$('ui').hidden=x.status!=200};"
      "x.onerror=function(){p.hidden=1;s.textContent='The connection to the "
      "device broke off during the upload. Nothing was installed.'};"
      "x.open('POST','/update');x.send(f)}"
      "</script>");
}

#endif

static void
out_ip_u16(const u16_t *ip)
{
  out_ip_bytes((const uint8_t *)ip);
}

/* Device, address, time; the radio modules and the duty cycle. */
static void
out_overview(void)
{
  out("<section id=\"t-ov\"><h2>Device</h2><table>"
      "<tr><td>Board</td><td>" BOARD_ID_STR "</td></tr>"
      "<tr><td>Firmware</td><td>" FW_NAME " " VERSION
#ifdef FW_IMAGE_ID
      ", " FW_IMAGE_ID
#endif
      "<br><span class=\"i\">");
#if defined(USE_RF_MODE) && defined(HAS_MULTI_CC)
  uint8_t found = 0;
  for(uint8_t i = 0; i < RADIO_COUNT; i++)
    found += radio_present(i) ? 1 : 0;
  out("for up to ");
  out_u(RADIO_COUNT);
  out(" radio modules, detected at start: ");
  out_u(found);
  out(" found");
#else
  out("for one radio module");
#endif
  out("</span></td></tr>");
#ifdef HAS_HOSTNAME
  out("<tr><td>Host name</td><td>");
  out(hostname_get());
  out("</td></tr>");
#endif
  out("<tr><td>Address</td><td>");
  out_ip_u16(uip_hostaddr);
  out(erb(EE_USE_DHCP) ? " (DHCP)" : " (static)");
  out("</td></tr><tr><td>Gateway</td><td>");
  out_ip_u16(uip_draddr);
  out("</td></tr><tr><td>MAC</td><td>");
  out_mac();
  out("</td></tr>");
#ifdef HAS_NTP
  out("<tr><td>NTP server</td><td>");
  if(ntp_conn) {
    out_ip_u16(ntp_conn->ripaddr);
    out(ntp_source == NTP_FROM_DHCP ? " (from DHCP)" :
        ntp_source == NTP_FROM_GATEWAY ? " (the gateway)" : "");
  } else {
    out("none yet");
  }
  out("</td></tr><tr><td>Time</td><td>");
  out_time();
  out("</td></tr>");
#endif
  out("</table>");
#ifdef USE_RF_MODE
  out_radios();
  out_duty();
#endif
  out("</section>");
}

/* tab: the section to show first, 0 for the overview */
static void
page_config(const char *error, const char *tab)
{
  int8_t off = (int8_t)erb(EE_IP4_NTPOFFSET);

  out_header(error ? "400 Bad Request" : "200 OK");
  out_head(0);
  out("<nav><a href=\"#t-ov\">Overview</a><a href=\"#t-net\">Network</a>"
      "<a href=\"#t-acc\">Access</a><a href=\"#t-sys\">System</a></nav>"
      "<main data-t=\"");
  out(tab ? tab : "t-ov");
  out("\">");
  if(error) {
    out("<p class=\"e\">");
    out(error);
    out(" Nothing was saved.</p>");
  }
  out_overview();
  out("<form method=\"post\" action=\"/\"><section id=\"t-net\">"
      "<h2>Network</h2>"
      "<label class=\"c\"><input type=\"checkbox\" name=\"d\" value=\"1\"");
  if(erb(EE_USE_DHCP))
    out(" checked");
  out("> DHCP (the addresses below are then the last lease)</label>");
#ifdef HAS_HOSTNAME
  out_field("Host name (letters, digits, '-'; empty: the default)", 'h', 0);
  out(hostname_get());
  out("\" maxlength=\"23\">");
  if(erb(EE_USE_DHCP)) {
    const char *dn = dhcpc_assigned_name(), *dd = dhcpc_assigned_domain();
    out("<p class=\"i\">Sent to the DHCP server, which lists the device by "
        "it. ");
    if(*dn || *dd) {
      out("The server answered with ");
      out(*dn ? dn : hostname_get());
      if(*dd) {
        out(".");
        out(dd);
      }
      out(".");
    } else {
      out("The server sent back no name or domain.");
    }
    out("</p>");
  }
#endif
  out_field("IP address", 'a', EE_IP4_ADDR);             out("\">");
  out_field("Netmask", 'n', EE_IP4_NETMASK);             out("\">");
  out_field("Gateway", 'g', EE_IP4_GATEWAY);             out("\">");
  out_field("NTP server (0.0.0.0: from DHCP, else the gateway)", 'N',
            EE_IP4_NTPSERVER);
  out("\">");
  out_field("TCP port (CUL protocol)", 'p', 0);
  out_u(eeprom_read_word((uint16_t *)EE_IP4_TCPLINK_PORT));
  out("\">");
  out_field("Time zone, hours from UTC", 'o', 0);
  if(off < 0) {
    out("-");
    out_u(-off);
  } else {
    out_u(off);
  }
  out("\"><button>Save and restart</button></section>"
      "<section id=\"t-acc\">");
#ifdef HAS_IP_FILTER
  ipfilter_entry e[IPFILTER_MAX];
  uint8_t n = ipfilter_get(e);
  out("<h2>Allowed clients</h2><label for=\"f\">Addresses or networks "
      "(192.168.1.0/24, 10.0.0.5), at most 4. Others get no answer at all, "
      "on any port. Empty: everyone.</label>"
      "<input type=\"text\" id=\"f\" name=\"f\" value=\"");
  for(uint8_t i = 0; i < n; i++) {
    if(i)
      out(", ");
    out_ip_bytes(e[i].ip);
    if(e[i].prefix != 32) {
      out("/");
      out_u(e[i].prefix);
    }
  }
  out("\"><p class=\"i\">This computer: ");
  out_ip_bytes((const uint8_t *)uip_conn->ripaddr);
  out("</p>");
#endif
  out("<h2>Password for this page</h2>");
  if(erb(EE_HTTPD_AUTH) == AUTH_SET)
    out("<p class=\"i\">User name: " AUTH_USER ". Holding the button on the "
        "bottom for 10 seconds removes the password.</p>"
        "<label class=\"c\"><input type=\"checkbox\" name=\"x\" value=\"1\"> "
        "Remove the password</label>");
  else
    out("<p class=\"e\">No password is set: anyone on the network can "
        "change these settings.</p>");
  out("<label for=\"w\">New password (empty: unchanged)</label>"
      "<input type=\"password\" id=\"w\" name=\"w\" maxlength=\"32\" "
      "autocomplete=\"new-password\">"
      "<label for=\"r\">Repeat the new password</label>"
      "<input type=\"password\" id=\"r\" name=\"r\" maxlength=\"32\" "
      "autocomplete=\"new-password\">"
      "<button>Save and restart</button></section></form>"
      "<section id=\"t-sys\">");
#ifdef SAM7
  out_memory();
#endif
  out("<h2>Restart</h2><form method=\"post\" action=\"/reboot\">"
      "<button class=\"s\">Restart the device</button></form>"
      "<h2>Factory reset</h2>"
      "<form method=\"post\" action=\"/factory\" novalidate>"
      "<p class=\"i\">Sets everything back to the defaults and restarts: "
      "radio settings, DHCP with the default addresses, TCP port 2323, "
      "no password, no allowed-clients list, the default host name. "
      "A device with a static address may then answer at another one.</p>"
      "<label class=\"c\"><input type=\"checkbox\" name=\"c\" value=\"1\">"
      " Reset all settings</label>"
      "<button class=\"s\">Factory reset</button></form>");
#ifdef HAS_FW_UPDATE
  out_update();
#endif
  /* Tabs: without JavaScript every section shows, one below the other. */
  out("</section></main><script>(function(){var d=document,b=d.body,"
      "t=d.querySelectorAll('nav a');b.className='js';"
      "function s(){var h=location.hash.slice(1),f=0,i,o;"
      "for(i=0;i<t.length;i++)if(t[i].hash.slice(1)==h)f=1;"
      "if(!f)h=d.querySelector('main').getAttribute('data-t');"
      "for(i=0;i<t.length;i++){o=t[i].hash.slice(1)==h;"
      "t[i].className=o?'on':'';"
      "d.getElementById(t[i].hash.slice(1)).className=o?'on':''}}"
      "onhashchange=s;s()})()</script></body></html>");
}

static void
page_restart(const char *new_ip)
{
  out_header("200 OK");
  out_head(new_ip ? new_ip : "");     // DHCP: the same address, most likely
  out("<main><section><p>Restarting&hellip; The page reloads by itself.</p>");
  if(new_ip) {
    out("<p>The device will answer at <a href=\"http://");
    out(new_ip);
    out("/\">");
    out(new_ip);
    out("</a>.</p>");
  }
  out("</section></main></body></html>");
}

static void
page_error(const char *status)
{
  out_header(status);
  out("<!DOCTYPE html><title>");
  out(status);
  out("</title><p>");
  out(status);
  out("</p>");
}

static void
page_redirect(void)
{
  out("HTTP/1.0 303 See Other\r\nLocation: /#t-ov\r\n"
      "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
}

static void
page_unauthorized(void)
{
  out("HTTP/1.0 401 Unauthorized\r\n"
      "WWW-Authenticate: Basic realm=\"" BOARD_NAME "\", charset=\"UTF-8\"\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
      "<!DOCTYPE html><title>401 Unauthorized</title>"
      "<p>401 Unauthorized. Holding the button on the bottom of the device "
      "for 10 seconds removes the password.</p>");
}

/* ------------------------------------------------------------------------
 * Request parsing
 */

static uint8_t
lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? c + 'a' - 'A' : c;
}

/* The value of header 'name' (lower case, with the colon), or 0. */
static const char *
header(const char *name, const char *end)
{
  uint8_t n = strlen(name);
  const char *p = strstr(rx, "\r\n");

  while(p && p + 2 + n <= end) {
    p += 2;
    uint8_t i;
    for(i = 0; i < n && lower(p[i]) == name[i]; i++)
      ;
    if(i == n) {
      p += n;
      while(*p == ' ')
        p++;
      return p;
    }
    p = strstr(p, "\r\n");
  }
  return 0;
}

static uint16_t
value_len(const char *v)
{
  uint16_t n = 0;
  while(v[n] && v[n] != '\r')
    n++;
  return n;
}

/* A POST from a page served by someone else carries an Origin header that
   does not match "http://" + Host. Browsers always send Origin on POST. */
static uint8_t
foreign_origin(const char *end)
{
  const char *o = header("origin:", end);
  const char *h = header("host:", end);
  if(!o)
    return 0;
  if(!h || strncmp(o, "http://", 7))
    return 1;
  uint16_t hl = value_len(h);
  return value_len(o) != hl + 7 || strncmp(o + 7, h, hl);
}

/* The value of form field 'key' in the body, or 0. */
static const char *
field(const char *body, char key, uint16_t *len)
{
  const char *p = body;
  while(*p) {
    const char *e = strchr(p, '&');
    if(!e)
      e = p + strlen(p);
    if(p[0] == key && p[1] == '=') {
      *len = e - p - 2;
      return p + 2;
    }
    p = *e ? e + 1 : e;
  }
  return 0;
}

static uint8_t
parse_uint(const char *v, uint16_t len, uint16_t max, uint16_t *out_v)
{
  uint32_t n = 0;
  if(!v || !len || len > 5)
    return 0;
  for(uint16_t i = 0; i < len; i++) {
    if(v[i] < '0' || v[i] > '9')
      return 0;
    n = n * 10 + v[i] - '0';
  }
  if(n > max)
    return 0;
  *out_v = n;
  return 1;
}

static uint8_t
parse_ip(const char *v, uint16_t len, uint8_t ip[4])
{
  if(!v)
    return 0;
  const char *end = v + len;
  for(uint8_t i = 0; i < 4; i++) {
    const char *s = v;
    while(v < end && *v != '.')
      v++;
    uint16_t n;
    if(!parse_uint(s, v - s, 255, &n))
      return 0;
    ip[i] = n;
    if(i < 3) {
      if(v >= end)
        return 0;
      v++;                            // the dot
    }
  }
  return v == end;
}

static void
ip_str(const uint8_t ip[4], char *s)
{
  for(uint8_t i = 0; i < 4; i++) {
    uint8_t v = ip[i];
    if(v >= 100) *s++ = '0' + v / 100;
    if(v >= 10)  *s++ = '0' + v / 10 % 10;
    *s++ = '0' + v % 10;
    *s++ = i < 3 ? '.' : 0;
  }
}

static void
ew_bytes(uint8_t *ee, const uint8_t *v, uint8_t n)
{
  while(n--)
    ewb(ee++, *v++);                 // unchanged bytes are not rewritten
}

static void
schedule_reboot(void)
{
  reboot_pending = 1;
  timer_set(&reboot_timer, REBOOT_DELAY);
}

/* ------------------------------------------------------------------------
 * Password
 */

static int8_t
hexval(char c)
{
  if(c >= '0' && c <= '9') return c - '0';
  c = lower(c);
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

/* Decodes a form value (+ and %XX); returns the length, or -1. */
static int16_t
url_decode(const char *v, uint16_t len, char *dst, uint16_t max)
{
  uint16_t n = 0;
  for(uint16_t i = 0; i < len; i++) {
    char c = v[i];
    if(c == '+') {
      c = ' ';
    } else if(c == '%') {
      if(i + 2 >= len)                // two hex digits have to follow
        return -1;
      int8_t hi = hexval(v[i+1]), lo = hexval(v[i+2]);
      if(hi < 0 || lo < 0)
        return -1;
      c = hi << 4 | lo;
      i += 2;
      if(!c)
        return -1;
    }
    if(n >= max)
      return -1;
    dst[n++] = c;
  }
  return n;
}

static int8_t
b64val(char c)
{
  if(c >= 'A' && c <= 'Z') return c - 'A';
  if(c >= 'a' && c <= 'z') return c - 'a' + 26;
  if(c >= '0' && c <= '9') return c - '0' + 52;
  if(c == '+') return 62;
  if(c == '/') return 63;
  return -1;
}

/* Decodes base64; returns the length, or -1. */
static int16_t
b64_decode(const char *in, uint16_t len, char *dst, uint16_t max)
{
  uint16_t n = 0;
  uint16_t acc = 0;
  uint8_t bits = 0;
  for(uint16_t i = 0; i < len && in[i] != '='; i++) {
    int8_t v = b64val(in[i]);
    if(v < 0)
      return -1;
    acc = acc << 6 | v;
    bits += 6;
    if(bits >= 8) {
      bits -= 8;
      if(n >= max)
        return -1;
      dst[n++] = acc >> bits;
    }
  }
  return n;
}

static void
auth_hash(const uint8_t *salt, const char *pw, uint8_t len, uint8_t h[32])
{
  sha256_ctx c;
  sha256_init(&c);
  sha256_update(&c, salt, AUTH_SALT);
  sha256_update(&c, pw, len);
  sha256_final(&c, h);
}

static void
auth_store(const char *pw, uint8_t len)
{
  uint8_t salt[AUTH_SALT], h[32];
  uint32_t t = ticks;

  // No RNG on this chip: the uptime in ticks, folded into the old salt.
  for(uint8_t i = 0; i < AUTH_SALT; i++)
    salt[i] = erb(EE_HTTPD_AUTH + 1 + i) ^ (uint8_t)(t >> (8 * i));
  auth_hash(salt, pw, len, h);
  ew_bytes(EE_HTTPD_AUTH + 1, salt, AUTH_SALT);
  ew_bytes(EE_HTTPD_AUTH + 1 + AUTH_SALT, h, AUTH_HASH);
  ewb(EE_HTTPD_AUTH, AUTH_SET);       // last: a torn write leaves no marker
}

/* 1: the request may proceed. 0: a 401 or 429 has been rendered. */
static uint8_t
authorized(const char *hdr_end)
{
  if(erb(EE_HTTPD_AUTH) != AUTH_SET)
    return 1;

  if(auth_fails >= AUTH_TRIES) {
    if(!timer_expired(&auth_lock)) {
      page_error("429 Too Many Requests");
      return 0;
    }
    auth_fails = 0;
  }

  // A browser asks without credentials first; that is not a failed try.
  const char *v = header("authorization:", hdr_end);
  if(!v || strncmp(v, "Basic ", 6)) {
    page_unauthorized();
    return 0;
  }
  if(logout_pending && !memcmp(logout_ip, uip_conn->ripaddr, 4)) {
    logout_pending = 0;               // not a failed try either
    page_unauthorized();
    return 0;
  }

  char cred[sizeof(AUTH_USER) + AUTH_PW_MAX];       // "admin:" + password
  int16_t n = b64_decode(v + 6, value_len(v) - 6, cred, sizeof(cred));
  uint8_t diff = 1;
  if(n >= (int16_t)sizeof(AUTH_USER) &&
     !memcmp(cred, AUTH_USER ":", sizeof(AUTH_USER))) {
    uint8_t salt[AUTH_SALT], h[32];
    for(uint8_t i = 0; i < AUTH_SALT; i++)
      salt[i] = erb(EE_HTTPD_AUTH + 1 + i);
    auth_hash(salt, cred + sizeof(AUTH_USER), n - sizeof(AUTH_USER), h);
    diff = 0;
    for(uint8_t i = 0; i < AUTH_HASH; i++)
      diff |= h[i] ^ erb(EE_HTTPD_AUTH + 1 + AUTH_SALT + i);
  }
  memset(cred, 0, sizeof(cred));

  if(!diff) {
    auth_fails = 0;
    return 1;
  }
  if(++auth_fails >= AUTH_TRIES)
    timer_set(&auth_lock, AUTH_LOCK);
  page_unauthorized();
  return 0;
}

/* Checks the whole form before anything is written; returns the reason
   it was refused, or 0. */
static const char *
save(const char *body, uint8_t *dhcp, uint8_t a[4])
{
  uint8_t n[4], g[4], ntp[4];
  uint16_t len = 0, port, off_abs;
  const char *v;
  int8_t off;

  // field() first: the order function arguments are evaluated in is
  // unspecified, so it cannot be an argument next to its own 'len'
  v = field(body, 'a', &len);
  if(!parse_ip(v, len, a))
    return "Invalid IP address.";
  v = field(body, 'n', &len);
  if(!parse_ip(v, len, n))
    return "Invalid netmask.";
  v = field(body, 'g', &len);
  if(!parse_ip(v, len, g))
    return "Invalid gateway.";
  v = field(body, 'N', &len);
  if(!parse_ip(v, len, ntp))
    return "Invalid NTP server.";
  v = field(body, 'p', &len);
  if(!parse_uint(v, len, 65535, &port) || !port || port == HTTPD_PORT)
    return "Invalid TCP port (1-65535, not 80).";

  v = field(body, 'o', &len);
  uint8_t neg = v && len && v[0] == '-';
  if(!parse_uint(neg ? v + 1 : v, neg ? len - 1 : len, neg ? 12 : 14,
                 &off_abs))
    return "Invalid time zone (-12 to 14).";
  off = neg ? -(int8_t)off_abs : (int8_t)off_abs;

  char pw[AUTH_PW_MAX], pw2[AUTH_PW_MAX];
  int16_t pw_len = 0, pw2_len = 0;
  v = field(body, 'w', &len);
  if(v)
    pw_len = url_decode(v, len, pw, sizeof(pw));
  v = field(body, 'r', &len);
  if(v)
    pw2_len = url_decode(v, len, pw2, sizeof(pw2));
  if(pw_len < 0 || pw2_len < 0)
    return "Invalid password (at most 32 characters).";
  if(pw_len != pw2_len || memcmp(pw, pw2, pw_len))
    return "The two passwords differ.";

#ifdef HAS_IP_FILTER
  // a request without the field leaves the list alone
  ipfilter_entry fl[IPFILTER_MAX];
  int8_t fl_n = 0;
  const char *fv = field(body, 'f', &len);
  if(fv) {
    char fs[100];
    int16_t fs_len = url_decode(fv, len, fs, sizeof(fs));
    fl_n = fs_len < 0 ? -1 : ipfilter_parse(fs, fs_len, fl);
    if(fl_n < 0)
      return "Invalid list of allowed clients (at most 4, like "
             "192.168.1.0/24).";
    if(!ipfilter_match(fl, fl_n, (const uint8_t *)uip_conn->ripaddr))
      return "The allowed clients do not include this computer.";
  }
#endif

#ifdef HAS_HOSTNAME
  // a request without the field leaves the name alone; empty: the default
  char hn[EE_HOSTNAME_SIZE];
  int16_t hn_len = -1;
  const char *hv = field(body, 'h', &len);
  if(hv) {
    hn_len = url_decode(hv, len, hn, sizeof(hn) - 1);
    if(hn_len < 0 || (hn_len && !hostname_valid(hn, hn_len)))
      return "Invalid host name (up to 23 letters, digits and '-', not "
             "first or last).";
  }
#endif

  // an unchecked checkbox is not sent at all
  v = field(body, 'x', &len);
  uint8_t remove_pw = v && len == 1 && v[0] == '1';
  v = field(body, 'd', &len);
  *dhcp = v && len == 1 && v[0] == '1';
  uint8_t pb[2] = { port & 0xff, port >> 8 };  // eeprom_read_word order

  if(pw_len)
    auth_store(pw, pw_len);
  else if(remove_pw)
    ewb(EE_HTTPD_AUTH, 0);
#ifdef HAS_IP_FILTER
  if(fv)
    ipfilter_store(fl, fl_n);
#endif
#ifdef HAS_HOSTNAME
  if(hn_len >= 0)
    hostname_store(hn, hn_len);
#endif
  memset(pw, 0, sizeof(pw));
  memset(pw2, 0, sizeof(pw2));

  ewb(EE_USE_DHCP, *dhcp);
  ew_bytes(EE_IP4_ADDR, a, 4);
  ew_bytes(EE_IP4_NETMASK, n, 4);
  ew_bytes(EE_IP4_GATEWAY, g, 4);
  ew_bytes(EE_IP4_NTPSERVER, ntp, 4);
  ew_bytes(EE_IP4_TCPLINK_PORT, pb, 2);
  ewb(EE_IP4_NTPOFFSET, (uint8_t)off);
  return 0;
}

static void
handle_save(const char *body)
{
  uint8_t dhcp, a[4];
  const char *err = save(body, &dhcp, a);

  if(err) {
    page_config(err, "t-net");
    return;
  }
  char ip[16];
  ip_str(a, ip);
  page_restart(dhcp ? 0 : ip);
  schedule_reboot();
}

#ifdef USE_RF_MODE
/* m=0 enforces the duty cycle limit again; m=1..DUTY_MAX_MIN suspends it
   for that many minutes, but only with the regulations confirmed (c=1). */
static void
handle_duty(const char *body)
{
  uint16_t len = 0, m;
  const char *v = field(body, 'm', &len);
  if(!parse_uint(v, len, DUTY_MAX_MIN, &m)) {
    page_config("Invalid duration (1-60 minutes).", "t-ov");
    return;
  }
  v = field(body, 'c', &len);
  if(m && !(v && len == 1 && v[0] == '1')) {
    page_config("Confirm that you observe the radio regulations.", "t-ov");
    return;
  }
  credit_suspend_s = m * 60;
  if(m)                               // nothing is taken while suspended:
    credit_10ms = MAX_CREDIT;         // keep FHEM from waiting on it
  page_redirect();
}
#endif

#ifdef HAS_FW_UPDATE
static void
upload_reply(const char *status, const char *msg)
{
  out("HTTP/1.0 ");
  out(status);
  out("\r\nContent-Type: text/plain; charset=utf-8\r\n"
      "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
  out(msg);
}

static void
upload_done(void)
{
  const char *err = fwupdate_finish();
  if(err) {
    upload_reply("400 Bad Request", err);
    return;
  }
  upload_reply("200 OK", "Checked: ");
  out(fwupdate_staged_id());
  out(" version ");
  out(fwupdate_version());
  out(", ");
  out_u32(fwupdate_size());
  out(" bytes, CRC32 ");
  out_hex32(fwupdate_crc());
  uint32_t tu, tc;
  fwupdate_times(&tu, &tc);
  out(" (upload ");
  out_fixed(tu * 10 / 125, 1);
  out(" s, check ");
  out_fixed(tc * 10 / 125, 1);
  out(" s). Ready to install.");
  if(strcmp(fwupdate_staged_id(), FW_IMAGE_ID)) {
    out(" It replaces " FW_IMAGE_ID " with ");
    out(fwupdate_staged_id());
    out(".");
  }
}

/* data: body bytes of the owner's request */
static void
upload_data(const uint8_t *data, uint16_t len)
{
  if(len > upload_left)
    len = upload_left;                // anything behind the body
  const char *err = fwupdate_feed(data, len);
  upload_left -= len;
  if(err) {
    uploading = 0;
    upload_reply("400 Bad Request", err);
  } else if(!upload_left) {
    uploading = 0;
    upload_done();
  }
}

/* The headers of POST /update are in rx[]; body bytes may follow in rx and
   in the rest of the current segment (more). */
static void
upload_start(const char *hdr_end, const uint8_t *more, uint16_t more_len)
{
  if(!authorized(hdr_end))
    return;                           // 401 or 429 rendered
  if(foreign_origin(hdr_end)) {
    upload_reply("403 Forbidden", "Refused: the request came from another page.");
    return;
  }
  if(erb(EE_HTTPD_AUTH) != AUTH_SET) {
    upload_reply("403 Forbidden", "Set a password for this page first.");
    return;
  }

  const char *cl = header("content-length:", hdr_end);
  uint32_t len = 0;
  uint16_t n = cl ? value_len(cl) : 0;
  if(!n || n > 7) {
    upload_reply("411 Length Required", "The upload has no valid length.");
    return;
  }
  for(uint16_t i = 0; i < n; i++) {
    if(cl[i] < '0' || cl[i] > '9') {
      upload_reply("400 Bad Request", "The upload has no valid length.");
      return;
    }
    len = len * 10 + cl[i] - '0';
  }

  const char *err = fwupdate_begin(len);
  if(err) {
    upload_reply("400 Bad Request", err);
    return;
  }
  uploading = 1;
  upload_left = len;

  const char *body = hdr_end + 4;
  if(rx + rx_len > body)
    upload_data((const uint8_t *)body, rx + rx_len - body);
  if(uploading && more_len)
    upload_data(more, more_len);
}

static void
handle_install(void)
{
  if(!fwupdate_ready()) {
    page_error("409 Conflict");
    return;
  }
  out_header("200 OK");
  out_head("");                       // reload / after a while
  out("<main><section><p>Installing ");
  out(fwupdate_staged_id());
  out(" version ");
  out(fwupdate_version());
  out(" and restarting. This takes a few seconds; do not switch the device "
      "off meanwhile. The page reloads by itself.</p></section></main>"
      "</body></html>");
  install_pending = 1;
  timer_set(&install_timer, INSTALL_DELAY);
}
#endif

/* Called once the request is complete in rx[]; renders the response. */
static void
handle_request(const char *hdr_end)
{
  uint8_t post = !strncmp(rx, "POST ", 5);
  const char *path = rx + (post ? 5 : 4);
  uint8_t root = !strncmp(path, "/ ", 2);

  if(!post && strncmp(rx, "GET ", 4))
    page_error("405 Method Not Allowed");
  else if(!authorized(hdr_end))
    ;                                 // 401 or 429 rendered
  else if(!post)
    root ? page_config(0, 0) :
    !strncmp(path, "/s.css", 6) && (path[6] == ' ' || path[6] == '?') ?
      page_css() : page_error("404 Not Found");
  else if(foreign_origin(hdr_end))
    page_error("403 Forbidden");
  else if(root)
    handle_save(hdr_end + 4);
#ifdef USE_RF_MODE
  else if(!strncmp(path, "/duty ", 6))
    handle_duty(hdr_end + 4);
#endif
#ifdef HAS_FW_UPDATE
  else if(!strncmp(path, "/install ", 9))
    handle_install();
#endif
  else if(!strncmp(path, "/logout ", 8)) {
    memcpy(logout_ip, uip_conn->ripaddr, 4);
    logout_pending = erb(EE_HTTPD_AUTH) == AUTH_SET;
    out_header("200 OK");
    out("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>" BOARD_NAME "</title>"
        "<link rel=\"stylesheet\" href=\"/s.css?" VERSION "\">"
        "<script>history.replaceState(null,'','/')</script></head><body>"
        "<main><section><p>Logged out. <a href=\"/\">Log in again</a></p>"
        "<p class=\"i\">Some browsers ask for the password only after they "
        "have been closed.</p></section></main></body></html>");
  } else if(!strncmp(path, "/factory ", 9)) {
    const char *v;
    uint16_t len = 0;
    v = field(hdr_end + 4, 'c', &len);
    if(!(v && len == 1 && v[0] == '1')) {
      page_config("Tick \"Reset all settings\" to confirm.", "t-sys");
    } else {
      out_header("200 OK");
      out_head("");
      out("<main><section><p>Resetting all settings and restarting. This "
          "takes a few seconds; the page then reloads by itself.</p>"
          "</section></main></body></html>");
      factory_pending = 1;
      timer_set(&reboot_timer, REBOOT_DELAY);
    }
  } else if(!strncmp(path, "/reboot ", 8)) {
    page_restart(0);
    schedule_reboot();
  } else
    page_error("404 Not Found");
}

/* Appends the new segment; renders the response once the request is
   complete. */
static void
receive(void)
{
  // Take what fits: a firmware upload's first segment carries the headers
  // and the start of a body far larger than rx.
  uint16_t n = uip_datalen(), take = n;
  if(take > RX_SIZE - rx_len)
    take = RX_SIZE - rx_len;
  memcpy(rx + rx_len, uip_appdata, take);
  rx_len += take;
  rx[rx_len] = 0;

  char *hdr_end = strstr(rx, "\r\n\r\n");
  if(!hdr_end) {
    if(take < n || rx_len == RX_SIZE)
      page_error("413 Request Entity Too Large");
    return;                           // headers incomplete
  }
#ifdef HAS_FW_UPDATE
  if(!strncmp(rx, "POST /update ", 13)) {
    upload_start(hdr_end, (const uint8_t *)uip_appdata + take, n - take);
    return;
  }
#endif
  if(take < n) {
    page_error("413 Request Entity Too Large");
    return;
  }

  const char *cl = header("content-length:", hdr_end);
  uint16_t body_len = 0;
  if(cl && !parse_uint(cl, value_len(cl), RX_SIZE, &body_len)) {
    page_error("400 Bad Request");
    return;
  }
  if(rx + rx_len < hdr_end + 4 + body_len)
    return;                           // body incomplete

  hdr_end[4 + body_len] = 0;           // ignore anything after the body
  handle_request(hdr_end);
}

static void
send_chunk(void)
{
  tx_chunk = tx_len - tx_pos;
  if(tx_chunk > uip_mss())
    tx_chunk = uip_mss();
  uip_send(tx + tx_pos, tx_chunk);
}

/* ------------------------------------------------------------------------
 * uIP interface
 */

void
httpd_init(void)
{
  uip_unlisten(HTONS(HTTPD_PORT));
  uip_listen(HTONS(HTTPD_PORT));
}

void
httpd_appcall(void)
{
  if(uip_conn != owner) {
    /* Idle connections (browsers open some in advance) cost nothing; the
       server is taken by the first one that sends a request. */
    if(uip_newdata() && !owner) {
      owner = uip_conn;
      rx_len = tx_len = tx_pos = tx_chunk = 0;
#ifdef HAS_FW_UPDATE
      uploading = 0;
#endif
      timer_set(&req_timer, REQ_TIMEOUT);
    } else if(uip_newdata() || uip_rexmit()) {
      uip_send(busy, sizeof(busy) - 1);
      return;
    } else {
      if(uip_acked())
        uip_close();                  // the 503 went out
      return;
    }
  }

  if(uip_aborted() || uip_timedout() || uip_closed()) {
    owner = 0;
#ifdef HAS_FW_UPDATE
    if(uploading) {                   // the browser gave up mid-upload
      uploading = 0;
      fwupdate_abort();
    }
#endif
    return;
  }

  if(uip_acked() && tx_chunk) {
    tx_pos += tx_chunk;
    tx_chunk = 0;
    if(tx_pos >= tx_len) {
      owner = 0;
      uip_close();
      return;
    }
  }

  if(uip_rexmit()) {
    uip_send(tx + tx_pos, tx_chunk);
    return;
  }

#ifdef HAS_FW_UPDATE
  if(uip_newdata() && !tx_len && uploading) {
    timer_set(&req_timer, REQ_TIMEOUT);   // an upload takes longer than 10 s
    upload_data(uip_appdata, uip_datalen());
  } else
#endif
  if(uip_newdata() && !tx_len)
    receive();

  if(uip_poll() && !tx_len && timer_expired(&req_timer)) {
    owner = 0;
    uip_abort();
    return;
  }

  if(tx_len && !tx_chunk)
    send_chunk();
}

#ifdef HTTPD_RESET_PIN
/* Held for RESET_HOLD calls, the button removes the password and the IP
   whitelist; all LEDs then blink together for a few seconds. led_hold keeps
   the heartbeat and the status LEDs away meanwhile; afterwards they show
   their state again by themselves. */
static void
reset_button(void)
{
  static uint8_t init, held, blink;

  if(!init) {
    HTTPD_RESET_PIO->PIO_PER   = HTTPD_RESET_PIN;   // a plain input
    HTTPD_RESET_PIO->PIO_ODR   = HTTPD_RESET_PIN;
    HTTPD_RESET_PIO->PIO_PPUER = HTTPD_RESET_PIN;
    init = 1;
  }

  if(blink) {
    blink--;
    for(uint8_t i = 0; i < LED_COUNT; i++)
      HAL_LED_Set(i, blink & 1 ? LED_on : LED_off);
    if(!blink)
      led_hold = 0;
  }

  if(HTTPD_RESET_PIO->PIO_PDSR & HTTPD_RESET_PIN) {  // released
    held = 0;
    return;
  }
  if(held < RESET_HOLD && ++held == RESET_HOLD) {
    ewb(EE_HTTPD_AUTH, 0);
    auth_fails = 0;
#ifdef HAS_IP_FILTER
    ipfilter_clear();
#endif
    led_hold = 1;
    blink = RESET_BLINK;
  }
}
#endif

/* Called from Ethernet_Task on its periodic timer. */
void
httpd_periodic(void)
{
#ifdef HTTPD_RESET_PIN
  reset_button();
#endif
  if(reboot_pending && timer_expired(&reboot_timer)) {
    reboot_pending = 0;
    prepare_boot(0);
  }
  if(factory_pending && timer_expired(&reboot_timer)) {
    factory_pending = 0;
    eeprom_factory_reset(0);          // restarts the device itself
  }
#ifdef HAS_FW_UPDATE
  if(install_pending && timer_expired(&install_timer))
    fwupdate_install();               // does not return
#endif
}

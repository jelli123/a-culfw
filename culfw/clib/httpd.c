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
 * There is no authentication, as there is none on the tcplink port either.
 * A POST whose Origin header names another host is refused, so a web page
 * elsewhere cannot make the browser reconfigure the device.
 */

#include <string.h>
#include <avr/eeprom.h>

#include "board.h"
#include "uip.h"
#include "timer.h"
#include "fncollection.h"
#include "version.h"
#include "httpd.h"

#define RX_SIZE       1024            // request line, headers and body
#define TX_SIZE       4096            // the complete response
#define REQ_TIMEOUT   (CLOCK_SECOND * 10)
#define REBOOT_DELAY  (CLOCK_SECOND * 3)

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
out_ip(uint8_t *ee)
{
  for(uint8_t i = 0; i < 4; i++) {
    if(i)
      out(".");
    out_u(erb(ee + i));
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

static void
out_head(const char *refresh_ip)
{
  out("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  if(refresh_ip) {
    out("<meta http-equiv=\"refresh\" content=\"10;url=http://");
    out(refresh_ip);
    out("/\">");
  }
  out("<title>" BOARD_NAME "</title><style>"
      "body{font-family:sans-serif;max-width:30em;margin:1em auto;padding:0 1em}"
      "label{display:block;margin:.7em 0 .2em}"
      "input[type=text]{width:100%;box-sizing:border-box;padding:.3em}"
      "button{margin-top:1em;padding:.5em 1em}"
      ".i{color:#666;font-size:.9em}.e{padding:.5em;background:#fdd}"
      "</style></head><body><h1>" BOARD_NAME "</h1>"
      "<p class=\"i\">" FW_NAME " " VERSION " &middot; " BOARD_ID_STR
      "<br>MAC ");
  out_mac();
  out("</p>");
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

static void
page_config(const char *error)
{
  int8_t off = (int8_t)erb(EE_IP4_NTPOFFSET);

  out_header(error ? "400 Bad Request" : "200 OK");
  out_head(0);
  if(error) {
    out("<p class=\"e\">");
    out(error);
    out(" Nothing was saved.</p>");
  }
  out("<form method=\"post\" action=\"/\">"
      "<label><input type=\"checkbox\" name=\"d\" value=\"1\"");
  if(erb(EE_USE_DHCP))
    out(" checked");
  out("> DHCP (the addresses below are then the last lease)</label>");
  out_field("IP address", 'a', EE_IP4_ADDR);             out("\">");
  out_field("Netmask", 'n', EE_IP4_NETMASK);             out("\">");
  out_field("Gateway", 'g', EE_IP4_GATEWAY);             out("\">");
  out_field("NTP server (0.0.0.0: the gateway)", 'N', EE_IP4_NTPSERVER);
  out("\">");
  out_field("TCP port for FHEM", 'p', 0);
  out_u(eeprom_read_word((uint16_t *)EE_IP4_TCPLINK_PORT));
  out("\">");
  out_field("Time zone, hours from UTC", 'o', 0);
  if(off < 0) {
    out("-");
    out_u(-off);
  } else {
    out_u(off);
  }
  out("\"><button>Save and restart</button></form>"
      "<form method=\"post\" action=\"/reboot\">"
      "<button>Restart</button></form></body></html>");
}

static void
page_restart(const char *new_ip)
{
  out_header("200 OK");
  out_head(new_ip);
  out("<p>Restarting&hellip;</p>");
  if(new_ip) {
    out("<p>The device will answer at <a href=\"http://");
    out(new_ip);
    out("/\">");
    out(new_ip);
    out("</a>.</p>");
  }
  out("</body></html>");
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

  // an unchecked checkbox is not sent at all
  v = field(body, 'd', &len);
  *dhcp = v && len == 1 && v[0] == '1';
  uint8_t pb[2] = { port & 0xff, port >> 8 };  // eeprom_read_word order

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
    page_config(err);
    return;
  }
  char ip[16];
  ip_str(a, ip);
  page_restart(dhcp ? 0 : ip);
  schedule_reboot();
}

/* Called once the request is complete in rx[]; renders the response. */
static void
handle_request(const char *hdr_end)
{
  uint8_t post = !strncmp(rx, "POST ", 5);
  const char *path = rx + (post ? 5 : 4);
  uint8_t root = !strncmp(path, "/ ", 2);

  if(!post && strncmp(rx, "GET ", 4))
    page_error("405 Method Not Allowed");
  else if(!post)
    root ? page_config(0) : page_error("404 Not Found");
  else if(foreign_origin(hdr_end))
    page_error("403 Forbidden");
  else if(root)
    handle_save(hdr_end + 4);
  else if(!strncmp(path, "/reboot ", 8)) {
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
  uint16_t n = uip_datalen();
  if(n > RX_SIZE - rx_len) {
    page_error("413 Request Entity Too Large");
    return;
  }
  memcpy(rx + rx_len, uip_appdata, n);
  rx_len += n;
  rx[rx_len] = 0;

  char *hdr_end = strstr(rx, "\r\n\r\n");
  if(!hdr_end)
    return;                           // headers incomplete

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

/* Called from Ethernet_Task on its periodic timer. */
void
httpd_periodic(void)
{
  if(reboot_pending && timer_expired(&reboot_timer)) {
    reboot_pending = 0;
    prepare_boot(0);
  }
}

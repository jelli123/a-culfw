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
 * (HTTPD_RESET_PIN) for 10 seconds while running removes the password, as
 * does the e factory reset.
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
#ifdef HTTPD_RESET_PIN
#include "led.h"
#endif

#define RX_SIZE       1024            // request line, headers and body
#define TX_SIZE       4096            // the complete response
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
      "input:not([type=checkbox]){width:100%;box-sizing:border-box;padding:.3em}"
      "h2{font-size:1.1em;margin-top:1.5em}"
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
  out("\"><h2>Password for this page</h2>");
  if(erb(EE_HTTPD_AUTH) == AUTH_SET)
    out("<p class=\"i\">User name: " AUTH_USER ". Holding the button on the "
        "bottom for 10 seconds removes the password.</p>"
        "<label><input type=\"checkbox\" name=\"x\" value=\"1\"> "
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
      "<button>Save and restart</button></form>"
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
  else if(!authorized(hdr_end))
    ;                                 // 401 or 429 rendered
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

#ifdef HTTPD_RESET_PIN
/* Held for RESET_HOLD calls, the button removes the password; the LED
   then blinks fast for a few seconds. */
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
    LED_TOGGLE();
    if(!--blink)
      LED_OFF();
  }

  if(HTTPD_RESET_PIO->PIO_PDSR & HTTPD_RESET_PIN) {  // released
    held = 0;
    return;
  }
  if(held < RESET_HOLD && ++held == RESET_HOLD) {
    ewb(EE_HTTPD_AUTH, 0);
    auth_fails = 0;
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
}

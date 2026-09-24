#ifndef _HTTPD_H_
#define _HTTPD_H_

/* Minimal configuration web page on port 80: shows and sets the network
   settings that are otherwise written with the Wi* commands. */

#define HTTPD_PORT 80

void httpd_init(void);
void httpd_appcall(void);
void httpd_periodic(void);

#endif

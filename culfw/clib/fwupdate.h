#ifndef _FWUPDATE_H_
#define _FWUPDATE_H_

#include <stdint.h>

/* Firmware update through the AT45 dataflash (HAS_FW_UPDATE).

   fwupdate_begin() / fwupdate_feed() / fwupdate_finish() stage an uploaded
   image in the dataflash and check it; each returns the reason it refused,
   or 0. fwupdate_install() then copies it over the application and
   restarts - it does not return. */

const char *fwupdate_begin(uint32_t len);
const char *fwupdate_feed(const uint8_t *data, uint16_t len);
const char *fwupdate_finish(void);
void fwupdate_abort(void);

uint8_t fwupdate_ready(void);
uint32_t fwupdate_size(void);
uint32_t fwupdate_crc(void);
void fwupdate_times(uint32_t *upload, uint32_t *check);   // 1/125 s
const char *fwupdate_version(void);     // of the staged image
const char *fwupdate_staged_id(void);   // its FW_IMAGE_ID
const char *fwupdate_image_id(void);    // of the running one

void fwupdate_install(void) __attribute__((noreturn));

/* The last install's report, kept in RAM across its restart */
#define FW_NONE       0
#define FW_INSTALLED  1               // copied, the flash CRC matches
#define FW_REFUSED    2               // misread at 6 and 1 MHz: nothing written
#define FW_STARTED    3               // cut short while copying
#define FW_BAD_FLASH  4               // copied, but the flash CRC differs
#define FW_READING    5               // stopped while reading the staged image
void fwupdate_boot(void);             // once, early at start
uint8_t fwupdate_last(uint32_t *mhz, uint32_t *expect, uint32_t *fast,
                      uint32_t *slow, uint32_t *flash_crc);

#endif

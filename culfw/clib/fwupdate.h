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

#endif

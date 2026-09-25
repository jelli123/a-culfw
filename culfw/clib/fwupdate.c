/*
 * Firmware update through the AT45 dataflash (HAS_FW_UPDATE, AT91SAM7).
 *
 * Staging: the uploaded image is written page by page into the dataflash
 * from STAGE_PAGE on, far behind the settings page, and a CRC32 is kept as
 * it arrives. fwupdate_finish() reads it all back: the CRC has to match,
 * and the image has to carry the id of the running one - the
 * "a-culfw-image:<FW_IMAGE_ID>;<version>;" string below. That keeps a
 * CUBEx4 image off a CUBe, and images without update support (older ones)
 * off the page; those still go through the bootloader's USB drive.
 *
 * Installing: the image cannot be copied by code that lives in the flash it
 * overwrites. ram_install() runs from RAM (.ramfunc) with every interrupt
 * off, drives SPI1 by hand instead of the at91lib driver (the chip select
 * as a plain output), and calls nothing in flash. It first reads the whole
 * staged image that way and checks its CRC32; on a mismatch it restarts
 * without writing anything, and the old firmware runs on. Then it programs
 * the application's first page blank:
 * the bootloader starts the application only when that word is not
 * 0xffffffff, so if the copy is cut short - power, anything - the device
 * comes up in the bootloader's USB drive by itself. Then it copies pages
 * 1..n, each verified and retried, and the first page last. The bootloader
 * (0x100000-0x104000) is never written.
 */

#include "board.h"

#ifdef HAS_FW_UPDATE

#include <string.h>
#include <avr/eeprom.h>                 // dataflash_*

#include "fwupdate.h"
#include "version.h"

#define STAGE_PAGE      16              // first dataflash page of the image
#define PAGE_MAX        528             // largest AT45 page handled
#define FLASH_PAGE      256             // AT91SAM7X256 internal flash page

#define ID_PREFIX       "a-culfw-image:" FW_IMAGE_ID ";"

/* Found in the image by fwupdate_finish(); the version ends it. Its own
   first PREFIX_LEN bytes are what is searched for, so the prefix is in
   every image once, here, and this string is linked in by that use. */
const char fw_image_ident[] = ID_PREFIX VERSION ";";
#define PREFIX_LEN      (sizeof(ID_PREFIX) - 1)

extern char _sfixed[], _flash_end[];    // CUBE*_flash.lds

enum { FW_IDLE, FW_RECEIVING, FW_READY };

static uint8_t state;
static uint32_t total, got, crc_rx, crc_ok;
static uint16_t page_size, fill;
static uint32_t wr_addr;
static uint8_t pbuf[PAGE_MAX];
static char version[24];

static uint32_t
crc32(uint32_t crc, const uint8_t *p, uint16_t n)
{
  while(n--) {
    crc ^= *p++;
    for(uint8_t k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return crc;
}

void
fwupdate_abort(void)
{
  state = FW_IDLE;
}

const char *
fwupdate_begin(uint32_t len)
{
  uint16_t pages, reserved;
  state = FW_IDLE;
  if(!dataflash_info(&pages, &page_size, &reserved))
    return "No dataflash found.";
  if(page_size > PAGE_MAX || pages >= 16384)
    return "This dataflash is not supported.";
  if(len < FLASH_PAGE || len > (uint32_t)(_flash_end - _sfixed))
    return "The file is no firmware image: too small or too large.";
  if(len > (uint32_t)(pages - STAGE_PAGE) * page_size)
    return "The image does not fit into the dataflash.";

  total = len;
  got = 0;
  fill = 0;
  crc_rx = 0xffffffff;
  wr_addr = (uint32_t)STAGE_PAGE * page_size;
  version[0] = 0;
  state = FW_RECEIVING;
  return 0;
}

const char *
fwupdate_feed(const uint8_t *data, uint16_t len)
{
  if(state != FW_RECEIVING)
    return "No upload in progress.";
  if(len > total - got) {
    state = FW_IDLE;
    return "More data than announced.";
  }
  crc_rx = crc32(crc_rx, data, len);
  while(len) {
    uint16_t n = page_size - fill;
    if(n > len)
      n = len;
    memcpy(pbuf + fill, data, n);
    fill += n;
    data += n;
    len -= n;
    got += n;
    // what the bootloader checks too: the image starts with 'ldr pc, ...'
    if(got >= 4 && got - n < 4 && (pbuf[2] != 0x9f || pbuf[3] != 0xe5)) {
      state = FW_IDLE;
      return "Not an application image: it does not start with 'ldr pc'.";
    }
    if(fill == page_size) {
      dataflash_write(wr_addr, pbuf, fill);
      wr_addr += fill;
      fill = 0;
    }
  }
  return 0;
}

const char *
fwupdate_finish(void)
{
  const char *prefix = fw_image_ident;
  uint8_t matched = 0, vlen = 0, in_version = 0, found = 0;
  uint32_t crc = 0xffffffff;

  if(state != FW_RECEIVING || got != total)
    return "The upload is incomplete.";
  if(fill) {
    dataflash_write(wr_addr, pbuf, fill);
    fill = 0;
  }

  // Read it back: the CRC checks the dataflash, the id the image.
  for(uint32_t pos = 0; pos < total; pos += page_size) {
    uint16_t n = total - pos < page_size ? total - pos : page_size;
    dataflash_read((uint32_t)STAGE_PAGE * page_size + pos, pbuf, n);
    crc = crc32(crc, pbuf, n);
    for(uint16_t i = 0; i < n && !found; i++) {
      char c = pbuf[i];
      if(in_version) {
        // only a version closed by ';' counts
        if(c == ';' && vlen) {
          version[vlen] = 0;
          found = 1;
        } else if(c < ' ' || c > '~' || c == ';' || vlen == sizeof(version) - 1) {
          in_version = 0;
          matched = c == prefix[0];
        } else {
          version[vlen++] = c;
        }
      } else if(c == prefix[matched]) {
        if(++matched == PREFIX_LEN) {
          in_version = 1;
          vlen = 0;
        }
      } else {
        // Restarting at the mismatching byte is not a full KMP search: it
        // would miss the id right behind a broken-off "a-culfw-ima". No
        // image holds that; the id is found wherever the linker put it.
        matched = c == prefix[0];
      }
    }
  }

  state = FW_IDLE;
  if(crc != crc_rx)
    return "The dataflash does not read back what was received.";
  if(!found)
    return "Not an a-culfw " FW_IMAGE_ID " image with update support.";
  crc_ok = ~crc;
  state = FW_READY;
  return 0;
}

uint8_t
fwupdate_ready(void)
{
  return state == FW_READY;
}

uint32_t
fwupdate_size(void)
{
  return total;
}

uint32_t
fwupdate_crc(void)
{
  return crc_ok;
}

const char *
fwupdate_version(void)
{
  return version;
}

const char *
fwupdate_image_id(void)
{
  return fw_image_ident;
}

/* ------------------------------------------------------------------------
 * Running from RAM. Nothing below may call into flash: no library calls,
 * no division (libgcc), and no loop the compiler could turn into memcpy.
 */

#define RAMFUNC __attribute__((section(".ramfunc"), noinline, \
                               optimize("no-tree-loop-distribute-patterns")))

/* The dataflash's chip select (NPCS0, PA21) is driven as a plain output
   during the copy. The SPI's own CSAAT/LASTXFER handling left it asserted
   after a read - LASTXFER acts on the next character written - so the
   next read's 0x0B went into the running one and the address byte became
   the command: every page after the first read as 0xff, the first word
   too, and the device stayed in the bootloader. */
#define DF_CS_PIO       AT91C_BASE_PIOA
#define DF_CS_PIN       (1u << 21)

static RAMFUNC uint8_t
ram_spi(uint8_t out)
{
  AT91PS_SPI spi = AT91C_BASE_SPI1;
  while(!(spi->SPI_SR & AT91C_SPI_TDRE))
    ;
  spi->SPI_TDR = out;
  while(!(spi->SPI_SR & AT91C_SPI_RDRF))
    ;
  return spi->SPI_RDR;
}

/* 256 bytes from device address a (continuous array read, 0x0B); bytes
   from 'valid' on are padding, 0xff as erased flash. */
static RAMFUNC void
ram_read(uint32_t a, uint32_t *buf, uint32_t valid)
{
  AT91PS_SPI spi = AT91C_BASE_SPI1;
  DF_CS_PIO->PIO_CODR = DF_CS_PIN;      // select
  ram_spi(0x0B);
  ram_spi(a >> 16);
  ram_spi(a >> 8);
  ram_spi(a);
  ram_spi(0);                           // dummy byte
  for(uint32_t w = 0; w < FLASH_PAGE / 4; w++) {
    uint32_t v = 0;
    for(uint32_t b = 0; b < 4; b++) {
      uint32_t c = ram_spi(0);
      if(4 * w + b >= valid)
        c = 0xff;
      v |= c << (8 * b);
    }
    buf[w] = v;
  }
  while(!(spi->SPI_SR & AT91C_SPI_TXEMPTY))
    ;
  DF_CS_PIO->PIO_SODR = DF_CS_PIN;      // deselect: the read ends here
  for(volatile uint32_t d = 0; d < 8; d++)
    ;                                   // CS high for more than 50 ns
}

static RAMFUNC uint32_t
ram_crc32(uint32_t crc, const uint32_t *buf, uint32_t n)
{
  for(uint32_t i = 0; i < n; i++) {
    crc ^= (buf[i / 4] >> (8 * (i & 3))) & 0xff;
    for(uint32_t k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return crc;
}

/* Programs one internal flash page (erase included), verifies, retries. */
static RAMFUNC void
ram_program(uint32_t dest, const uint32_t *buf)
{
  volatile uint32_t *f = (volatile uint32_t *)dest;
  for(uint32_t tries = 0; tries < 3; tries++) {
    for(uint32_t w = 0; w < FLASH_PAGE / 4; w++)
      f[w] = buf[w];                    // into the page latch
    AT91C_BASE_MC->MC_FCR = (0x5Au << 24) |
                            (((dest - AT91C_IFLASH) >> 8) << 8) |
                            AT91C_MC_FCMD_START_PROG;
    while(!(AT91C_BASE_MC->MC_FSR & AT91C_MC_FRDY))
      ;
    AT91C_BASE_WDTC->WDTC_WDCR = (0xA5u << 24) | AT91C_WDTC_WDRSTT;
    uint32_t same = 1;
    for(uint32_t w = 0; w < FLASH_PAGE / 4; w++)
      if(f[w] != buf[w])
        same = 0;
    if(same)
      return;
  }
}

static RAMFUNC void __attribute__((noreturn))
ram_install(uint32_t first_page, uint32_t page_size, uint32_t shift,
            uint32_t len, uint32_t dest, uint32_t crc_expect)
{
  AT91PS_SPI spi = AT91C_BASE_SPI1;
  uint32_t buf[FLASH_PAGE / 4];
  uint32_t df_page = first_page, df_off = 0, cpsr;

  // IRQ and FIQ off in the core as well as in the AIC
  __asm__ volatile("mrs %0, cpsr\n\t"
                   "orr %0, %0, #0xc0\n\t"
                   "msr cpsr_c, %0" : "=r"(cpsr));
  AT91C_BASE_AIC->AIC_IDCR = 0xffffffff;
  spi->SPI_PTCR = AT91C_PDC_RXTDIS | AT91C_PDC_TXTDIS;
  spi->SPI_MR = AT91C_SPI_MSTR | AT91C_SPI_MODFDIS | (0xE << 16);  // NPCS0
  spi->SPI_CSR[0] = (spi->SPI_CSR[0] & ~(AT91C_SPI_SCBR | AT91C_SPI_CSAAT)) |
                    (8 << 8);                                      // 6 MHz
  (void)spi->SPI_RDR;
  DF_CS_PIO->PIO_SODR = DF_CS_PIN;      // high first, then ours
  DF_CS_PIO->PIO_OER = DF_CS_PIN;
  DF_CS_PIO->PIO_PER = DF_CS_PIN;

  // 0. read the whole image the way the copy will, before touching the
  //    flash: on a mismatch the running firmware stays and just restarts
  uint32_t crc = 0xffffffff, page = first_page, off = 0;
  for(uint32_t done = 0; done < len; done += FLASH_PAGE) {
    uint32_t n = len - done < FLASH_PAGE ? len - done : FLASH_PAGE;
    ram_read((page << shift) | off, buf, n);
    crc = ram_crc32(crc, buf, n);
    AT91C_BASE_WDTC->WDTC_WDCR = (0xA5u << 24) | AT91C_WDTC_WDRSTT;
    off += FLASH_PAGE;
    if(off >= page_size) {
      off -= page_size;
      page++;
    }
  }
  if(~crc != crc_expect)
    goto restart;
  // erase before programming, FMCN for 1.5 us as EFC_PerformCommand1 uses
  AT91C_BASE_MC->MC_FMR = (AT91C_BASE_MC->MC_FMR &
                           ~(AT91C_MC_FMCN | AT91C_MC_NEBP)) | (72 << 16);

  // 1. the first page blank: from now on the bootloader keeps the device
  for(uint32_t w = 0; w < FLASH_PAGE / 4; w++)
    buf[w] = 0xffffffff;
  ram_program(dest, buf);

  // 2. pages 1..n (the dataflash address advances without a division)
  for(uint32_t done = FLASH_PAGE; done < len; done += FLASH_PAGE) {
    df_off += FLASH_PAGE;
    if(df_off >= page_size) {
      df_off -= page_size;
      df_page++;
    }
    ram_read((df_page << shift) | df_off, buf, len - done);
    ram_program(dest + done, buf);
  }

  // 3. the first page: the image is complete
  ram_read(first_page << shift, buf, len);
  ram_program(dest, buf);

restart:
  AT91C_BASE_RSTC->RSTC_RCR = (0xA5u << 24) | AT91C_RSTC_PROCRST |
                              AT91C_RSTC_PERRST | AT91C_RSTC_EXTRST;
  for(;;)
    ;
}

void
fwupdate_install(void)
{
  // the geometry before leaving flash: dataflash_* live there
  uint32_t shift = dataflash_shift();
  ram_install(STAGE_PAGE, page_size, shift, total, (uint32_t)_sfixed,
              crc_ok);
}

#endif

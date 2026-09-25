/* ----------------------------------------------------------------------------
 *         ATMEL Microcontroller Software Support
 * ----------------------------------------------------------------------------
 * Copyright (c) 2008, Atmel Corporation
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the disclaimer below.
 *
 * Atmel's name may not be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * DISCLAIMER: THIS SOFTWARE IS PROVIDED BY ATMEL "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT ARE
 * DISCLAIMED. IN NO EVENT SHALL ATMEL BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ----------------------------------------------------------------------------
 */

/*------------------------------------------------------------------------------
 *      Linker script for running in internal Flash on the AT91SAM7X256.
 *----------------------------------------------------------------------------*/

OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(entry)

MEMORY
{
    /* To the end of the 256 KiB. The CUBe keeps its settings in the AT45
       dataflash (USE_DATAFLASH); the internal-flash EEPROM emulation in
       at91lib/avr/eeprom.c, pages 959-1022, is compiled only without it. */
    sram (W!RX) : ORIGIN = 0x200000, LENGTH = 0x10000
    flash (RX) : ORIGIN = 0x104000, LENGTH = 0x3C000
}

SECTIONS
{  
    .fixed :
    {
        . = ALIGN(4);
        _sfixed = .;
        /* The startup code first, whatever order the objects are linked in:
           the image has to begin with its 'ldr pc, =1f' - the bootloader
           only accepts a .bin whose first word is an 'ldr pc' (0xE59F...) and
           starts the application at the beginning of its area. The makefile
           happens to link board_cstartup.o first; PlatformIO does not. */
        *board_cstartup.o(.text)
        *(.text*)
        *(.rodata*)
        *(.data.__malloc_av_)
        *(.data.__malloc_trim_threshold)
        *(.data.__malloc_sbrk_base)
        *(.data.__ctype_ptr__)
        *(.data.__global_locale)
        . = ALIGN(4);
    } >flash

    /* Unwind tables. GCC 7 emits .ARM.extab / .ARM.exidx and neither script
       placed them, so the linker left them as orphans right after .fixed,
       where their LMA collided with .relocate's AT (_efixed).
       .ARM.exidx must be an output section of its own: it is SHF_LINK_ORDER,
       and ld refuses to mix ordered and unordered input sections in one output
       section. _efixed therefore moves to the end of it — it is the LMA
       .relocate is copied from, so it has to sit behind everything in flash. */
    .ARM.extab : {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } >flash

    .ARM.exidx : {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
        . = ALIGN(4);
        _efixed = .;
    } >flash

    .relocate : AT (_efixed)
    {
        . = ALIGN(4);
        _srelocate = .;
        *(.vectors)
        *(.ramfunc)
        *(.data .data.*)
        . = ALIGN(4);
        _erelocate = .;
    } >sram

    .bss (NOLOAD) : {
        _szero = .;
        /* COMMON too (-fcommon): ld puts it at the end of .bss, and with
           only *(.bss) listed that was behind _ezero - never cleared at
           start, so ticks, credit_suspend_s and the like began with what
           the RAM held, across a restart even the old values. */
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ezero = .;
    } >sram
    _bss_end = . ;
    
    _sstack = 0x210000;

    /* end of the application area, for the free flash on the status page */
    _flash_end = ORIGIN(flash) + LENGTH(flash);
}
end = .;


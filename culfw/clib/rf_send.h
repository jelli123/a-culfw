#ifndef _RF_SEND_H
#define _RF_SEND_H


#include <stdint.h>                     // for uint8_t, uint16_t
#include "board.h"

/* public prototypes */
void fs20send(char *in);
void rawsend(char *in);
void em_send(char *in);
void ks_send(char *in);
void ur_send(char *in);
void hm_send(char *in);
void addParityAndSend(char *in, uint8_t startcs, uint8_t repeat);
void addParityAndSendData(uint8_t *hb, uint8_t hblen,
                        uint8_t startcs, uint8_t repeat);


/* One duty cycle budget per radio module: each is a transmitter of its
   own. credit_10ms and credit_debt name those of the module being served
   (CC1101.instance), so every send path charges the one it transmits on. */
#ifdef HAS_MULTI_CC
#include "rf_mode.h"                    // for CC_INSTANCE
#define CREDIT_RADIOS HAS_MULTI_CC
#define credit_10ms   (credit_radio[CC_INSTANCE])
#define credit_debt   (debt_radio[CC_INSTANCE])
#else
#define CREDIT_RADIOS 1
#define credit_10ms   (credit_radio[0])
#define credit_debt   (debt_radio[0])
#endif
extern uint16_t credit_radio[CREDIT_RADIOS];
extern uint16_t debt_radio[CREDIT_RADIOS];      // air time past the budget
extern uint16_t credit_suspend_s;     // > 0: limit suspended, seconds left
uint8_t credit_take(uint16_t sum);
/* For transmissions whose length is known only afterwards: check before,
   charge after (air time weighted by band, the excess as debt). */
uint8_t credit_ok(void);
void credit_air(uint32_t t0);
#ifndef MAX_CREDIT
#define MAX_CREDIT 900       // max 9 seconds burst / 25% of the hourly budget
#endif
#ifndef START_CREDIT
#define START_CREDIT (MAX_CREDIT/2)     // budget after a restart
#endif

#endif

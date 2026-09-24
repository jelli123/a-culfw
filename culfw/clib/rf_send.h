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


extern uint16_t credit_10ms;
extern uint16_t credit_suspend_s;     // > 0: limit suspended, seconds left
uint8_t credit_take(uint16_t sum);
#ifndef MAX_CREDIT
#define MAX_CREDIT 900       // max 9 seconds burst / 25% of the hourly budget
#endif
#ifndef START_CREDIT
#define START_CREDIT (MAX_CREDIT/2)     // budget after a restart
#endif

#endif

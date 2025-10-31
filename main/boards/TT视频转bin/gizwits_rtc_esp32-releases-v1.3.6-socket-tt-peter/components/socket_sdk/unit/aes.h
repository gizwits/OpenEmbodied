#ifndef _AES_H_
#define _AES_H_

#include <stdio.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define AES_BLOCK_LEN       16

/*
 * Number of columns (32-bit words) comprising the State. For this
 * standard, Nb = 4.
 */
#define AES_NB_LEN      4       /* 4 */

/*
 * Number of 32-bit words comprising the Cipher Key. For this
 * standard, Nk = 4, 6, or 8.
 */
#define AES_NK_LEN      4       /* 128-bit key */

/*
 * Number of rounds, which is a function of  Nk  and  Nb (which is
 * fixed). For this standard, Nr = 10, 12, or 14.
 */
#define AES_NR_LEN      10      /* 128-bit key */

void sboxAssign(unsigned char *outData);
void invSboxAssign(unsigned char *outData);
void aesInit(void);
void aesDestroy(void);
unsigned int aesECB128Encrypt(unsigned char *in, unsigned char *out, unsigned char *key, unsigned int len);
unsigned int aesECB128Decrypt(unsigned char *in, unsigned char *out, unsigned char *key, unsigned int len);
unsigned int  aesECB128DecryptRaw(unsigned char *in, unsigned char *out, unsigned char *key, unsigned int len);


#endif /* _AES_H_ */


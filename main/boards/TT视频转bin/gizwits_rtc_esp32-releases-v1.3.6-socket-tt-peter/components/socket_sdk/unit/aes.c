#include "aes.h"
#include "string.h"

void sboxAssign(unsigned char *outData);
void invSboxAssign(unsigned char *outData);

/*
 * Addition in GF(2^8)
 * http://en.wikipedia.org/wiki/Finite_field_arithmetic
 */
unsigned char  gadd(unsigned char a, unsigned char b) {
    return a^b;
}

/*
 * Subtraction in GF(2^8)
 * http://en.wikipedia.org/wiki/Finite_field_arithmetic
 */
unsigned char  gsub(unsigned char a, unsigned char b) {
    return a^b;
}

/*
 * Multiplication in GF(2^8)
 * http://en.wikipedia.org/wiki/Finite_field_arithmetic
 * Irreducible polynomial m(x) = x8 + x4 + x3 + x + 1
 */
unsigned char  gmult(unsigned char a, unsigned char b) {

    unsigned char p = 0, i = 0, hbs = 0;

    for (i = 0; i < 8; i++) {
        if (b & 1) {
            p ^= a;
        }

        hbs = a & 0x80;
        a <<= 1;
        if (hbs) a ^= 0x1b; // 0000 0001 0001 1011
        b >>= 1;
    }

    return (unsigned char)p;
}

/*
 * Addition of 4 byte words
 * m(x) = x4+1
 */
void  coef_add(unsigned char a[], unsigned char b[], unsigned char d[]) {

    d[0] = a[0]^b[0];
    d[1] = a[1]^b[1];
    d[2] = a[2]^b[2];
    d[3] = a[3]^b[3];
}

/*
 * Multiplication of 4 byte words
 * m(x) = x4+1
 */
void  coef_mult(unsigned char *a, unsigned char *b, unsigned char *d) {

    d[0] = gmult(a[0],b[0])^gmult(a[3],b[1])^gmult(a[2],b[2])^gmult(a[1],b[3]);
    d[1] = gmult(a[1],b[0])^gmult(a[0],b[1])^gmult(a[3],b[2])^gmult(a[2],b[3]);
    d[2] = gmult(a[2],b[0])^gmult(a[1],b[1])^gmult(a[0],b[2])^gmult(a[3],b[3]);
    d[3] = gmult(a[3],b[0])^gmult(a[2],b[1])^gmult(a[1],b[2])^gmult(a[0],b[3]);
}

/*
 * S-box transformation table
 */
static unsigned char *s_box = NULL;
/*
 * Inverse S-box transformation table
 */

static unsigned char *inv_s_box = NULL;
/*
 * Generates the round constant Rcon[i]
 */
static unsigned char R[] = {0x02, 0x00, 0x00, 0x00};

void  invSboxInit(void)
{
    if(NULL == inv_s_box)
    {
        inv_s_box = (unsigned char *)malloc(256);
        if(NULL == inv_s_box)
        {
            return ;
        }
        invSboxAssign(inv_s_box);
    }
}

/* will */
void  aesInit(void)
{
    if(NULL == s_box)
    {
        s_box = (unsigned char *)malloc(256);
        if(NULL == s_box)
        {
            return ;
        }
        sboxAssign(s_box);
    }

    invSboxInit();
}

void  aesDestroy(void)
{
    if(NULL != s_box)
    {
        free(s_box);
        s_box = NULL;
    }

    if(NULL != inv_s_box)
    {
        free(inv_s_box);
        inv_s_box = NULL;
    }
}

unsigned char  *Rcon(unsigned char i) {

    if (i == 1) {
        R[0] = 0x01; // x^(1-1) = x^0 = 1
    } else if (i > 1) {
        R[0] = 0x02;
        i--;
        while (i-1 > 0) {
            R[0] = gmult(R[0], 0x02);
            i--;
        }
    }

    return R;
}

/*
 * Transformation in the Cipher and Inverse Cipher in which a Round
 * Key is added to the State using an XOR operation. The length of a
 * Round Key equals the size of the State (i.e., for Nb = 4, the Round
 * Key length equals 128 bits/16 bytes).
 */
void  add_round_key(unsigned char *state, unsigned char *w, unsigned char r) {

    unsigned char c;

    for (c = 0; c < AES_NB_LEN; c++) {
        state[AES_NB_LEN*0+c] = state[AES_NB_LEN*0+c]^w[4*AES_NB_LEN*r+4*c+0];   //debug, so it works for AES_NB_LEN !=4
        state[AES_NB_LEN*1+c] = state[AES_NB_LEN*1+c]^w[4*AES_NB_LEN*r+4*c+1];
        state[AES_NB_LEN*2+c] = state[AES_NB_LEN*2+c]^w[4*AES_NB_LEN*r+4*c+2];
        state[AES_NB_LEN*3+c] = state[AES_NB_LEN*3+c]^w[4*AES_NB_LEN*r+4*c+3];
    }
}

/*
 * Transformation in the Cipher that takes all of the columns of the
 * State and mixes their data (independently of one another) to
 * produce new columns.
 */
void  mix_columns(unsigned char *state) {

    unsigned char a[] = {0x02, 0x01, 0x01, 0x03}; // a(x) = {02} + {01}x + {01}x2 + {03}x3
    unsigned char i, j, col[4], res[4];

    for (j = 0; j < AES_NB_LEN; j++) {
        for (i = 0; i < 4; i++) {
            col[i] = state[AES_NB_LEN*i+j];
        }

        coef_mult(a, col, res);

        for (i = 0; i < 4; i++) {
            state[AES_NB_LEN*i+j] = res[i];
        }
    }
}

/*
 * Transformation in the Inverse Cipher that is the inverse of
 * MixColumns().
 */
void  inv_mix_columns(unsigned char *state) {

    unsigned char a[] = {0x0e, 0x09, 0x0d, 0x0b}; // a(x) = {0e} + {09}x + {0d}x2 + {0b}x3
    unsigned char i, j, col[4], res[4];

    for (j = 0; j < AES_NB_LEN; j++) {
        for (i = 0; i < 4; i++) {
            col[i] = state[AES_NB_LEN*i+j];
        }

        coef_mult(a, col, res);

        for (i = 0; i < 4; i++) {
            state[AES_NB_LEN*i+j] = res[i];
        }
    }
}

/*
 * Transformation in the Cipher that processes the State by cyclically
 * shifting the last three rows of the State by different offsets.
 */
void  shift_rows(unsigned char *state) {

    unsigned char i, k, s, tmp;

    for (i = 1; i < 4; i++) {
        // shift(1,4)=1; shift(2,4)=2; shift(3,4)=3
        // shift(r, 4) = r;
        s = 0;
        while (s < i) {
            tmp = state[AES_NB_LEN*i+0];

            for (k = 1; k < AES_NB_LEN; k++) {
                state[AES_NB_LEN*i+k-1] = state[AES_NB_LEN*i+k];
            }

            state[AES_NB_LEN*i+AES_NB_LEN-1] = tmp;
            s++;
        }
    }
}

/*
 * Transformation in the Inverse Cipher that is the inverse of
 * ShiftRows().
 */
void  inv_shift_rows(unsigned char *state) {

    unsigned char i, k, s, tmp;

    for (i = 1; i < 4; i++) {
        s = 0;
        while (s < i) {
            tmp = state[AES_NB_LEN*i+AES_NB_LEN-1];

            for (k = AES_NB_LEN-1; k > 0; k--) {
                state[AES_NB_LEN*i+k] = state[AES_NB_LEN*i+k-1];
            }

            state[AES_NB_LEN*i+0] = tmp;
            s++;
        }
    }
}

/*
 * Transformation in the Cipher that processes the State using a non-
 * linear byte substitution table (S-box) that operates on each of the
 * State bytes independently.
 */
void  sub_bytes(unsigned char *state) {

    unsigned char i, j;
    unsigned char row, col;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < AES_NB_LEN; j++) {
            row = (state[AES_NB_LEN*i+j] & 0xf0) >> 4;
            col = state[AES_NB_LEN*i+j] & 0x0f;
            state[AES_NB_LEN*i+j] = s_box[16*row+col];
        }
    }
}

/*
 * Transformation in the Inverse Cipher that is the inverse of
 * SubBytes().
 */
void  inv_sub_bytes(unsigned char *state) {

    unsigned char i, j;
    unsigned char row, col;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < AES_NB_LEN; j++) {
            row = (state[AES_NB_LEN*i+j] & 0xf0) >> 4;
            col = state[AES_NB_LEN*i+j] & 0x0f;
            state[AES_NB_LEN*i+j] = inv_s_box[16*row+col];
        }
    }
}

/*
 * Function used in the Key Expansion routine that takes a four-byte
 * input word and applies an S-box to each of the four bytes to
 * produce an output word.
 */
void  sub_word(unsigned char *w) {

    unsigned char i;

    for (i = 0; i < 4; i++) {
        w[i] = s_box[16*((w[i] & 0xf0) >> 4) + (w[i] & 0x0f)];
    }
}

/*
 * Function used in the Key Expansion routine that takes a four-byte
 * word and performs a cyclic permutation.
 */
void  rot_word(unsigned char *w) {

    unsigned char tmp;
    unsigned char i;

    tmp = w[0];

    for (i = 0; i < 3; i++) {
        w[i] = w[i+1];
    }

    w[3] = tmp;
}

/*
 * Key Expansion
 */
void  key_expansion(unsigned char *key, unsigned char *w) {

    unsigned char tmp[4];
    unsigned char i;
    unsigned char len = AES_NB_LEN*(AES_NR_LEN+1);

    for (i = 0; i < AES_NK_LEN; i++) {
        w[4*i+0] = key[4*i+0];
        w[4*i+1] = key[4*i+1];
        w[4*i+2] = key[4*i+2];
        w[4*i+3] = key[4*i+3];
    }

    for (i = AES_NK_LEN; i < len; i++) {
        tmp[0] = w[4*(i-1)+0];
        tmp[1] = w[4*(i-1)+1];
        tmp[2] = w[4*(i-1)+2];
        tmp[3] = w[4*(i-1)+3];

        if (i%AES_NK_LEN == 0) {

            rot_word(tmp);
            sub_word(tmp);
            coef_add(tmp, Rcon(i/AES_NK_LEN), tmp);

        } else if (AES_NK_LEN > 6 && i%AES_NK_LEN == 4) {

            sub_word(tmp);

        }

        w[4*i+0] = w[4*(i-AES_NK_LEN)+0]^tmp[0];
        w[4*i+1] = w[4*(i-AES_NK_LEN)+1]^tmp[1];
        w[4*i+2] = w[4*(i-AES_NK_LEN)+2]^tmp[2];
        w[4*i+3] = w[4*(i-AES_NK_LEN)+3]^tmp[3];
    }
}

void  cipher(unsigned char *in, unsigned char *out, unsigned char *w) {

    unsigned char state[4*AES_NB_LEN];
    unsigned char r, i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < AES_NB_LEN; j++) {
            state[AES_NB_LEN*i+j] = in[i+4*j];
        }
    }

    add_round_key(state, w, 0);

    for (r = 1; r < AES_NR_LEN; r++) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, w, r);
    }

    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, w, AES_NR_LEN);

    for (i = 0; i < 4; i++) {
        for (j = 0; j < AES_NB_LEN; j++) {
            out[i+4*j] = state[AES_NB_LEN*i+j];
        }
    }
}

void  inv_cipher(unsigned char *in, unsigned char *out, unsigned char *w) {

    unsigned char state[4*AES_NB_LEN];
    unsigned char r, i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < AES_NB_LEN; j++) {
            state[AES_NB_LEN*i+j] = in[i+4*j];
        }
    }

    add_round_key(state, w, AES_NR_LEN);

    for (r = AES_NR_LEN-1; r >= 1; r--) {
        inv_shift_rows(state);
        inv_sub_bytes(state);
        add_round_key(state, w, r);
        inv_mix_columns(state);
    }

    inv_shift_rows(state);
    inv_sub_bytes(state);
    add_round_key(state, w, 0);

    for (i = 0; i < 4; i++) {
        for (j = 0; j < AES_NB_LEN; j++) {
            out[i+4*j] = state[AES_NB_LEN*i+j];
        }
    }
}

/**/
unsigned int  aesECB128Encrypt(unsigned char *in, unsigned char *out, unsigned char *key, unsigned int len)
{
    unsigned char *w; // expanded key
    unsigned int remain = len;
    unsigned char data[AES_BLOCK_LEN];
    unsigned int readed = 0;

    /* 入参合法性检查 */
    if(NULL == in || NULL == out || NULL == key || len == 0)
    {
        return 0;
    }

    w = (unsigned char *)malloc( AES_NB_LEN *(AES_NR_LEN + 1)*4);
    if(NULL == w)
    {
        return 0;
    }

    key_expansion(key, w);

    while(readed < len)
    {
        if(remain < AES_BLOCK_LEN)
        {
            memcpy(data, in + readed, remain);
            memset(data + remain, AES_BLOCK_LEN - remain, AES_BLOCK_LEN - remain);
        }
        else
        {
            memcpy(data, in + readed, AES_BLOCK_LEN);
        }

        cipher(data, out + readed, w);

        readed += AES_BLOCK_LEN;
        remain -= AES_BLOCK_LEN;
    }

    free(w);

    return readed;
}

unsigned int  aesECB128Decrypt(unsigned char *in, unsigned char *out, unsigned char *key, unsigned int len)
{
    unsigned char *w=NULL; // expanded key
    unsigned char data[AES_BLOCK_LEN];
    unsigned int readed = 0;
    int i;
    int repeat = 1;

    /* 入参合法性检查 */
    if(NULL == in || NULL == out || NULL == key || len == 0)
    {
        return 0;
    }

    w = (unsigned char *)malloc( AES_NB_LEN *(AES_NR_LEN + 1)*4);
    if(NULL == w)
    {
        return 0;
    }

    key_expansion(key, w);

    while(readed + AES_BLOCK_LEN <= len)
    {

        memcpy(data, in + readed, AES_BLOCK_LEN);

        inv_cipher(data, out + readed, w);

        readed += AES_BLOCK_LEN;
    }

    /* 去除补码的数据 */
    for(i = 0; i < AES_BLOCK_LEN - 2; i++)
    {
        if(out[readed - i - 1] == out[readed - i - 2])
        {
            repeat++;
        }
        else
        {
            if(repeat == out[readed - i - 1])
            {
                /* 是补码数据 */
                readed -= repeat;
            }
            break;
        }
    }

    free(w);

    return readed;
}

unsigned int  aesECB128DecryptRaw(unsigned char *in, unsigned char *out, unsigned char *key, unsigned int len)
{
    unsigned char *w; // expanded key
    unsigned char data[AES_BLOCK_LEN];
    unsigned int readed = 0;

    /* 入参合法性检查 */
    if(NULL == in || NULL == out || NULL == key || len == 0)
    {
        return 0;
    }

    w = (unsigned char *)malloc( AES_NB_LEN *(AES_NR_LEN + 1)*4);
    if(NULL == w)
    {
        return 0;
    }

    key_expansion(key, w);

    while(readed < len)
    {

        memcpy(data, in + readed, AES_BLOCK_LEN);

        inv_cipher(data, out + readed, w);

        readed += AES_BLOCK_LEN;
    }

    free(w);

    return readed;
}



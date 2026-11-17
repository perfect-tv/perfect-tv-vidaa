#include "qr.h"
#include <string.h>

#define DATA_BYTES 108
#define ECC_BYTES 26

static int gmul(int a, int b) {
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((b >> i) & 1) * a;
    }
    return z & 0xff;
}

/* Остаток Рида — Соломона: 26 байт коррекции для одного блока данных */
static void ecc(const uint8_t *data, int n, uint8_t *rem) {
    int div[ECC_BYTES] = {0};
    div[ECC_BYTES - 1] = 1;
    int root = 1;
    for (int i = 0; i < ECC_BYTES; i++) {
        for (int j = 0; j < ECC_BYTES; j++) {
            div[j] = gmul(div[j], root);
            if (j + 1 < ECC_BYTES) div[j] ^= div[j + 1];
        }
        root = gmul(root, 2);
    }
    memset(rem, 0, ECC_BYTES);
    for (int i = 0; i < n; i++) {
        int f = data[i] ^ rem[0];
        memmove(rem, rem + 1, ECC_BYTES - 1);
        rem[ECC_BYTES - 1] = 0;
        for (int j = 0; j < ECC_BYTES; j++) rem[j] ^= (uint8_t)gmul(div[j], f);
    }
}

typedef struct { uint8_t (*m)[QR_SIZE]; uint8_t fn[QR_SIZE][QR_SIZE]; } qrm;

static void set(qrm *q, int x, int y, int black) {
    if (x < 0 || x >= QR_SIZE || y < 0 || y >= QR_SIZE) return;
    q->m[y][x] = (uint8_t)(black != 0);
    q->fn[y][x] = 1;
}
static void finder(qrm *q, int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int r = dx < 0 ? -dx : dx, ry = dy < 0 ? -dy : dy;
            if (ry > r) r = ry;
            set(q, cx + dx, cy + dy, r != 2 && r != 4);
        }
}
/* формат: уровень L (01) и маска, BCH(15,5) */
static void format(qrm *q, int mask) {
    int value = (1 << 3) | mask, rem = value;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int f = ((value << 10) | rem) ^ 0x5412;
#define B(i) ((f >> (i)) & 1)
    for (int i = 0; i <= 5; i++) set(q, 8, i, B(i));
    set(q, 8, 7, B(6));
    set(q, 8, 8, B(7));
    set(q, 7, 8, B(8));
    for (int i = 9; i < 15; i++) set(q, 14 - i, 8, B(i));
    for (int i = 0; i < 8; i++) set(q, QR_SIZE - 1 - i, 8, B(i));
    for (int i = 8; i < 15; i++) set(q, 8, QR_SIZE - 15 + i, B(i));
    set(q, 8, QR_SIZE - 8, 1);
#undef B
}

int qr_encode(const char *text, uint8_t m[QR_SIZE][QR_SIZE]) {
    size_t len = strlen(text);
    if (len > 106) return -1;
    uint8_t cw[DATA_BYTES + ECC_BYTES];
    memset(cw, 0, sizeof cw);
    int bit = 0;
#define PUT(v, w) for (int i_ = (w) - 1; i_ >= 0; i_--, bit++) if (((v) >> i_) & 1) cw[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7))
    PUT(4, 4);                 /* байтовый режим */
    PUT((int)len, 8);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c > 127) return -1;
        PUT(c, 8);
    }
#undef PUT
    bit += 4;                  /* терминатор */
    if (bit > DATA_BYTES * 8) bit = DATA_BYTES * 8;
    int n = (bit + 7) / 8;
    for (int i = n; i < DATA_BYTES; i++) cw[i] = (i - n) % 2 ? 0x11 : 0xEC;
    ecc(cw, DATA_BYTES, cw + DATA_BYTES);

    qrm q;
    memset(&q, 0, sizeof q);
    q.m = m;
    memset(m, 0, QR_SIZE * QR_SIZE);
    for (int i = 0; i < QR_SIZE; i++) { set(&q, 6, i, i % 2 == 0); set(&q, i, 6, i % 2 == 0); }
    finder(&q, 3, 3);
    finder(&q, QR_SIZE - 4, 3);
    finder(&q, 3, QR_SIZE - 4);
    for (int dy = -2; dy <= 2; dy++)   /* метка выравнивания версии 5 — (30, 30) */
        for (int dx = -2; dx <= 2; dx++) {
            int r = dx < 0 ? -dx : dx, ry = dy < 0 ? -dy : dy;
            if (ry > r) r = ry;
            set(&q, 30 + dx, 30 + dy, r != 1);
        }
    format(&q, 0);             /* занять места формата */
    int total = (DATA_BYTES + ECC_BYTES) * 8;
    bit = 0;
    for (int right = QR_SIZE - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < QR_SIZE; vert++) {
            int y = ((right + 1) & 2) == 0 ? QR_SIZE - 1 - vert : vert;
            for (int dx = 0; dx < 2; dx++) {
                int x = right - dx;
                if (q.fn[y][x]) continue;
                if (bit < total) m[y][x] = (cw[bit >> 3] >> (7 - (bit & 7))) & 1;
                bit++;
            }
        }
    }
    for (int y = 0; y < QR_SIZE; y++)   /* маска 0 */
        for (int x = 0; x < QR_SIZE; x++)
            if (!q.fn[y][x] && (x + y) % 2 == 0) m[y][x] ^= 1;
    format(&q, 0);
    return 0;
}

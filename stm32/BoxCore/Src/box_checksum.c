/* Line checksum: see box_checksum.h. */
#include "box_checksum.h"

#include <string.h>

/* CRC-8, polynomial 0x1D (x^8 + x^4 + x^3 + x^2 + 1), initial value 0x00,
 * most significant bit first, no final XOR. */
static unsigned crc_of(const char *text, size_t length)
{
    unsigned crc = 0x00u;
    size_t i;
    int bit;
    for (i = 0; i < length; i++) {
        crc ^= (unsigned char)text[i];
        for (bit = 0; bit < 8; bit++) {
            crc = ((crc << 1) ^ ((crc & 0x80u) ? 0x1Du : 0x00u)) & 0xFFu;
        }
    }
    return crc;
}

/* The value of one hex digit, either case, or -1. */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

void box_checksum_append(char *line)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t length = strlen(line);
    unsigned sum = crc_of(line, length);

    line[length++] = '*';
    line[length++] = hex[sum >> 4];
    line[length++] = hex[sum & 0x0Fu];
    line[length++] = '\n';
    line[length] = '\0';
}

bool box_checksum_check(const char *line, size_t *body_length)
{
    size_t length = strlen(line);
    size_t star;
    int high;
    int low;

    if (length < 3u || line[length - 3u] != '*') {
        return false;
    }
    star = length - 3u;
    high = hex_value(line[star + 1u]);
    low = hex_value(line[star + 2u]);
    if (high < 0 || low < 0 || (unsigned)(high * 16 + low) != crc_of(line, star)) {
        return false;
    }
    *body_length = star;
    return true;
}

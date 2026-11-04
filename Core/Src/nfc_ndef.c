#include "nfc_ndef.h"
#include <string.h>

size_t NfcNdef_BuildUri(const char *url, uint8_t *output, size_t capacity)
{
    if (!url || !output) return 0;
    size_t length = strlen(url);
    size_t skip;
    uint8_t code;
    if (strncmp(url, "http://", 7) == 0) { skip = 7; code = 0x03; }
    else if (strncmp(url, "https://", 8) == 0) { skip = 8; code = 0x04; }
    else return 0;
    if (length <= skip || length - skip > 240u) return 0;
    for (size_t i = 0; i < length; ++i)
        if ((uint8_t)url[i] <= 0x20u || (uint8_t)url[i] >= 0x7fu) return 0;
    size_t suffix = length - skip;
    size_t total = suffix + 12u;
    if (capacity < total) return 0;
    /* AN4911: E1 40 40 00 is the smartphone CC for the 512-byte ST25DV04K. */
    const uint8_t header[] = {0xe1, 0x40, 0x40, 0x00, 0x03, 0,
                             0xd1, 0x01, 0, 0x55, 0};
    memcpy(output, header, sizeof header);
    output[5] = (uint8_t)(suffix + 5u);
    output[8] = (uint8_t)(suffix + 1u);
    output[10] = code;
    memcpy(output + 11, url + skip, suffix);
    output[11 + suffix] = 0xfe;
    return total;
}

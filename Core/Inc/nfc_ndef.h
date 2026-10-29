#ifndef NFC_NDEF_H
#define NFC_NDEF_H
#include <stddef.h>
#include <stdint.h>
/* NFC Forum Type 5 capability container + short NDEF URI + terminator. */
size_t NfcNdef_BuildUri(const char *url, uint8_t *output, size_t capacity);
#endif

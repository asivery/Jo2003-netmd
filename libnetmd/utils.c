/*
 * utils.c
 *
 * This file is part of libnetmd, a library for accessing Sony NetMD devices.
 *
 * Copyright (C) 2011 Alexander Sulfrian
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <string.h>
#include "utils.h"
#include "log.h"

inline unsigned char proper_to_bcd_single(unsigned char value)
{
    unsigned char high, low;

    low = (value % 10) & 0xf;
    high = (((value / 10) % 10) * 0x10U) & 0xf0;

    return high | low;
}

inline unsigned char* proper_to_bcd(unsigned int value, unsigned char* target, size_t len)
{
    while (value > 0 && len > 0) {
        target[len - 1] = proper_to_bcd_single(value & 0xff);
        value /= 100;
        len--;
    }

    return target;
}

inline unsigned char bcd_to_proper_single(unsigned char value)
{
    unsigned char high, low;

    high = (value & 0xf0) >> 4;
    low = (value & 0xf);

    return ((high * 10U) + low) & 0xff;
}

inline unsigned int bcd_to_proper(unsigned char* value, size_t len)
{
    unsigned int result = 0;
    unsigned int nibble_value = 1;

    for (; len > 0; len--) {
        result += nibble_value * bcd_to_proper_single(value[len - 1]);

        nibble_value *= 100;
    }

    return result;
}

void netmd_check_response_bulk(netmd_response *response, const unsigned char* const expected,
                               const size_t expected_length, netmd_error *error)
{
    unsigned char *current;

    /* only check if there was no error before */
    if (*error == NETMD_NO_ERROR) {

        if ((response->length - response->position) < expected_length) {
            *error = NETMD_RESPONSE_TO_SHORT;
        }
        else {
            current = response->content + response->position;

            if (memcmp(current, expected, expected_length) == 0) {
                response->position += expected_length;
            }
            else {
                netmd_log_hex(0, current, expected_length);
                netmd_log_hex(0, expected, expected_length);
                *error = NETMD_RESPONSE_NOT_EXPECTED;
            }
        }
    }
}

void netmd_check_response_word(netmd_response *response, const uint16_t expected,
                               netmd_error *error)
{
    unsigned char buf[2];
    unsigned char *tmp = buf;

    /* only check if there was no error before */
    if (*error == NETMD_NO_ERROR) {
        if ((response->length - response->position) < 2) {
            *error = NETMD_RESPONSE_TO_SHORT;
        }
        else {
            netmd_copy_word_to_buffer(&tmp, expected, 0);
            netmd_check_response_bulk(response, buf, 2, error);
        }
    }
}

void netmd_check_response_doubleword(netmd_response *response, const uint32_t expected,
                                     netmd_error *error)
{
    unsigned char buf[4];
    unsigned char *tmp = buf;

    /* only check if there was no error before */
    if (*error == NETMD_NO_ERROR) {
        if ((response->length - response->position) < 4) {
            *error = NETMD_RESPONSE_TO_SHORT;
        }
        else {
            netmd_copy_doubleword_to_buffer(&tmp, expected, 0);
            netmd_check_response_bulk(response, buf, 4, error);
        }
    }
}


void netmd_check_response(netmd_response *response, const unsigned char expected,
                          netmd_error *error)
{
    /* only check if there was no error before */
    if (*error == NETMD_NO_ERROR) {

        if ((response->length - response->position) < 1) {
            *error = NETMD_RESPONSE_TO_SHORT;
        }
        else {
            if (response->content[response->position] == expected) {
                response->position++;
            }
            else {
                netmd_log_hex(0, response->content + response->position, 1);
                netmd_log_hex(0, &expected, 1);
                *error = NETMD_RESPONSE_NOT_EXPECTED;
            }
        }
    }
}

void netmd_read_response_bulk(netmd_response *response, unsigned char* target,
                              const size_t length, netmd_error *error)
{
    /* only copy if there was no error before */
    if (*error == NETMD_NO_ERROR) {

        if ((response->length - response->position) < length) {
            *error = NETMD_RESPONSE_TO_SHORT;
        }
        else {
            if (target) {
                memcpy(target, response->content + response->position, length);
            }

            response->position += length;
        }
    }
}

unsigned char *netmd_copy_word_to_buffer(unsigned char **buf, uint16_t value, int little_endian)
{
    if (little_endian == 0) {
        **buf = (unsigned char)((value >> 8) & 0xff);
        (*buf)++;
    }

    **buf = (unsigned char)((value >> 0) & 0xff);
    (*buf)++;

    if (little_endian == 1) {
        **buf = (unsigned char)((value >> 8) & 0xff);
        (*buf)++;
    }

    return *buf;
}

unsigned char *netmd_copy_doubleword_to_buffer(unsigned char **buf, uint32_t value, int little_endian)
{
    int8_t diff = 8;
    int bit = 24;
    int i;

    if (little_endian == 1) {
        diff = -8;
        bit = 0;
    }

    for (i = 0; i < 4; i++, bit = (bit - diff) & 0xff) {
        **buf = (unsigned char)(value >> bit) & 0xff;
        (*buf)++;
    }

    return *buf;
}

unsigned char *netmd_copy_quadword_to_buffer(unsigned char **buf, uint64_t value)
{
    **buf = (value >> 56) & 0xff;
    (*buf)++;

    **buf = (value >> 48) & 0xff;
    (*buf)++;

    **buf = (value >> 40) & 0xff;
    (*buf)++;

    **buf = (value >> 32) & 0xff;
    (*buf)++;

    **buf = (value >> 24) & 0xff;
    (*buf)++;

    **buf = (value >> 16) & 0xff;
    (*buf)++;

    **buf = (value >> 8) & 0xff;
    (*buf)++;

    **buf = (value >> 0) & 0xff;
    (*buf)++;

    return *buf;
}

/* TODO: add error */

unsigned char netmd_read(netmd_response *response)
{
    return response->content[response->position++];
}

uint16_t netmd_read_word(netmd_response *response)
{
    int i;
    uint16_t value;

    value = 0;
    for (i = 0; i < 2; i++) {
        value = (((unsigned int)value << 8U) | ((unsigned int)response->content[response->position] & 0xff)) & 0xffff;
        response->position++;
    }

    return value;
}

uint32_t netmd_read_doubleword(netmd_response *response)
{
    int i;
    uint32_t value;

    value = 0;
    for (i = 0; i < 4; i++) {
        value <<= 8;
        value += (response->content[response->position] & 0xff);
        response->position++;
    }

    return value;
}

uint64_t netmd_read_quadword(netmd_response *response)
{
    int i;
    uint64_t value;

    value = 0;
    for (i = 0; i < 8; i++) {
        value <<= 8;
        value += (response->content[response->position] & 0xff);
        response->position++;
    }

    return value;
}

static int bigEndian()
{
    unsigned char buf[2];
    uint16_t *pU16 = (uint16_t*)buf;

    *pU16 = 0x8811;

    return (buf[0] == 0x88) ? 1 : 0;
}

static uint16_t byteSwop(uint16_t in)
{
    return ((in & 0xff) << 8) | ((in >> 8) & 0xff);
}

//------------------------------------------------------------------------------
//! @brief      htons short and dirty
//!
//! @param[in]  in    value to convert
//!
//! @return     converted or original value
//------------------------------------------------------------------------------
uint16_t netmd_htons(uint16_t in)
{
    if (bigEndian() == 0)
    {
        // little endian -> swop bytes
        return byteSwop(in);
    }
    return in;
}

//------------------------------------------------------------------------------
//! @brief      ntohs short and dirty
//!
//! @param[in]  in    value to convert
//!
//! @return     converted or original value
//------------------------------------------------------------------------------
uint16_t netmd_ntohs(uint16_t in)
{
    // same same and not different ...
    return netmd_htons(in);
}

//------------------------------------------------------------------------------
//! @brief      iconvert a c string
//!
//! @param[in]  cd         conversion descriptor
//! @param[in]  in         c string to convert
//! @param      out        converted string (must be freed)
//!
//! @return     0 -> ok; else -> error
//------------------------------------------------------------------------------
static int str_iconv(iconv_t cd, ICONV_CONST char *in, char **out)
{
    size_t inlen, outlen;
    int buflen, rc;
    int len;                    /* number of chars in buffer */
    char *buf;

    inlen = strlen(in);
    buflen = 0;
    buf = NULL;
    do {
        outlen = inlen * 2;
        buflen += outlen;
        /* iconv() below changes the buf pointer:
         * - decrement to point at beginning of buffer before realloc
         * - re-increment to point at first free position after realloc
         */
        len = buflen - outlen;
        buf = (char*)realloc(buf - len, buflen) + len;
        if (buf == NULL) {
            /* XXX: report out of memory error */
            return FALSE;
        }
        rc = iconv(cd, &in, &inlen, &buf, &outlen);
        if ((rc == -1) && (errno != E2BIG)) {
            free(buf);
            return FALSE;       /* conversion failed */
        }
    } while (inlen != 0);
    len = buflen - outlen;
    buf -= len;                 /* reposition at begin of buffer */
    /* make a copy just big enough for the result */
    *out = malloc(len + 1);
    memcpy(*out, buf, len);
    *(*out + len) = '\0';
    free(buf);

    return TRUE;
}

//------------------------------------------------------------------------------
//! @brief      convert from utf8 to shift-jis
//!
//! @param[in]  in   c string to convert
//! @param      out  converted c string (need free)
//!
//! @return     0 -> ok; else -> error
//------------------------------------------------------------------------------
int utf8_to_shjis(ICONV_CONST char *in, char **out)
{
    iconv_t icv = iconv_open("SHIFT-JIS//IGNORE", "UTF-8");
    return str_iconv(icv, in, out);
}

//------------------------------------------------------------------------------
//! @brief      convert from shift-jis to utf8
//!
//! @param[in]  in   c string to convert
//! @param      out  converted c string (need free)
//!
//! @return     0 -> ok; else -> error
//------------------------------------------------------------------------------
int shjis_to_utf8(ICONV_CONST char *in, char **out)
{
    iconv_t icv = iconv_open("UTF-8", "SHIFT-JIS");
    return str_iconv(icv, in, out);
}

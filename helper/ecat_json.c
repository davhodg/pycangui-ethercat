/*!
    \file   ecat_json.c
    \brief  The little JSON the protocol needs: flat objects in, answers out.

    \author davhodg
    \copyright Copyright (C) 2026 davhodg

    This program is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
    more details.

    You should have received a copy of the GNU General Public License along
    with this program. If not, see <https://www.gnu.org/licenses/>.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "ecat_json.h"

#include <string.h>

/*============================================================================*/
/*! \name Types */
/*============================================================================*/
/*!@{*/

/*! A position in the line being read. */
typedef struct
{
    char const* line_p; /*!< The line */
    size_t      len;    /*!< Its length */
    size_t      pos;    /*!< The next character to read */
} SCAN_T;

/*!@}*/

/*============================================================================*/
/*! \name Static prototypes */
/*============================================================================*/
/*!@{*/

static inline bool at_end( SCAN_T const* const scan_p );
static inline char peek( SCAN_T const* const scan_p );
static void skip_space( SCAN_T* const scan_p );
static bool take( SCAN_T* const scan_p, char const wanted );
static bool take_word( SCAN_T* const scan_p, char const word[] );
static ecat_json_sts_t read_string( SCAN_T* const scan_p, char text[], size_t const max );
static bool read_number( SCAN_T* const scan_p, uint32_t* const number_p );
static int32_t hex_digit( char const c );
static bool put_utf8( uint32_t const code, char text[], size_t const max, size_t* const used_p );
static ecat_json_sts_t read_field( SCAN_T* const scan_p, ECAT_JSON_FIELD_T* const field_p );
static void put_raw( ECAT_JSON_OUT_T* const out_p, char const text[], size_t const len );
static void put_key( ECAT_JSON_OUT_T* const out_p, char const key[] );
static void put_escaped( ECAT_JSON_OUT_T* const out_p, char const text[] );

/*!@}*/

/*============================================================================*/
/*! \name Public functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_json.h.
*/
ecat_json_sts_t ecat_json_parse( char const line[], size_t const len, ECAT_JSON_OBJ_T* const obj_p )
{
    SCAN_T scan = { .line_p = line, .len = len, .pos = 0U };
    ecat_json_sts_t sts = ECAT_JSON_STS_OK;
    bool more = true;

    obj_p->count = 0U;
    skip_space( &scan );
    if( !take( &scan, '{' ) )
    {
        sts = ECAT_JSON_STS_SYNTAX;
    }
    else
    {
        skip_space( &scan );
        if( take( &scan, '}' ) )
        {
            more = false;
        }
        else
        {
            /* An object with fields: carry on below. */
        }

        while( more && ( ECAT_JSON_STS_OK == sts ) )
        {
            if( ECAT_JSON_MAX_FIELDS == obj_p->count )
            {
                sts = ECAT_JSON_STS_TOO_MANY;
            }
            else
            {
                sts = read_field( &scan, &obj_p->fields[obj_p->count] );
            }

            if( ECAT_JSON_STS_OK == sts )
            {
                obj_p->count++;
                skip_space( &scan );
                if( take( &scan, '}' ) )
                {
                    more = false;
                }
                else if( !take( &scan, ',' ) )
                {
                    sts = ECAT_JSON_STS_SYNTAX;
                }
                else
                {
                    /* A comma: another field follows. */
                }
            }
            else
            {
                /* sts says why: the loop ends. */
            }
        }

        if( ECAT_JSON_STS_OK == sts )
        {
            skip_space( &scan );
            if( !at_end( &scan ) )
            {
                sts = ECAT_JSON_STS_SYNTAX;
            }
            else
            {
                /* Nothing after the object: read. */
            }
        }
        else
        {
            /* Already failed. */
        }
    }

    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_json.h.
*/
ECAT_JSON_FIELD_T const* ecat_json_get( ECAT_JSON_OBJ_T const* const obj_p,
                                        char const key[],
                                        ecat_json_kind_t const kind )
{
    ECAT_JSON_FIELD_T const* found_p = NULL;

    for( size_t idx = 0U; ( idx < obj_p->count ) && ( NULL == found_p ); idx++ )
    {
        ECAT_JSON_FIELD_T const* const field_p = &obj_p->fields[idx];

        if( ( kind == field_p->kind ) && ( 0 == strcmp( field_p->key, key ) ) )
        {
            found_p = field_p;
        }
        else
        {
            /* Not this one. */
        }
    }

    return found_p;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_json.h.
*/
void ecat_json_out_reset( ECAT_JSON_OUT_T* const out_p )
{
    out_p->buf[0U] = '\0';
    out_p->len = 0U;
    out_p->overflow = false;
    out_p->first = true;
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_json.h.
*/
void ecat_json_out_open( ECAT_JSON_OUT_T* const out_p, char const key[], bool const list )
{
    put_key( out_p, key );
    put_raw( out_p, list ? "[" : "{", 1U );
    out_p->first = true;
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_json.h.
*/
void ecat_json_out_close( ECAT_JSON_OUT_T* const out_p, bool const list )
{
    put_raw( out_p, list ? "]" : "}", 1U );
    out_p->first = false;
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_json.h.
*/
void ecat_json_out_str( ECAT_JSON_OUT_T* const out_p, char const key[], char const text[] )
{
    put_key( out_p, key );
    put_raw( out_p, "\"", 1U );
    put_escaped( out_p, text );
    put_raw( out_p, "\"", 1U );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_json.h.
*/
void ecat_json_out_u32( ECAT_JSON_OUT_T* const out_p, char const key[], uint32_t const number )
{
    char digits[10U];
    size_t count = 0U;
    uint32_t rest = number;

    /* Written backwards, least significant first, then put out in reverse. */
    do
    {
        digits[count] = (char)( '0' + (char)( rest % 10U ) );
        count++;
        rest /= 10U;
    } while( 0U != rest );

    put_key( out_p, key );
    while( 0U != count )
    {
        count--;
        put_raw( out_p, &digits[count], 1U );
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_json.h.
*/
void ecat_json_out_bool( ECAT_JSON_OUT_T* const out_p, char const key[], bool const value )
{
    put_key( out_p, key );
    if( value )
    {
        put_raw( out_p, "true", 4U );
    }
    else
    {
        put_raw( out_p, "false", 5U );
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_json.h.
*/
void ecat_json_out_hex( ECAT_JSON_OUT_T* const out_p,
                        char const key[],
                        uint8_t const data[],
                        size_t const len )
{
    static char const digits[] = "0123456789abcdef";

    put_key( out_p, key );
    put_raw( out_p, "\"", 1U );
    for( size_t idx = 0U; idx < len; idx++ )
    {
        put_raw( out_p, &digits[data[idx] >> 4U], 1U );
        put_raw( out_p, &digits[data[idx] & 0x0FU], 1U );
    }
    put_raw( out_p, "\"", 1U );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_json.h.
*/
bool ecat_json_hex_read( char const text[], uint8_t data[], size_t const max, size_t* const len_p )
{
    size_t const digits = strlen( text );
    bool ok = ( 0U == ( digits % 2U ) ) && ( ( digits / 2U ) <= max );

    *len_p = 0U;
    for( size_t idx = 0U; ok && ( idx < digits ); idx += 2U )
    {
        int32_t const high = hex_digit( text[idx] );
        int32_t const low = hex_digit( text[idx + 1U] );

        if( ( high < 0 ) || ( low < 0 ) )
        {
            ok = false;
        }
        else
        {
            data[*len_p] = (uint8_t)( ( high * 16 ) + low );
            ( *len_p )++;
        }
    }

    return ok;
}

/*!@}*/

/*============================================================================*/
/*! \name Static functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Whether the whole line has been read.

    \param  scan_p  The position.

    \return true at the end.
*/
static inline bool at_end( SCAN_T const* const scan_p )
{
    return ( scan_p->pos >= scan_p->len );
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  The next character, without taking it.

    \param  scan_p  The position.

    \return The character, or '\0' at the end.
*/
static inline char peek( SCAN_T const* const scan_p )
{
    return at_end( scan_p ) ? '\0' : scan_p->line_p[scan_p->pos];
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Move past spaces, tabs and carriage returns.

    \param  scan_p  The position.

    \return None.
*/
static void skip_space( SCAN_T* const scan_p )
{
    while( !at_end( scan_p ) && ( ( ' ' == peek( scan_p ) ) || ( '\t' == peek( scan_p ) ) ||
                                  ( '\r' == peek( scan_p ) ) ) )
    {
        scan_p->pos++;
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Take one character if it is the one wanted.

    \param  scan_p  The position.
    \param  wanted  The character.

    \return true when it was there and has been taken.
*/
static bool take( SCAN_T* const scan_p, char const wanted )
{
    bool const there = ( !at_end( scan_p ) && ( wanted == peek( scan_p ) ) );

    if( there )
    {
        scan_p->pos++;
    }
    else
    {
        /* Left where it is. */
    }
    return there;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Take a word, true or false, if it is next.

    \param  scan_p  The position.
    \param  word    The word, terminated.

    \return true when it was there and has been taken.
*/
static bool take_word( SCAN_T* const scan_p, char const word[] )
{
    size_t const len = strlen( word );
    bool const there = ( ( scan_p->len - scan_p->pos ) >= len ) &&
                       ( 0 == memcmp( &scan_p->line_p[scan_p->pos], word, len ) );

    if( there )
    {
        scan_p->pos += len;
    }
    else
    {
        /* Left where it is. */
    }
    return there;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Read a quoted string, unescaping it.

    \param  scan_p  The position, at the opening quote.
    \param  text    Filled with the string, terminated.
    \param  max     How many \p text holds, terminator included.

    \return ECAT_JSON_STS_OK, ECAT_JSON_STS_SYNTAX or ECAT_JSON_STS_TOO_LONG.
*/
static ecat_json_sts_t read_string( SCAN_T* const scan_p, char text[], size_t const max )
{
    ecat_json_sts_t sts = ECAT_JSON_STS_OK;
    size_t used = 0U;
    bool closed = false;

    if( !take( scan_p, '"' ) )
    {
        sts = ECAT_JSON_STS_SYNTAX;
    }
    else
    {
        /* Inside the string: read up to the closing quote below. */
    }

    while( ( ECAT_JSON_STS_OK == sts ) && !closed )
    {
        if( at_end( scan_p ) )
        {
            sts = ECAT_JSON_STS_SYNTAX;
        }
        else
        {
            char const c = peek( scan_p );

            scan_p->pos++;
            if( '"' == c )
            {
                closed = true;
            }
            else if( (unsigned char)c < 0x20U )
            {
                sts = ECAT_JSON_STS_SYNTAX;     /* control characters must be escaped */
            }
            else if( '\\' != c )
            {
                if( ( used + 1U ) >= max )
                {
                    sts = ECAT_JSON_STS_TOO_LONG;
                }
                else
                {
                    text[used] = c;
                    used++;
                }
            }
            else
            {
                char const e = peek( scan_p );
                char plain = '\0';

                scan_p->pos++;
                switch( e )
                {
                    case '"':
                    case '\\':
                    case '/':
                    {
                        plain = e;
                        break;
                    }
                    case 'b':
                    {
                        plain = '\b';
                        break;
                    }
                    case 'f':
                    {
                        plain = '\f';
                        break;
                    }
                    case 'n':
                    {
                        plain = '\n';
                        break;
                    }
                    case 'r':
                    {
                        plain = '\r';
                        break;
                    }
                    case 't':
                    {
                        plain = '\t';
                        break;
                    }
                    case 'u':
                    {
                        uint32_t code = 0U;

                        for( size_t idx = 0U; ( idx < 4U ) && ( ECAT_JSON_STS_OK == sts ); idx++ )
                        {
                            int32_t const digit = hex_digit( peek( scan_p ) );

                            if( digit < 0 )
                            {
                                sts = ECAT_JSON_STS_SYNTAX;
                            }
                            else
                            {
                                code = ( ( code * 16U ) + (uint32_t)digit );
                                scan_p->pos++;
                            }
                        }

                        if( ( ECAT_JSON_STS_OK == sts ) && ( 0U == code ) )
                        {
                            sts = ECAT_JSON_STS_SYNTAX;     /* a NUL would end the text early */
                        }
                        else if( ( ECAT_JSON_STS_OK == sts ) && !put_utf8( code, text, max, &used ) )
                        {
                            sts = ECAT_JSON_STS_TOO_LONG;
                        }
                        else
                        {
                            /* Written, or already failed. */
                        }
                        break;
                    }
                    default:
                    {
                        sts = ECAT_JSON_STS_SYNTAX;
                        break;
                    }
                }

                if( ( ECAT_JSON_STS_OK == sts ) && ( '\0' != plain ) )
                {
                    if( ( used + 1U ) >= max )
                    {
                        sts = ECAT_JSON_STS_TOO_LONG;
                    }
                    else
                    {
                        text[used] = plain;
                        used++;
                    }
                }
                else
                {
                    /* \u was written above, or the escape failed. */
                }
            }
        }
    }

    text[used] = '\0';
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Read a non-negative integer that fits 32 bits.

    \param  scan_p      The position, at the first digit.
    \param  number_p    The number.

    \return true when there was one, with no fraction or exponent after it.
*/
static bool read_number( SCAN_T* const scan_p, uint32_t* const number_p )
{
    uint64_t value = 0U;
    size_t digits = 0U;
    bool ok = true;

    while( ok && !at_end( scan_p ) && ( peek( scan_p ) >= '0' ) && ( peek( scan_p ) <= '9' ) )
    {
        value = ( ( value * 10U ) + (uint64_t)( peek( scan_p ) - '0' ) );
        digits++;
        scan_p->pos++;
        if( value > UINT32_MAX )
        {
            ok = false;
        }
        else
        {
            /* Still fits. */
        }
    }

    if( ok && ( ( 0U == digits ) || ( ( digits > 1U ) && ( '0' == scan_p->line_p[scan_p->pos - digits] ) ) ) )
    {
        ok = false;     /* no digits, or a leading zero, which JSON does not allow */
    }
    else if( ok && ( ( '.' == peek( scan_p ) ) || ( 'e' == peek( scan_p ) ) || ( 'E' == peek( scan_p ) ) ) )
    {
        ok = false;     /* not an integer */
    }
    else
    {
        /* Read, or already failed. */
    }

    *number_p = (uint32_t)value;
    return ok;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  The value of one hex digit.

    \param  c   The character.

    \return 0 to 15, or -1 when it is not a hex digit.
*/
static int32_t hex_digit( char const c )
{
    int32_t value;

    if( ( c >= '0' ) && ( c <= '9' ) )
    {
        value = (int32_t)( c - '0' );
    }
    else if( ( c >= 'a' ) && ( c <= 'f' ) )
    {
        value = (int32_t)( ( c - 'a' ) + 10 );
    }
    else if( ( c >= 'A' ) && ( c <= 'F' ) )
    {
        value = (int32_t)( ( c - 'A' ) + 10 );
    }
    else
    {
        value = -1;
    }
    return value;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Write a character from a \\u escape as UTF-8.

    \param  code    The code point, 1 to 0xFFFF.
    \param  text    The string being written.
    \param  max     How many \p text holds, terminator included.
    \param  used_p  How much of \p text is used; moved on past what is written.

    \return true when it fitted.
*/
static bool put_utf8( uint32_t const code, char text[], size_t const max, size_t* const used_p )
{
    uint8_t bytes[3U];
    size_t count;

    if( code < 0x80U )
    {
        bytes[0U] = (uint8_t)code;
        count = 1U;
    }
    else if( code < 0x800U )
    {
        bytes[0U] = (uint8_t)( 0xC0U | ( code >> 6U ) );
        bytes[1U] = (uint8_t)( 0x80U | ( code & 0x3FU ) );
        count = 2U;
    }
    else
    {
        bytes[0U] = (uint8_t)( 0xE0U | ( code >> 12U ) );
        bytes[1U] = (uint8_t)( 0x80U | ( ( code >> 6U ) & 0x3FU ) );
        bytes[2U] = (uint8_t)( 0x80U | ( code & 0x3FU ) );
        count = 3U;
    }

    bool const fits = ( ( *used_p + count ) < max );

    if( fits )
    {
        for( size_t idx = 0U; idx < count; idx++ )
        {
            text[*used_p] = (char)bytes[idx];
            ( *used_p )++;
        }
    }
    else
    {
        /* Nothing written. */
    }
    return fits;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Read one "key": value pair.

    \param  scan_p  The position, before the key.
    \param  field_p Filled in.

    \return ECAT_JSON_STS_OK, or why not.
*/
static ecat_json_sts_t read_field( SCAN_T* const scan_p, ECAT_JSON_FIELD_T* const field_p )
{
    ecat_json_sts_t sts;

    skip_space( scan_p );
    sts = read_string( scan_p, field_p->key, ECAT_JSON_KEY_LEN );
    if( ECAT_JSON_STS_OK == sts )
    {
        skip_space( scan_p );
        if( !take( scan_p, ':' ) )
        {
            sts = ECAT_JSON_STS_SYNTAX;
        }
        else
        {
            skip_space( scan_p );
        }
    }
    else
    {
        /* The key could not be read. */
    }

    field_p->number = 0U;
    field_p->text[0U] = '\0';
    if( ECAT_JSON_STS_OK != sts )
    {
        /* Already failed. */
    }
    else if( '"' == peek( scan_p ) )
    {
        field_p->kind = ECAT_JSON_KIND_STRING;
        sts = read_string( scan_p, field_p->text, ECAT_JSON_TEXT_LEN );
    }
    else if( take_word( scan_p, "true" ) )
    {
        field_p->kind = ECAT_JSON_KIND_BOOL;
        field_p->number = 1U;
    }
    else if( take_word( scan_p, "false" ) )
    {
        field_p->kind = ECAT_JSON_KIND_BOOL;
    }
    else
    {
        field_p->kind = ECAT_JSON_KIND_NUMBER;
        if( !read_number( scan_p, &field_p->number ) )
        {
            sts = ECAT_JSON_STS_SYNTAX;
        }
        else
        {
            /* Read. */
        }
    }

    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Add text to an answer, or note that it did not fit.

    \param  out_p   The answer.
    \param  text    The text.
    \param  len     How long it is.

    \return None.
*/
static void put_raw( ECAT_JSON_OUT_T* const out_p, char const text[], size_t const len )
{
    if( out_p->overflow || ( ( out_p->len + len ) >= ECAT_JSON_OUT_LEN ) )
    {
        out_p->overflow = true;
    }
    else
    {
        ( void )memcpy( &out_p->buf[out_p->len], text, len );
        out_p->len += len;
        out_p->buf[out_p->len] = '\0';
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  The comma before a value, if one came before it, and its key.

    \param  out_p   The answer.
    \param  key     The key, or NULL for a value in a list.

    \return None.
*/
static void put_key( ECAT_JSON_OUT_T* const out_p, char const key[] )
{
    if( !out_p->first )
    {
        put_raw( out_p, ",", 1U );
    }
    else
    {
        out_p->first = false;
    }

    if( NULL != key )
    {
        put_raw( out_p, "\"", 1U );
        put_escaped( out_p, key );
        put_raw( out_p, "\":", 2U );
    }
    else
    {
        /* A value in a list: no key. */
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Add a string's characters, escaped as JSON needs.

    \param  out_p   The answer.
    \param  text    The string, terminated.

    \return None.
*/
static void put_escaped( ECAT_JSON_OUT_T* const out_p, char const text[] )
{
    static char const digits[] = "0123456789abcdef";

    for( size_t idx = 0U; '\0' != text[idx]; idx++ )
    {
        unsigned char const c = (unsigned char)text[idx];

        if( ( '"' == c ) || ( '\\' == c ) )
        {
            char const pair[2U] = { '\\', (char)c };

            put_raw( out_p, pair, 2U );
        }
        else if( c < 0x20U )
        {
            char const code[6U] = { '\\', 'u', '0', '0', digits[c >> 4U], digits[c & 0x0FU] };

            put_raw( out_p, code, 6U );
        }
        else
        {
            put_raw( out_p, &text[idx], 1U );
        }
    }
    return;
}

/*!@}*/

/*============================================================================*/

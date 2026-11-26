/*!
    \file   ecat_json.h
    \brief  The little JSON the protocol needs: flat objects in, answers out.

    A request is one flat object on one line: string keys, and values that are
    strings, non-negative integers, true or false. Nothing nests, so it is read
    in one pass with no recursion and no allocation. Answers do nest (a list of
    slaves), and are written with the ecat_json_out_*() functions into a fixed
    buffer that notes when it has overflowed rather than writing past its end.

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

#ifndef ECAT_JSON_H
#define ECAT_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*============================================================================*/
/*! \name Defines */
/*============================================================================*/
/*!@{*/

#define ECAT_JSON_MAX_FIELDS    ( 16U )     /*!< Most fields in one request */
#define ECAT_JSON_KEY_LEN       ( 16U )     /*!< Longest key, with its terminator */
#define ECAT_JSON_TEXT_LEN      ( 2048U )   /*!< Longest string value, with its terminator */
#define ECAT_JSON_OUT_LEN       ( 65536U )  /*!< Longest answer */

/*!@}*/

/*============================================================================*/
/*! \name Types */
/*============================================================================*/
/*!@{*/

/*! What a request line came to. */
typedef enum
{
    ECAT_JSON_STS_OK = 0,           /*!< Read */
    ECAT_JSON_STS_SYNTAX,           /*!< Not a flat JSON object */
    ECAT_JSON_STS_TOO_MANY,         /*!< More fields than ECAT_JSON_MAX_FIELDS */
    ECAT_JSON_STS_TOO_LONG          /*!< A key or a string longer than this reader holds */
} ecat_json_sts_t;

/*! The kind of a field's value. */
typedef enum
{
    ECAT_JSON_KIND_STRING = 0,      /*!< A string, unescaped into text */
    ECAT_JSON_KIND_NUMBER,          /*!< A non-negative integer that fits 32 bits */
    ECAT_JSON_KIND_BOOL             /*!< true or false */
} ecat_json_kind_t;

/*! One field of a request. */
typedef struct
{
    char             key[ECAT_JSON_KEY_LEN];    /*!< Its name */
    ecat_json_kind_t kind;                      /*!< What its value is */
    uint32_t         number;                    /*!< The value, for a number; 1 or 0 for a bool */
    char             text[ECAT_JSON_TEXT_LEN];  /*!< The value, for a string */
} ECAT_JSON_FIELD_T;

/*! A request, read. */
typedef struct
{
    ECAT_JSON_FIELD_T fields[ECAT_JSON_MAX_FIELDS]; /*!< The fields, in the order they came */
    size_t            count;                        /*!< How many there are */
} ECAT_JSON_OBJ_T;

/*! An answer being written. */
typedef struct
{
    char   buf[ECAT_JSON_OUT_LEN];  /*!< The text so far, always terminated */
    size_t len;                     /*!< How much of buf is used */
    bool   overflow;                /*!< Something did not fit: the answer is not to be sent */
    bool   first;                   /*!< Nothing written yet in the object or list now open */
} ECAT_JSON_OUT_T;

/*!@}*/

/*============================================================================*/
/*! \name Public functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Read one request line.

    \param  line    The line, without its newline; need not be terminated.
    \param  len     How long it is.
    \param  obj_p   Filled with its fields.

    \return ECAT_JSON_STS_OK, or why it could not be read.
*/
ecat_json_sts_t ecat_json_parse( char const line[], size_t const len, ECAT_JSON_OBJ_T* const obj_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  A field by its key.

    \param  obj_p   The request.
    \param  key     The key.
    \param  kind    The kind it must be.

    \return The field, or NULL when there is none of that key and kind.
*/
ECAT_JSON_FIELD_T const* ecat_json_get( ECAT_JSON_OBJ_T const* const obj_p,
                                        char const key[],
                                        ecat_json_kind_t const kind );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Start an answer, empty.

    \param  out_p   The answer.

    \return None.
*/
void ecat_json_out_reset( ECAT_JSON_OUT_T* const out_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Open an object or a list: "{" or "[", with a comma first if needed.

    \param  out_p   The answer.
    \param  key     The key it is the value of, or NULL inside a list or at the top.
    \param  list    true for a list, false for an object.

    \return None.
*/
void ecat_json_out_open( ECAT_JSON_OUT_T* const out_p, char const key[], bool const list );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Close the object or list opened last.

    \param  out_p   The answer.
    \param  list    true for a list, false for an object.

    \return None.
*/
void ecat_json_out_close( ECAT_JSON_OUT_T* const out_p, bool const list );

/*----------------------------------------------------------------------------*/
/*!
    \brief  A string field, escaped.

    \param  out_p   The answer.
    \param  key     Its key, or NULL inside a list.
    \param  text    The string, terminated.

    \return None.
*/
void ecat_json_out_str( ECAT_JSON_OUT_T* const out_p, char const key[], char const text[] );

/*----------------------------------------------------------------------------*/
/*!
    \brief  A number field.

    \param  out_p   The answer.
    \param  key     Its key, or NULL inside a list.
    \param  number  The number.

    \return None.
*/
void ecat_json_out_u32( ECAT_JSON_OUT_T* const out_p, char const key[], uint32_t const number );

/*----------------------------------------------------------------------------*/
/*!
    \brief  A true or false field.

    \param  out_p   The answer.
    \param  key     Its key, or NULL inside a list.
    \param  value   The value.

    \return None.
*/
void ecat_json_out_bool( ECAT_JSON_OUT_T* const out_p, char const key[], bool const value );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Bytes, as a string of hex digits two to a byte.

    \param  out_p   The answer.
    \param  key     Its key, or NULL inside a list.
    \param  data    The bytes.
    \param  len     How many.

    \return None.
*/
void ecat_json_out_hex( ECAT_JSON_OUT_T* const out_p,
                        char const key[],
                        uint8_t const data[],
                        size_t const len );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Read a string of hex digits, two to a byte.

    \param  text    The digits, terminated; upper or lower case.
    \param  data    Filled with the bytes.
    \param  max     How many \p data holds.
    \param  len_p   How many were read.

    \return true when \p text was an even number of hex digits that fitted.
*/
bool ecat_json_hex_read( char const text[], uint8_t data[], size_t const max, size_t* const len_p );

/*!@}*/

#endif /* #ifndef ECAT_JSON_H */

/*============================================================================*/

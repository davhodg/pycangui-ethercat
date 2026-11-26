/*!
    \file   ecat_helper.h
    \brief  The protocol: one request line in, one answer out.

    Everything the plugin can ask of the helper is handled here, and only here,
    with no socket and no thread: a line goes in and an answer comes out, so the
    whole protocol is tested by feeding it lines. docs/PROTOCOL.md describes it.

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

#ifndef ECAT_HELPER_H
#define ECAT_HELPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecat_json.h"
#include "ecat_port.h"

/*============================================================================*/
/*! \name Defines */
/*============================================================================*/
/*!@{*/

#define ECAT_HELPER_PROTOCOL        ( 1U )      /*!< The protocol version this helper speaks */
#define ECAT_HELPER_DATA_LEN        ( ( ECAT_JSON_TEXT_LEN - 1U ) / 2U )   /*!< Longest SDO value, in bytes: what fits a request as hex */
#define ECAT_HELPER_STATE_TIMEOUT_MS ( 2000U )  /*!< How long a state change is waited for, unless the request says */

/*!@}*/

/*============================================================================*/
/*! \name Types */
/*============================================================================*/
/*!@{*/

/*! The helper's state between requests. */
typedef struct
{
    ECAT_PORT_CTX_T*    port_p;                                 /*!< The master */
    bool                hello;                                  /*!< The hello has been answered: other requests are taken */
    ECAT_JSON_OBJ_T     request;                                /*!< The request being handled */
    uint8_t             data[ECAT_HELPER_DATA_LEN];             /*!< An SDO value on its way through */
    ECAT_PORT_ADAPTER_T adapters[ECAT_PORT_MAX_ADAPTERS];       /*!< The adapters, while they are listed */
} ECAT_HELPER_CTX_T;

/*!@}*/

/*============================================================================*/
/*! \name Public functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Set up a helper over a master, waiting for its hello.

    \param  ctx_p   The helper.
    \param  port_p  The master it works.

    \return None.
*/
void ecat_helper_init( ECAT_HELPER_CTX_T* const ctx_p, ECAT_PORT_CTX_T* const port_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Handle one request line.

    \param  ctx_p   The helper.
    \param  line    The line, without its newline.
    \param  len     How long it is.
    \param  out_p   Filled with the answer, one JSON object without a newline.

    \return None.
*/
void ecat_helper_handle( ECAT_HELPER_CTX_T* const ctx_p,
                         char const line[],
                         size_t const len,
                         ECAT_JSON_OUT_T* const out_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  The answer to a line too long to read, which has no id to answer with.

    \param  out_p   Filled with the answer.

    \return None.
*/
void ecat_helper_too_long( ECAT_JSON_OUT_T* const out_p );

/*!@}*/

#endif /* #ifndef ECAT_HELPER_H */

/*============================================================================*/

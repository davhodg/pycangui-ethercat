/*!
    \file   ecat_helper.c
    \brief  The protocol: one request line in, one answer out.

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

#include "ecat_helper.h"

#include <string.h>

/*============================================================================*/
/*! \name Types */
/*============================================================================*/
/*!@{*/

/*! A state's number and the name the protocol gives it. */
typedef struct
{
    uint16_t    state;  /*!< ECAT_PORT_STATE_* */
    char const* name_p; /*!< Its name in requests and answers */
} STATE_NAME_T;

/*!@}*/

/*============================================================================*/
/*! \name Static prototypes */
/*============================================================================*/
/*!@{*/

static void handle( ECAT_HELPER_CTX_T* const ctx_p, char const op[], ECAT_JSON_OUT_T* const out_p );
static void do_hello( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p );
static void do_adapters( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p );
static void do_open( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p );
static void do_scan( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p );
static void do_states( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p );
static void do_set_state( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p );
static void do_sdo_read( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p );
static void do_sdo_write( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p );
static void do_map( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p );
static void do_pdo_write( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p );
static void do_process( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p );
static void do_stop( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p );
static void unmap( ECAT_HELPER_CTX_T* const ctx_p );
static void read_entries( ECAT_HELPER_CTX_T* const ctx_p );
static void read_assignment( ECAT_HELPER_CTX_T* const ctx_p,
                             uint16_t const position,
                             uint16_t const assignment,
                             bool const out,
                             uint32_t const first_bit );
static bool sdo_number( ECAT_HELPER_CTX_T* const ctx_p,
                        uint16_t const position,
                        uint16_t const index,
                        uint8_t const sub,
                        uint32_t* const value_p );
static ECAT_HELPER_ENTRY_T const* find_entry( ECAT_HELPER_CTX_T const* const ctx_p,
                                              uint16_t const slave,
                                              uint16_t const index,
                                              uint8_t const sub );
static bool get_number( ECAT_HELPER_CTX_T const* const ctx_p,
                        char const key[],
                        uint32_t const max,
                        uint32_t* const number_p );
static bool get_object( ECAT_HELPER_CTX_T const* const ctx_p,
                        uint16_t* const slave_p,
                        uint16_t* const index_p,
                        uint8_t* const sub_p );
static void put_slaves( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p, bool const full );
static void put_ok( ECAT_JSON_OUT_T* const out_p, ECAT_JSON_FIELD_T const* const id_p );
static void put_error( ECAT_JSON_OUT_T* const out_p, ECAT_JSON_FIELD_T const* const id_p, char const code[] );
static void put_error_open( ECAT_JSON_OUT_T* const out_p, ECAT_JSON_FIELD_T const* const id_p, char const code[] );
static void put_port_error( ECAT_JSON_OUT_T* const out_p,
                            ECAT_JSON_FIELD_T const* const id_p,
                            ecat_port_sts_t const sts,
                            uint32_t const abort_code );
static void put_port_error_open( ECAT_JSON_OUT_T* const out_p,
                                 ECAT_JSON_FIELD_T const* const id_p,
                                 ecat_port_sts_t const sts,
                                 uint32_t const abort_code );
static char const* state_name( uint16_t const state );
static bool state_number( char const name[], uint16_t* const state_p );

/*!@}*/

/*============================================================================*/
/*! \name Static data */
/*============================================================================*/
/*!@{*/

/*! The states and their names, in the order a pane offers them. */
static STATE_NAME_T const state_names[] =
{
    { ECAT_PORT_STATE_INIT, "INIT" },
    { ECAT_PORT_STATE_PREOP, "PREOP" },
    { ECAT_PORT_STATE_BOOT, "BOOT" },
    { ECAT_PORT_STATE_SAFEOP, "SAFEOP" },
    { ECAT_PORT_STATE_OP, "OP" }
};

/*!@}*/

/*============================================================================*/
/*! \name Public functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_helper.h.
*/
void ecat_helper_init( ECAT_HELPER_CTX_T* const ctx_p, ECAT_PORT_CTX_T* const port_p )
{
    ctx_p->port_p = port_p;
    ctx_p->hello = false;
    ctx_p->request.count = 0U;
    unmap( ctx_p );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_helper.h.
*/
void ecat_helper_handle( ECAT_HELPER_CTX_T* const ctx_p,
                         char const line[],
                         size_t const len,
                         ECAT_JSON_OUT_T* const out_p )
{
    ecat_json_sts_t const sts = ecat_json_parse( line, len, &ctx_p->request );

    ecat_json_out_reset( out_p );
    if( ECAT_JSON_STS_TOO_LONG == sts )
    {
        ecat_helper_too_long( out_p );
    }
    else if( ECAT_JSON_STS_OK != sts )
    {
        put_error( out_p, NULL, "bad_request" );
    }
    else
    {
        ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );
        ECAT_JSON_FIELD_T const* const op_p = ecat_json_get( &ctx_p->request, "op", ECAT_JSON_KIND_STRING );

        if( ( NULL == id_p ) || ( NULL == op_p ) )
        {
            put_error( out_p, id_p, "bad_request" );
        }
        else if( ( !ctx_p->hello ) && ( 0 != strcmp( op_p->text, "hello" ) ) )
        {
            put_error( out_p, id_p, "hello_first" );
        }
        else
        {
            handle( ctx_p, op_p->text, out_p );
        }
    }

    if( out_p->overflow )
    {
        /* Too much to say in one answer: say that instead, which always fits. */
        ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );

        ecat_json_out_reset( out_p );
        put_error( out_p, id_p, "too_long" );
    }
    else
    {
        /* The answer fitted. */
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_helper.h.
*/
void ecat_helper_too_long( ECAT_JSON_OUT_T* const out_p )
{
    ecat_json_out_reset( out_p );
    put_error( out_p, NULL, "too_long" );
    return;
}

/*!@}*/

/*============================================================================*/
/*! \name Static functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Send a request to what handles its op.

    \param  ctx_p   The helper, with the request read.
    \param  op      The request's op.
    \param  out_p   The answer.

    \return None.
*/
static void handle( ECAT_HELPER_CTX_T* const ctx_p, char const op[], ECAT_JSON_OUT_T* const out_p )
{
    if( 0 == strcmp( op, "hello" ) )
    {
        do_hello( ctx_p, out_p );
    }
    else if( 0 == strcmp( op, "adapters" ) )
    {
        do_adapters( ctx_p, out_p );
    }
    else if( 0 == strcmp( op, "open" ) )
    {
        do_open( ctx_p, out_p );
    }
    else if( 0 == strcmp( op, "close" ) )
    {
        unmap( ctx_p );
        ecat_port_close( ctx_p->port_p );
        put_ok( out_p, ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER ) );
        ecat_json_out_close( out_p, false );
    }
    else if( 0 == strcmp( op, "scan" ) )
    {
        do_scan( ctx_p, out_p );
    }
    else if( 0 == strcmp( op, "states" ) )
    {
        do_states( ctx_p, out_p );
    }
    else if( 0 == strcmp( op, "state" ) )
    {
        do_set_state( ctx_p, out_p );
    }
    else if( 0 == strcmp( op, "sdo_read" ) )
    {
        do_sdo_read( ctx_p, out_p );
    }
    else if( 0 == strcmp( op, "sdo_write" ) )
    {
        do_sdo_write( ctx_p, out_p );
    }
    else if( 0 == strcmp( op, "map" ) )
    {
        do_map( ctx_p, out_p );
    }
    else if( 0 == strcmp( op, "pdo_write" ) )
    {
        do_pdo_write( ctx_p, out_p );
    }
    else if( 0 == strcmp( op, "process" ) )
    {
        do_process( ctx_p, out_p );
    }
    else if( 0 == strcmp( op, "stop" ) )
    {
        do_stop( ctx_p, out_p );
    }
    else
    {
        put_error( out_p, ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER ), "unknown_op" );
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  hello: agree the protocol version before anything else.

    \param  ctx_p   The helper.
    \param  out_p   The answer: the version this helper speaks, either way.

    \return None.
*/
static void do_hello( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p )
{
    ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );
    ECAT_JSON_FIELD_T const* const version_p = ecat_json_get( &ctx_p->request, "version", ECAT_JSON_KIND_NUMBER );

    if( ( NULL == version_p ) || ( ECAT_HELPER_PROTOCOL != version_p->number ) )
    {
        ctx_p->hello = false;
        ecat_json_out_open( out_p, NULL, false );
        if( NULL != id_p )
        {
            ecat_json_out_u32( out_p, "id", id_p->number );
        }
        else
        {
            /* No id to answer with. */
        }
        ecat_json_out_bool( out_p, "ok", false );
        ecat_json_out_str( out_p, "error", "version" );
        ecat_json_out_u32( out_p, "version", ECAT_HELPER_PROTOCOL );
        ecat_json_out_close( out_p, false );
    }
    else
    {
        ctx_p->hello = true;
        put_ok( out_p, id_p );
        ecat_json_out_u32( out_p, "version", ECAT_HELPER_PROTOCOL );
        ecat_json_out_str( out_p, "master", ecat_port_version() );
        ecat_json_out_bool( out_p, "driver", ( ECAT_PORT_STS_OK == ecat_port_check() ) );
        ecat_json_out_close( out_p, false );
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  adapters: the network adapters there are.

    \param  ctx_p   The helper.
    \param  out_p   The answer.

    \return None.
*/
static void do_adapters( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p )
{
    ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );
    ecat_port_sts_t const sts = ecat_port_check();

    if( ECAT_PORT_STS_OK != sts )
    {
        put_port_error( out_p, id_p, sts, 0U );
    }
    else
    {
        size_t const count = ecat_port_adapters( ctx_p->adapters, ECAT_PORT_MAX_ADAPTERS );

        put_ok( out_p, id_p );
        ecat_json_out_open( out_p, "adapters", true );
        for( size_t idx = 0U; idx < count; idx++ )
        {
            ecat_json_out_open( out_p, NULL, false );
            ecat_json_out_str( out_p, "name", ctx_p->adapters[idx].name );
            ecat_json_out_str( out_p, "desc", ctx_p->adapters[idx].desc );
            ecat_json_out_close( out_p, false );
        }
        ecat_json_out_close( out_p, true );
        ecat_json_out_close( out_p, false );
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  open: open an adapter by name.

    \param  ctx_p   The helper.
    \param  out_p   The answer.

    \return None.
*/
static void do_open( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p )
{
    ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );
    ECAT_JSON_FIELD_T const* const name_p = ecat_json_get( &ctx_p->request, "adapter", ECAT_JSON_KIND_STRING );

    if( NULL == name_p )
    {
        put_error( out_p, id_p, "bad_request" );
    }
    else if( ECAT_PORT_STS_OK != ecat_port_check() )
    {
        put_port_error( out_p, id_p, ECAT_PORT_STS_NO_DRIVER, 0U );
    }
    else
    {
        ecat_port_sts_t sts;

        unmap( ctx_p );
        sts = ecat_port_open( ctx_p->port_p, name_p->text );

        if( ECAT_PORT_STS_OK == sts )
        {
            put_ok( out_p, id_p );
            ecat_json_out_close( out_p, false );
        }
        else
        {
            put_port_error( out_p, id_p, sts, 0U );
        }
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  scan: find the slaves, and say all that is known of each.

    \param  ctx_p   The helper.
    \param  out_p   The answer.

    \return None.
*/
static void do_scan( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p )
{
    ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );
    size_t count = 0U;
    ecat_port_sts_t sts;

    unmap( ctx_p );     /* a scan configures the ring afresh: what was mapped is gone */
    sts = ecat_port_scan( ctx_p->port_p, &count );

    if( ECAT_PORT_STS_OK == sts )
    {
        put_ok( out_p, id_p );
        put_slaves( ctx_p, out_p, true );
        ecat_json_out_close( out_p, false );
    }
    else
    {
        put_port_error( out_p, id_p, sts, 0U );
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  states: every slave's state and AL status code, read again.

    \param  ctx_p   The helper.
    \param  out_p   The answer.

    \return None.
*/
static void do_states( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p )
{
    ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );
    ecat_port_sts_t const sts = ecat_port_read_states( ctx_p->port_p );

    if( ECAT_PORT_STS_OK == sts )
    {
        put_ok( out_p, id_p );
        put_slaves( ctx_p, out_p, false );
        ecat_json_out_close( out_p, false );
    }
    else
    {
        put_port_error( out_p, id_p, sts, 0U );
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  state: ask one slave, or all of them, for a state.

    Answered with every slave's state afterwards, whether it worked or not, so
    a refusal comes with the AL status code that says why.

    \param  ctx_p   The helper.
    \param  out_p   The answer.

    \return None.
*/
static void do_set_state( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p )
{
    ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );
    ECAT_JSON_FIELD_T const* const name_p = ecat_json_get( &ctx_p->request, "state", ECAT_JSON_KIND_STRING );
    uint32_t slave = 0U;
    uint32_t timeout_ms = ECAT_HELPER_STATE_TIMEOUT_MS;
    uint16_t state = 0U;

    if( ( NULL == name_p ) || !state_number( name_p->text, &state ) ||
        !get_number( ctx_p, "slave", UINT16_MAX, &slave ) ||
        !get_number( ctx_p, "timeout_ms", 60000U, &timeout_ms ) )
    {
        put_error( out_p, id_p, "bad_request" );
    }
    else if( !ctx_p->mapped && ( ( ECAT_PORT_STATE_SAFEOP == state ) || ( ECAT_PORT_STATE_OP == state ) ) )
    {
        /* A slave refuses SAFE-OP without its sync managers set up for process data. */
        put_error( out_p, id_p, "not_mapped" );
    }
    else
    {
        ecat_port_sts_t const sts =
            ecat_port_set_state( ctx_p->port_p, (uint16_t)slave, state, ( timeout_ms * 1000U ) );

        if( ECAT_PORT_STS_OK == sts )
        {
            put_ok( out_p, id_p );
        }
        else
        {
            put_port_error_open( out_p, id_p, sts, 0U );
        }

        if( ( ECAT_PORT_STS_NOT_OPEN != sts ) && ( ECAT_PORT_STS_NO_SLAVE != sts ) )
        {
            put_slaves( ctx_p, out_p, false );
        }
        else
        {
            /* Nothing to list. */
        }
        ecat_json_out_close( out_p, false );
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  sdo_read: read an object.

    \param  ctx_p   The helper.
    \param  out_p   The answer: the value as hex.

    \return None.
*/
static void do_sdo_read( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p )
{
    ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );
    uint16_t slave;
    uint16_t index;
    uint8_t sub;

    if( !get_object( ctx_p, &slave, &index, &sub ) )
    {
        put_error( out_p, id_p, "bad_request" );
    }
    else
    {
        size_t len = ECAT_HELPER_DATA_LEN;
        uint32_t abort_code = 0U;
        ecat_port_sts_t const sts =
            ecat_port_sdo_read( ctx_p->port_p, slave, index, sub, ctx_p->data, &len, &abort_code );

        if( ECAT_PORT_STS_OK == sts )
        {
            put_ok( out_p, id_p );
            ecat_json_out_hex( out_p, "data", ctx_p->data, len );
            ecat_json_out_close( out_p, false );
        }
        else
        {
            put_port_error( out_p, id_p, sts, abort_code );
        }
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  sdo_write: write an object.

    \param  ctx_p   The helper.
    \param  out_p   The answer.

    \return None.
*/
static void do_sdo_write( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p )
{
    ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );
    ECAT_JSON_FIELD_T const* const data_p = ecat_json_get( &ctx_p->request, "data", ECAT_JSON_KIND_STRING );
    uint16_t slave;
    uint16_t index;
    uint8_t sub;
    size_t len = 0U;

    if( !get_object( ctx_p, &slave, &index, &sub ) || ( NULL == data_p ) ||
        !ecat_json_hex_read( data_p->text, ctx_p->data, ECAT_HELPER_DATA_LEN, &len ) || ( 0U == len ) )
    {
        put_error( out_p, id_p, "bad_request" );
    }
    else
    {
        uint32_t abort_code = 0U;
        ecat_port_sts_t const sts =
            ecat_port_sdo_write( ctx_p->port_p, slave, index, sub, ctx_p->data, len, &abort_code );

        if( ECAT_PORT_STS_OK == sts )
        {
            put_ok( out_p, id_p );
            ecat_json_out_close( out_p, false );
        }
        else
        {
            put_port_error( out_p, id_p, sts, abort_code );
        }
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  map: map the process data, start the cycle, and say where everything is.

    The slaves must be in PRE-OP, and are in SAFE-OP after. The answer gives
    the image's size, each slave's area in it, and each PDO entry the slaves'
    assignments (0x1C12, 0x1C13) list, read by SDO: a slave without a mailbox
    has an area but no entries.

    \param  ctx_p   The helper.
    \param  out_p   The answer.

    \return None.
*/
static void do_map( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p )
{
    ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );
    uint32_t period_us = ECAT_CYCLE_DEFAULT_US;

    if( !get_number( ctx_p, "period_us", ECAT_CYCLE_MAX_US, &period_us ) || ( period_us < ECAT_CYCLE_MIN_US ) )
    {
        put_error( out_p, id_p, "bad_request" );
    }
    else
    {
        ECAT_PORT_IMAGE_T image;
        ecat_port_sts_t sts;

        unmap( ctx_p );
        sts = ecat_port_map( ctx_p->port_p, &image );
        if( ECAT_PORT_STS_OK != sts )
        {
            put_port_error( out_p, id_p, sts, 0U );
        }
        else
        {
            /* The entries first, by SDO, before the cycle shares the ring. */
            read_entries( ctx_p );
            ctx_p->mapped = ecat_cycle_start( ctx_p->port_p, &image, period_us );
            if( !ctx_p->mapped )
            {
                put_error( out_p, id_p, "failed" );
            }
            else
            {
                ECAT_PORT_AREA_T area;

                put_ok( out_p, id_p );
                ecat_json_out_u32( out_p, "period_us", period_us );
                ecat_json_out_u32( out_p, "out_len", (uint32_t)image.out_len );
                ecat_json_out_u32( out_p, "in_len", (uint32_t)image.in_len );
                ecat_json_out_u32( out_p, "expected_wkc", image.expected_wkc );
                put_slaves( ctx_p, out_p, false );

                ecat_json_out_open( out_p, "areas", true );
                for( uint16_t position = 1U;
                     ( position <= ECAT_PORT_MAX_SLAVES ) &&
                     ( ECAT_PORT_STS_OK == ecat_port_area( ctx_p->port_p, position, &area ) );
                     position++ )
                {
                    ecat_json_out_open( out_p, NULL, false );
                    ecat_json_out_u32( out_p, "slave", position );
                    ecat_json_out_u32( out_p, "out_bit", area.out_bit );
                    ecat_json_out_u32( out_p, "out_bits", area.out_bits );
                    ecat_json_out_u32( out_p, "in_bit", area.in_bit );
                    ecat_json_out_u32( out_p, "in_bits", area.in_bits );
                    ecat_json_out_close( out_p, false );
                }
                ecat_json_out_close( out_p, true );

                ecat_json_out_open( out_p, "entries", true );
                for( size_t idx = 0U; idx < ctx_p->entry_count; idx++ )
                {
                    ECAT_HELPER_ENTRY_T const* const entry_p = &ctx_p->entries[idx];

                    ecat_json_out_open( out_p, NULL, false );
                    ecat_json_out_u32( out_p, "slave", entry_p->slave );
                    ecat_json_out_str( out_p, "dir", entry_p->out ? "out" : "in" );
                    ecat_json_out_u32( out_p, "index", entry_p->index );
                    ecat_json_out_u32( out_p, "sub", entry_p->sub );
                    ecat_json_out_u32( out_p, "bits", entry_p->bits );
                    ecat_json_out_u32( out_p, "bit", entry_p->bit );
                    ecat_json_out_close( out_p, false );
                }
                ecat_json_out_close( out_p, true );
                ecat_json_out_close( out_p, false );
            }
        }
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  pdo_write: change an output, from the next cycle on.

    Named by its object (index and sub) when the map listed it, or by bits
    within the slave's own outputs (bit and bits) when it did not -- a slave
    without a mailbox. The data is the value little-endian, (bits + 7) / 8 bytes.

    \param  ctx_p   The helper.
    \param  out_p   The answer.

    \return None.
*/
static void do_pdo_write( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p )
{
    ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );
    ECAT_JSON_FIELD_T const* const data_p = ecat_json_get( &ctx_p->request, "data", ECAT_JSON_KIND_STRING );
    bool const by_object = ( NULL != ecat_json_get( &ctx_p->request, "index", ECAT_JSON_KIND_NUMBER ) );
    uint32_t slave = 0U;
    uint32_t bit = UINT32_MAX;
    uint32_t bits = 0U;
    size_t len = 0U;
    bool ok = get_number( ctx_p, "slave", UINT16_MAX, &slave ) && ( 0U != slave ) && ( NULL != data_p ) &&
              ecat_json_hex_read( data_p->text, ctx_p->data, ECAT_HELPER_DATA_LEN, &len ) && ( 0U != len );
    char const* error_p = NULL;

    if( !ok )
    {
        error_p = "bad_request";
    }
    else if( !ctx_p->mapped )
    {
        error_p = "not_mapped";
    }
    else if( by_object )
    {
        uint16_t object_slave;
        uint16_t index;
        uint8_t sub;

        if( !get_object( ctx_p, &object_slave, &index, &sub ) )
        {
            error_p = "bad_request";
        }
        else
        {
            ECAT_HELPER_ENTRY_T const* const entry_p = find_entry( ctx_p, object_slave, index, sub );

            if( NULL == entry_p )
            {
                error_p = "no_entry";
            }
            else
            {
                bit = entry_p->bit;
                bits = entry_p->bits;
            }
        }
    }
    else
    {
        ECAT_PORT_AREA_T area;

        if( !get_number( ctx_p, "bit", UINT16_MAX, &bit ) || ( UINT32_MAX == bit ) ||
            !get_number( ctx_p, "bits", UINT16_MAX, &bits ) || ( 0U == bits ) )
        {
            error_p = "bad_request";
        }
        else if( ECAT_PORT_STS_OK != ecat_port_area( ctx_p->port_p, (uint16_t)slave, &area ) )
        {
            error_p = "no_slave";
        }
        else if( ( bit + bits ) > area.out_bits )
        {
            error_p = "no_entry";       /* not within the slave's outputs */
        }
        else
        {
            bit += area.out_bit;
        }
    }

    if( NULL != error_p )
    {
        /* Said below. */
    }
    else if( len != ( ( (size_t)bits + 7U ) / 8U ) )
    {
        error_p = "bad_request";        /* the value is not the entry's size */
    }
    else if( !ecat_cycle_put( bit, bits, ctx_p->data ) )
    {
        error_p = "failed";
    }
    else
    {
        /* Written. */
    }

    if( NULL != error_p )
    {
        put_error( out_p, id_p, error_p );
    }
    else
    {
        put_ok( out_p, id_p );
        ecat_json_out_close( out_p, false );
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  process: the cycle's counters, the outputs being sent and the last inputs.

    \param  ctx_p   The helper.
    \param  out_p   The answer.

    \return None.
*/
static void do_process( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p )
{
    ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );

    if( !ctx_p->mapped )
    {
        put_error( out_p, id_p, "not_mapped" );
    }
    else
    {
        ecat_cycle_snapshot( &ctx_p->snap );
        put_ok( out_p, id_p );
        ecat_json_out_bool( out_p, "running", ctx_p->snap.running );
        ecat_json_out_u32( out_p, "period_us", ctx_p->snap.period_us );
        ecat_json_out_u32( out_p, "cycles", ctx_p->snap.cycles );
        ecat_json_out_u32( out_p, "wkc", ctx_p->snap.wkc );
        ecat_json_out_u32( out_p, "expected_wkc", ctx_p->snap.expected_wkc );
        ecat_json_out_u32( out_p, "wkc_errors", ctx_p->snap.wkc_errors );
        ecat_json_out_u32( out_p, "late", ctx_p->snap.late );
        ecat_json_out_hex( out_p, "outputs", ctx_p->snap.outputs, ctx_p->snap.out_len );
        ecat_json_out_hex( out_p, "inputs", ctx_p->snap.inputs, ctx_p->snap.in_len );
        ecat_json_out_close( out_p, false );
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  stop: take every slave to PRE-OP, then stop the cycle and forget the map.

    PRE-OP is asked for while the cycle still runs, so the slaves leave OP as
    asked rather than by their watchdog, with an error; and PRE-OP is what a
    map needs, so the process data can be started again. The cycle stops even
    when a slave refuses, which the answer says, with every slave's state.

    \param  ctx_p   The helper.
    \param  out_p   The answer.

    \return None.
*/
static void do_stop( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p )
{
    ECAT_JSON_FIELD_T const* const id_p = ecat_json_get( &ctx_p->request, "id", ECAT_JSON_KIND_NUMBER );

    if( !ctx_p->mapped )
    {
        put_error( out_p, id_p, "not_mapped" );
    }
    else
    {
        ecat_port_sts_t const sts = ecat_port_set_state( ctx_p->port_p, 0U, ECAT_PORT_STATE_PREOP,
                                                         ECAT_HELPER_STATE_TIMEOUT_MS * 1000U );

        unmap( ctx_p );
        if( ECAT_PORT_STS_OK == sts )
        {
            put_ok( out_p, id_p );
        }
        else
        {
            put_port_error_open( out_p, id_p, sts, 0U );
        }
        put_slaves( ctx_p, out_p, false );
        ecat_json_out_close( out_p, false );
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Stop the cycle and forget the map: before the ring is opened, scanned or closed.

    \param  ctx_p   The helper.

    \return None.
*/
static void unmap( ECAT_HELPER_CTX_T* const ctx_p )
{
    ecat_cycle_stop();
    ctx_p->mapped = false;
    ctx_p->entry_count = 0U;
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Read every mailbox slave's PDO assignment into the entry list.

    \param  ctx_p   The helper, the ring mapped.

    \return None.
*/
static void read_entries( ECAT_HELPER_CTX_T* const ctx_p )
{
    ECAT_PORT_SLAVE_T slave;
    ECAT_PORT_AREA_T area;

    ctx_p->entry_count = 0U;
    for( uint16_t position = 1U;
         ( position <= ECAT_PORT_MAX_SLAVES ) &&
         ( ECAT_PORT_STS_OK == ecat_port_slave( ctx_p->port_p, position, &slave ) );
         position++ )
    {
        if( slave.mailbox && ( ECAT_PORT_STS_OK == ecat_port_area( ctx_p->port_p, position, &area ) ) )
        {
            read_assignment( ctx_p, position, 0x1C12U, true, area.out_bit );
            read_assignment( ctx_p, position, 0x1C13U, false, area.in_bit );
        }
        else
        {
            /* No mailbox to read an assignment through: an area, no entries. */
        }
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Add the entries of one PDO assignment, laid out from a first bit.

    Each assigned PDO's entries follow one another; an entry of index 0 is a
    gap, which takes its bits but is not listed. Stops at the first SDO that
    fails: a slave with a fixed mapping may not have the assignment objects.

    \param  ctx_p       The helper.
    \param  position    The slave.
    \param  assignment  0x1C12 for its outputs, 0x1C13 for its inputs.
    \param  out         true for outputs.
    \param  first_bit   Where the slave's outputs or inputs start.

    \return None.
*/
static void read_assignment( ECAT_HELPER_CTX_T* const ctx_p,
                             uint16_t const position,
                             uint16_t const assignment,
                             bool const out,
                             uint32_t const first_bit )
{
    uint32_t bit = first_bit;
    uint32_t pdos = 0U;
    bool ok = sdo_number( ctx_p, position, assignment, 0U, &pdos ) && ( pdos <= UINT8_MAX );

    for( uint32_t pdo_sub = 1U; ok && ( pdo_sub <= pdos ); pdo_sub++ )
    {
        uint32_t pdo = 0U;
        uint32_t count = 0U;

        ok = sdo_number( ctx_p, position, assignment, (uint8_t)pdo_sub, &pdo ) && ( pdo <= UINT16_MAX ) &&
             sdo_number( ctx_p, position, (uint16_t)pdo, 0U, &count ) && ( count <= UINT8_MAX );
        for( uint32_t entry_sub = 1U; ok && ( entry_sub <= count ); entry_sub++ )
        {
            uint32_t entry = 0U;

            ok = sdo_number( ctx_p, position, (uint16_t)pdo, (uint8_t)entry_sub, &entry );
            if( ok )
            {
                uint16_t const index = (uint16_t)( entry >> 16U );
                uint8_t const bits = (uint8_t)entry;

                if( ( 0U != index ) && ( ctx_p->entry_count < ECAT_HELPER_MAX_ENTRIES ) )
                {
                    ECAT_HELPER_ENTRY_T* const entry_p = &ctx_p->entries[ctx_p->entry_count];

                    entry_p->slave = position;
                    entry_p->index = index;
                    entry_p->sub = (uint8_t)( entry >> 8U );
                    entry_p->bits = bits;
                    entry_p->out = out;
                    entry_p->bit = bit;
                    ctx_p->entry_count++;
                }
                else
                {
                    /* A gap, or no room to list it. */
                }
                bit += bits;
            }
            else
            {
                /* The entry could not be read: stop here. */
            }
        }
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Read an object of up to 4 bytes by SDO, as a number.

    \param  ctx_p       The helper.
    \param  position    The slave.
    \param  index       The object's index.
    \param  sub         Its sub-index.
    \param  value_p     Its value, little-endian as the slave sent it.

    \return true when it was read and was 1 to 4 bytes.
*/
static bool sdo_number( ECAT_HELPER_CTX_T* const ctx_p,
                        uint16_t const position,
                        uint16_t const index,
                        uint8_t const sub,
                        uint32_t* const value_p )
{
    size_t len = ECAT_HELPER_DATA_LEN;
    uint32_t abort_code = 0U;
    bool const ok = ( ECAT_PORT_STS_OK ==
                      ecat_port_sdo_read( ctx_p->port_p, position, index, sub, ctx_p->data, &len, &abort_code ) ) &&
                    ( 0U != len ) && ( len <= 4U );

    *value_p = 0U;
    for( size_t idx = 0U; ok && ( idx < len ); idx++ )
    {
        *value_p |= (uint32_t)ctx_p->data[idx] << ( 8U * idx );
    }
    return ok;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  The output entry for an object of a slave.

    \param  ctx_p   The helper.
    \param  slave   The slave.
    \param  index   The object's index.
    \param  sub     Its sub-index.

    \return The entry, or NULL when the map listed no such output.
*/
static ECAT_HELPER_ENTRY_T const* find_entry( ECAT_HELPER_CTX_T const* const ctx_p,
                                              uint16_t const slave,
                                              uint16_t const index,
                                              uint8_t const sub )
{
    ECAT_HELPER_ENTRY_T const* found_p = NULL;

    for( size_t idx = 0U; ( NULL == found_p ) && ( idx < ctx_p->entry_count ); idx++ )
    {
        ECAT_HELPER_ENTRY_T const* const entry_p = &ctx_p->entries[idx];

        if( entry_p->out && ( slave == entry_p->slave ) && ( index == entry_p->index ) && ( sub == entry_p->sub ) )
        {
            found_p = entry_p;
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
    \brief  An optional number field, checked against its range.

    \param  ctx_p       The helper, with the request read.
    \param  key         The field's key.
    \param  max         The largest it may be.
    \param  number_p    The number; left as it is when the field is absent.

    \return false when the field is there and is not a number up to \p max.
*/
static bool get_number( ECAT_HELPER_CTX_T const* const ctx_p,
                        char const key[],
                        uint32_t const max,
                        uint32_t* const number_p )
{
    ECAT_JSON_FIELD_T const* const number_field_p = ecat_json_get( &ctx_p->request, key, ECAT_JSON_KIND_NUMBER );
    bool ok;

    if( NULL != number_field_p )
    {
        ok = ( number_field_p->number <= max );
        *number_p = number_field_p->number;
    }
    else
    {
        /* Absent as a number: wrong only if it is there as something else. */
        ok = ( NULL == ecat_json_get( &ctx_p->request, key, ECAT_JSON_KIND_STRING ) ) &&
             ( NULL == ecat_json_get( &ctx_p->request, key, ECAT_JSON_KIND_BOOL ) );
    }
    return ok;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  The slave, index and sub-index an SDO request names; all three required.

    \param  ctx_p   The helper, with the request read.
    \param  slave_p The slave's position, 1 for the first.
    \param  index_p The object's index.
    \param  sub_p   Its sub-index.

    \return true when all three are there and in range.
*/
static bool get_object( ECAT_HELPER_CTX_T const* const ctx_p,
                        uint16_t* const slave_p,
                        uint16_t* const index_p,
                        uint8_t* const sub_p )
{
    uint32_t slave = 0U;
    uint32_t index = UINT32_MAX;
    uint32_t sub = UINT32_MAX;
    bool const ok = get_number( ctx_p, "slave", UINT16_MAX, &slave ) && ( 0U != slave ) &&
                    get_number( ctx_p, "index", UINT16_MAX, &index ) && ( UINT32_MAX != index ) &&
                    get_number( ctx_p, "sub", UINT8_MAX, &sub ) && ( UINT32_MAX != sub );

    *slave_p = (uint16_t)slave;
    *index_p = (uint16_t)index;
    *sub_p = (uint8_t)sub;
    return ok;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  The "slaves" list, as the master last found them.

    \param  ctx_p   The helper.
    \param  out_p   The answer, with an object open.
    \param  full    true for all that is known of each, false for their states only.

    \return None.
*/
static void put_slaves( ECAT_HELPER_CTX_T* const ctx_p, ECAT_JSON_OUT_T* const out_p, bool const full )
{
    ECAT_PORT_SLAVE_T slave;

    ecat_json_out_open( out_p, "slaves", true );
    for( uint16_t position = 1U;
         ( position <= ECAT_PORT_MAX_SLAVES ) &&
         ( ECAT_PORT_STS_OK == ecat_port_slave( ctx_p->port_p, position, &slave ) );
         position++ )
    {
        ecat_json_out_open( out_p, NULL, false );
        ecat_json_out_u32( out_p, "position", slave.position );
        if( full )
        {
            ecat_json_out_str( out_p, "name", slave.name );
            ecat_json_out_u32( out_p, "vendor", slave.vendor );
            ecat_json_out_u32( out_p, "product", slave.product );
            ecat_json_out_u32( out_p, "revision", slave.revision );
            ecat_json_out_bool( out_p, "mailbox", slave.mailbox );
        }
        else
        {
            /* States only. */
        }
        ecat_json_out_str( out_p, "state", state_name( slave.state & ECAT_PORT_STATE_MASK ) );
        ecat_json_out_bool( out_p, "error", ( 0U != ( slave.state & ECAT_PORT_STATE_ERROR ) ) );
        ecat_json_out_u32( out_p, "al_status", slave.al_status );
        ecat_json_out_close( out_p, false );
    }
    ecat_json_out_close( out_p, true );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Start a successful answer: {"id": n, "ok": true -- left open for more.

    \param  out_p   The answer, empty.
    \param  id_p    The request's id.

    \return None.
*/
static void put_ok( ECAT_JSON_OUT_T* const out_p, ECAT_JSON_FIELD_T const* const id_p )
{
    ecat_json_out_open( out_p, NULL, false );
    if( NULL != id_p )
    {
        ecat_json_out_u32( out_p, "id", id_p->number );
    }
    else
    {
        /* No id to answer with. */
    }
    ecat_json_out_bool( out_p, "ok", true );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  A whole error answer: {"id": n, "ok": false, "error": code}.

    \param  out_p   The answer, empty.
    \param  id_p    The request's id, or NULL when it had none.
    \param  code    The error.

    \return None.
*/
static void put_error( ECAT_JSON_OUT_T* const out_p, ECAT_JSON_FIELD_T const* const id_p, char const code[] )
{
    put_error_open( out_p, id_p, code );
    ecat_json_out_close( out_p, false );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Start an error answer: {"id": n, "ok": false, "error": code -- left open for more.

    \param  out_p   The answer, empty.
    \param  id_p    The request's id, or NULL when it had none.
    \param  code    The error.

    \return None.
*/
static void put_error_open( ECAT_JSON_OUT_T* const out_p, ECAT_JSON_FIELD_T const* const id_p, char const code[] )
{
    ecat_json_out_open( out_p, NULL, false );
    if( NULL != id_p )
    {
        ecat_json_out_u32( out_p, "id", id_p->number );
    }
    else
    {
        /* No id to answer with. */
    }
    ecat_json_out_bool( out_p, "ok", false );
    ecat_json_out_str( out_p, "error", code );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  A whole error answer for what the master reported.

    \param  out_p       The answer, empty.
    \param  id_p        The request's id.
    \param  sts         What the master reported.
    \param  abort_code  The SDO abort code, with ECAT_PORT_STS_SDO_ABORT.

    \return None.
*/
static void put_port_error( ECAT_JSON_OUT_T* const out_p,
                            ECAT_JSON_FIELD_T const* const id_p,
                            ecat_port_sts_t const sts,
                            uint32_t const abort_code )
{
    put_port_error_open( out_p, id_p, sts, abort_code );
    ecat_json_out_close( out_p, false );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Start an error answer for what the master reported -- left open for more.

    \param  out_p       The answer, empty.
    \param  id_p        The request's id.
    \param  sts         What the master reported.
    \param  abort_code  The SDO abort code, with ECAT_PORT_STS_SDO_ABORT.

    \return None.
*/
static void put_port_error_open( ECAT_JSON_OUT_T* const out_p,
                                 ECAT_JSON_FIELD_T const* const id_p,
                                 ecat_port_sts_t const sts,
                                 uint32_t const abort_code )
{
    char const* code_p;

    switch( sts )
    {
        case ECAT_PORT_STS_NO_DRIVER:
        {
            code_p = "no_driver";
            break;
        }
        case ECAT_PORT_STS_NOT_OPEN:
        {
            code_p = "not_open";
            break;
        }
        case ECAT_PORT_STS_NO_ADAPTER:
        {
            code_p = "no_adapter";
            break;
        }
        case ECAT_PORT_STS_NO_SLAVES:
        {
            code_p = "no_slaves";
            break;
        }
        case ECAT_PORT_STS_NO_SLAVE:
        {
            code_p = "no_slave";
            break;
        }
        case ECAT_PORT_STS_NO_MAILBOX:
        {
            code_p = "no_mailbox";
            break;
        }
        case ECAT_PORT_STS_SDO_ABORT:
        {
            code_p = "sdo_abort";
            break;
        }
        case ECAT_PORT_STS_TIMEOUT:
        {
            code_p = "timeout";
            break;
        }
        case ECAT_PORT_STS_TOO_LONG:
        {
            code_p = "too_long";
            break;
        }
        case ECAT_PORT_STS_STATE_REFUSED:
        {
            code_p = "state_refused";
            break;
        }
        case ECAT_PORT_STS_BAD_STATE:
        {
            code_p = "bad_state";
            break;
        }
        case ECAT_PORT_STS_OK:
        case ECAT_PORT_STS_FAILED:
        default:
        {
            code_p = "failed";
            break;
        }
    }

    put_error_open( out_p, id_p, code_p );
    if( ECAT_PORT_STS_SDO_ABORT == sts )
    {
        ecat_json_out_u32( out_p, "abort", abort_code );
    }
    else
    {
        /* Nothing more to say. */
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  The name of a state, for an answer.

    \param  state   ECAT_PORT_STATE_*, without the error bit.

    \return Its name, or "NONE" for a slave that has not answered.
*/
static char const* state_name( uint16_t const state )
{
    char const* name_p = "NONE";

    for( size_t idx = 0U; idx < ( sizeof( state_names ) / sizeof( state_names[0U] ) ); idx++ )
    {
        if( state == state_names[idx].state )
        {
            name_p = state_names[idx].name_p;
        }
        else
        {
            /* Not this one. */
        }
    }
    return name_p;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  The state a request names.

    \param  name    Its name: "INIT", "PREOP", "BOOT", "SAFEOP" or "OP".
    \param  state_p The state.

    \return true when the name is one of them.
*/
static bool state_number( char const name[], uint16_t* const state_p )
{
    bool found = false;

    for( size_t idx = 0U; idx < ( sizeof( state_names ) / sizeof( state_names[0U] ) ); idx++ )
    {
        if( 0 == strcmp( name, state_names[idx].name_p ) )
        {
            *state_p = state_names[idx].state;
            found = true;
        }
        else
        {
            /* Not this one. */
        }
    }
    return found;
}

/*!@}*/

/*============================================================================*/

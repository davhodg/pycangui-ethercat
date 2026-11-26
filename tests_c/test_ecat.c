/*!
    \file   test_ecat.c
    \brief  The protocol and the JSON it is made of, against the scripted ring.

    Each test feeds request lines to ecat_helper_handle() and checks the fields
    of the answers -- whether it worked, the error code, the abort code, the
    states, the data -- never any wording.

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

/* MISRA C:2023 Rule 21.6 (no standard I/O): deviation. A test program reports on stdout. */
#include <stdio.h>
#include <string.h>

#include "ecat_helper.h"
#include "ecat_json.h"
#include "ecat_port.h"

/*============================================================================*/
/*! \name Defines */
/*============================================================================*/
/*!@{*/

/*! Check a condition, counting and reporting a failure with where it was. */
#define CHECK( cond )                                                                   \
    do                                                                                  \
    {                                                                                   \
        checks++;                                                                       \
        if( !( cond ) )                                                                 \
        {                                                                               \
            failures++;                                                                 \
            ( void )printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond );            \
            ( void )printf( "     answer: %s\n", answer.buf );                         \
        }                                                                               \
    } while( false )

/*!@}*/

/*============================================================================*/
/*! \name Static data */
/*============================================================================*/
/*!@{*/

/* MISRA C:2023 Dir 4.12: deviation. A test program's counters and buffers. */
static uint32_t checks;             /*!< Checks made */
static uint32_t failures;           /*!< Checks failed */
static ECAT_HELPER_CTX_T helper;    /*!< The helper under test */
static ECAT_JSON_OUT_T answer;      /*!< The last answer */
static ECAT_JSON_OBJ_T obj;         /*!< A request, read */

/*!@}*/

/*============================================================================*/
/*! \name Static functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Send one request line to the helper; the answer is in `answer`.

    \param  line    The request, terminated.

    \return None.
*/
static void ask( char const line[] )
{
    ecat_helper_handle( &helper, line, strlen( line ), &answer );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Whether the last answer has this text in it: a field and its value.

    \param  text    The text, such as "\"ok\":true".

    \return true when it is there.
*/
static bool has( char const text[] )
{
    return ( NULL != strstr( answer.buf, text ) );
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  A fresh helper over a fresh fake master, hello said.

    \return None.
*/
static void start( void )
{
    ecat_port_close( ecat_port_instance() );
    ecat_helper_init( &helper, ecat_port_instance() );
    ask( "{\"id\":1,\"op\":\"hello\",\"version\":1}" );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Started, the ring's adapter open and scanned.

    \return None.
*/
static void start_scanned( void )
{
    start();
    ask( "{\"id\":2,\"op\":\"open\",\"adapter\":\"\\\\Device\\\\NPF_{FAKE-0001}\"}" );
    ask( "{\"id\":3,\"op\":\"scan\"}" );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Flat objects are read: strings with escapes, numbers, true and false.

    \return None.
*/
static void test_json_reads_a_flat_object( void )
{
    char const line[] = " {\"op\" : \"a\\\\b\\\"c\\u00e9\", \"n\":4294967295, \"t\":true, \"f\":false} ";

    CHECK( ECAT_JSON_STS_OK == ecat_json_parse( line, strlen( line ), &obj ) );
    CHECK( 4U == obj.count );
    CHECK( 0 == strcmp( ecat_json_get( &obj, "op", ECAT_JSON_KIND_STRING )->text, "a\\b\"c\xC3\xA9" ) );
    CHECK( UINT32_MAX == ecat_json_get( &obj, "n", ECAT_JSON_KIND_NUMBER )->number );
    CHECK( 1U == ecat_json_get( &obj, "t", ECAT_JSON_KIND_BOOL )->number );
    CHECK( 0U == ecat_json_get( &obj, "f", ECAT_JSON_KIND_BOOL )->number );
    CHECK( NULL == ecat_json_get( &obj, "n", ECAT_JSON_KIND_STRING ) );
    CHECK( ECAT_JSON_STS_OK == ecat_json_parse( "{}", 2U, &obj ) );
    CHECK( 0U == obj.count );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  What is not a flat object of the kinds above is refused.

    \return None.
*/
static void test_json_refuses_what_it_does_not_read( void )
{
    char const* const bad[] =
    {
        "", "[]", "{", "{\"a\":1", "{\"a\":1}x", "{\"a\":-1}", "{\"a\":01}", "{\"a\":1.5}",
        "{\"a\":4294967296}", "{\"a\":{}}", "{\"a\":\"x}", "{\"a\":\"\\q\"}", "{\"a\":\"\\u0000\"}",
        "{\"a\":\"\t\"}", "{a:1}", "{\"a\" 1}", "{\"a\":1,}", "{\"a\":null}"
    };

    for( size_t idx = 0U; idx < ( sizeof( bad ) / sizeof( bad[0U] ) ); idx++ )
    {
        CHECK( ECAT_JSON_STS_SYNTAX == ecat_json_parse( bad[idx], strlen( bad[idx] ), &obj ) );
    }

    CHECK( ECAT_JSON_STS_TOO_LONG ==
           ecat_json_parse( "{\"a_key_too_long_x\":1}", strlen( "{\"a_key_too_long_x\":1}" ), &obj ) );
    CHECK( ECAT_JSON_STS_TOO_MANY ==
           ecat_json_parse( "{\"a\":1,\"b\":1,\"c\":1,\"d\":1,\"e\":1,\"f\":1,\"g\":1,\"h\":1,"
                            "\"i\":1,\"j\":1,\"k\":1,\"l\":1,\"m\":1,\"n\":1,\"o\":1,\"p\":1,\"q\":1}",
                            strlen( "{\"a\":1,\"b\":1,\"c\":1,\"d\":1,\"e\":1,\"f\":1,\"g\":1,\"h\":1,"
                                    "\"i\":1,\"j\":1,\"k\":1,\"l\":1,\"m\":1,\"n\":1,\"o\":1,\"p\":1,\"q\":1}" ),
                            &obj ) );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Hex goes both ways, and what is not whole bytes of hex is refused.

    \return None.
*/
static void test_hex_both_ways( void )
{
    uint8_t data[4U];
    size_t len = 0U;
    uint8_t const bytes[3U] = { 0x00U, 0xABU, 0x7FU };

    CHECK( ecat_json_hex_read( "00aB7f", data, sizeof( data ), &len ) );
    CHECK( ( 3U == len ) && ( 0 == memcmp( data, bytes, 3U ) ) );
    CHECK( !ecat_json_hex_read( "abc", data, sizeof( data ), &len ) );
    CHECK( !ecat_json_hex_read( "zz", data, sizeof( data ), &len ) );
    CHECK( !ecat_json_hex_read( "0011223344", data, sizeof( data ), &len ) );

    ecat_json_out_reset( &answer );
    ecat_json_out_hex( &answer, NULL, bytes, 3U );
    CHECK( 0 == strcmp( answer.buf, "\"00ab7f\"" ) );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Nothing is done before the hello, and the hello agrees the version.

    \return None.
*/
static void test_the_hello_comes_first_and_agrees_the_version( void )
{
    ecat_port_close( ecat_port_instance() );
    ecat_helper_init( &helper, ecat_port_instance() );

    ask( "{\"id\":7,\"op\":\"adapters\"}" );
    CHECK( has( "\"id\":7" ) && has( "\"ok\":false" ) && has( "\"error\":\"hello_first\"" ) );

    ask( "{\"id\":8,\"op\":\"hello\",\"version\":2}" );
    CHECK( has( "\"ok\":false" ) && has( "\"error\":\"version\"" ) && has( "\"version\":1" ) );
    ask( "{\"id\":9,\"op\":\"adapters\"}" );
    CHECK( has( "\"error\":\"hello_first\"" ) );

    ask( "{\"id\":10,\"op\":\"hello\",\"version\":1}" );
    CHECK( has( "\"id\":10" ) && has( "\"ok\":true" ) && has( "\"version\":1" ) && has( "\"master\":\"fake ring\"" ) &&
           has( "\"driver\":true" ) );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Requests that cannot be followed are said to be so, with their id where they had one.

    \return None.
*/
static void test_requests_that_cannot_be_followed( void )
{
    start();
    ask( "not json" );
    CHECK( has( "\"error\":\"bad_request\"" ) && !has( "\"id\"" ) );
    ask( "{\"op\":\"scan\"}" );
    CHECK( has( "\"error\":\"bad_request\"" ) );
    ask( "{\"id\":4,\"op\":\"fly\"}" );
    CHECK( has( "\"id\":4" ) && has( "\"error\":\"unknown_op\"" ) );
    ask( "{\"id\":5,\"op\":\"open\"}" );
    CHECK( has( "\"error\":\"bad_request\"" ) );
    ask( "{\"id\":6,\"op\":\"sdo_read\",\"slave\":1,\"index\":65536,\"sub\":0}" );
    CHECK( has( "\"error\":\"bad_request\"" ) );
    ask( "{\"id\":6,\"op\":\"sdo_read\",\"slave\":1,\"index\":4096,\"sub\":256}" );
    CHECK( has( "\"error\":\"bad_request\"" ) );
    ask( "{\"id\":6,\"op\":\"sdo_read\",\"slave\":0,\"index\":4096,\"sub\":0}" );
    CHECK( has( "\"error\":\"bad_request\"" ) );
    ask( "{\"id\":6,\"op\":\"sdo_read\",\"slave\":\"1\",\"index\":4096,\"sub\":0}" );
    CHECK( has( "\"error\":\"bad_request\"" ) );
    ask( "{\"id\":6,\"op\":\"sdo_write\",\"slave\":1,\"index\":24672,\"sub\":0,\"data\":\"1\"}" );
    CHECK( has( "\"error\":\"bad_request\"" ) );
    ask( "{\"id\":6,\"op\":\"state\",\"state\":\"FLYING\"}" );
    CHECK( has( "\"error\":\"bad_request\"" ) );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Adapters are listed, their names escaped, and opened by name.

    \return None.
*/
static void test_adapters_are_listed_and_opened_by_name( void )
{
    start();
    ask( "{\"id\":2,\"op\":\"adapters\"}" );
    CHECK( has( "\"ok\":true" ) && has( "\"name\":\"\\\\Device\\\\NPF_{FAKE-0001}\"" ) &&
           has( "\"name\":\"\\\\Device\\\\NPF_{FAKE-0002}\"" ) );

    ask( "{\"id\":3,\"op\":\"scan\"}" );
    CHECK( has( "\"error\":\"not_open\"" ) );
    ask( "{\"id\":4,\"op\":\"open\",\"adapter\":\"nothing\"}" );
    CHECK( has( "\"error\":\"no_adapter\"" ) );
    ask( "{\"id\":5,\"op\":\"open\",\"adapter\":\"\\\\Device\\\\NPF_{FAKE-0002}\"}" );
    CHECK( has( "\"ok\":true" ) );
    ask( "{\"id\":6,\"op\":\"scan\"}" );
    CHECK( has( "\"error\":\"no_slaves\"" ) );
    ask( "{\"id\":7,\"op\":\"close\"}" );
    CHECK( has( "\"ok\":true" ) );
    ask( "{\"id\":8,\"op\":\"states\"}" );
    CHECK( has( "\"error\":\"not_open\"" ) );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  A scan lists the slaves, identity and all, in PRE-OP.

    \return None.
*/
static void test_a_scan_lists_the_slaves_in_preop( void )
{
    start_scanned();
    CHECK( has( "\"ok\":true" ) );
    CHECK( has( "{\"position\":1,\"name\":\"Fake drive\",\"vendor\":2766,\"product\":1026,"
                "\"revision\":65536,\"mailbox\":true,\"state\":\"PREOP\",\"error\":false,\"al_status\":0}" ) );
    CHECK( has( "{\"position\":2,\"name\":\"Fake IO\"" ) && has( "\"mailbox\":false" ) );
    CHECK( !has( "\"position\":3" ) );

    ask( "{\"id\":4,\"op\":\"states\"}" );
    CHECK( has( "{\"position\":1,\"state\":\"PREOP\",\"error\":false,\"al_status\":0}" ) &&
           !has( "\"name\"" ) );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  States go up the ladder a step at a time; a jump is refused with the slave's reason.

    \return None.
*/
static void test_states_go_a_step_at_a_time( void )
{
    start_scanned();
    ask( "{\"id\":4,\"op\":\"state\",\"slave\":1,\"state\":\"OP\"}" );
    CHECK( has( "\"ok\":false" ) && has( "\"error\":\"state_refused\"" ) );
    CHECK( has( "{\"position\":1,\"state\":\"PREOP\",\"error\":true,\"al_status\":17}" ) );
    CHECK( has( "{\"position\":2,\"state\":\"PREOP\",\"error\":false" ) );

    ask( "{\"id\":5,\"op\":\"state\",\"state\":\"SAFEOP\"}" );
    CHECK( has( "\"ok\":true" ) && has( "{\"position\":1,\"state\":\"SAFEOP\",\"error\":false,\"al_status\":0}" ) &&
           has( "{\"position\":2,\"state\":\"SAFEOP\"" ) );
    ask( "{\"id\":6,\"op\":\"state\",\"state\":\"OP\",\"timeout_ms\":500}" );
    CHECK( has( "\"ok\":true" ) && has( "\"state\":\"OP\"" ) );
    ask( "{\"id\":7,\"op\":\"state\",\"slave\":2,\"state\":\"INIT\"}" );
    CHECK( has( "{\"position\":1,\"state\":\"OP\"" ) && has( "{\"position\":2,\"state\":\"INIT\"" ) );
    ask( "{\"id\":8,\"op\":\"state\",\"slave\":3,\"state\":\"INIT\"}" );
    CHECK( has( "\"error\":\"no_slave\"" ) );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  SDOs read and write, with the abort codes a slave sends.

    \return None.
*/
static void test_sdos_read_write_and_abort( void )
{
    start_scanned();
    ask( "{\"id\":4,\"op\":\"sdo_read\",\"slave\":1,\"index\":4096,\"sub\":0}" );
    CHECK( has( "\"id\":4" ) && has( "\"ok\":true" ) && has( "\"data\":\"92010200\"" ) );
    ask( "{\"id\":5,\"op\":\"sdo_read\",\"slave\":1,\"index\":4104,\"sub\":0}" );
    CHECK( has( "\"data\":\"46616b65206472697665\"" ) );     /* "Fake drive" */

    ask( "{\"id\":6,\"op\":\"sdo_write\",\"slave\":1,\"index\":24672,\"sub\":0,\"data\":\"01\"}" );
    CHECK( has( "\"ok\":true" ) );
    ask( "{\"id\":7,\"op\":\"sdo_read\",\"slave\":1,\"index\":24673,\"sub\":0}" );
    CHECK( has( "\"data\":\"01\"" ) );

    ask( "{\"id\":8,\"op\":\"sdo_write\",\"slave\":1,\"index\":24641,\"sub\":0,\"data\":\"0000\"}" );
    CHECK( has( "\"error\":\"sdo_abort\"" ) && has( "\"abort\":100728834" ) );     /* 0x06010002 */
    ask( "{\"id\":9,\"op\":\"sdo_read\",\"slave\":1,\"index\":8192,\"sub\":0}" );
    CHECK( has( "\"error\":\"sdo_abort\"" ) && has( "\"abort\":100794368" ) );     /* 0x06020000 */
    ask( "{\"id\":10,\"op\":\"sdo_read\",\"slave\":1,\"index\":4120,\"sub\":9}" );
    CHECK( has( "\"error\":\"sdo_abort\"" ) && has( "\"abort\":101253137" ) );     /* 0x06090011 */
    ask( "{\"id\":11,\"op\":\"sdo_write\",\"slave\":1,\"index\":24640,\"sub\":0,\"data\":\"01\"}" );
    CHECK( has( "\"error\":\"sdo_abort\"" ) && has( "\"abort\":101122064" ) );     /* 0x06070010 */

    ask( "{\"id\":12,\"op\":\"sdo_read\",\"slave\":2,\"index\":4096,\"sub\":0}" );
    CHECK( has( "\"error\":\"no_mailbox\"" ) );
    ask( "{\"id\":13,\"op\":\"sdo_read\",\"slave\":3,\"index\":4096,\"sub\":0}" );
    CHECK( has( "\"error\":\"no_slave\"" ) );

    ask( "{\"id\":14,\"op\":\"state\",\"slave\":1,\"state\":\"INIT\"}" );
    ask( "{\"id\":15,\"op\":\"sdo_read\",\"slave\":1,\"index\":4096,\"sub\":0}" );
    CHECK( has( "\"error\":\"timeout\"" ) );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  A line too long to read is answered, with no id.

    \return None.
*/
static void test_a_line_too_long_is_answered( void )
{
    static char line[ECAT_JSON_TEXT_LEN + 64U];
    char const head[] = "{\"id\":4,\"op\":\"open\",\"adapter\":\"";
    size_t len = 0U;

    start();
    ( void )memcpy( &line[len], head, strlen( head ) );
    len += strlen( head );
    while( len < ( ECAT_JSON_TEXT_LEN + 40U ) )
    {
        line[len] = 'x';
        len++;
    }
    ( void )memcpy( &line[len], "\"}", 3U );
    ask( line );
    CHECK( has( "\"ok\":false" ) && has( "\"error\":\"too_long\"" ) );

    ecat_helper_too_long( &answer );
    CHECK( has( "\"error\":\"too_long\"" ) && !has( "\"id\"" ) );
    return;
}

/*!@}*/

/*============================================================================*/
/*! \name Public functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Run every test.

    \return 0 when all passed, 1 otherwise.
*/
int main( void )
{
    test_json_reads_a_flat_object();
    test_json_refuses_what_it_does_not_read();
    test_hex_both_ways();
    test_the_hello_comes_first_and_agrees_the_version();
    test_requests_that_cannot_be_followed();
    test_adapters_are_listed_and_opened_by_name();
    test_a_scan_lists_the_slaves_in_preop();
    test_states_go_a_step_at_a_time();
    test_sdos_read_write_and_abort();
    test_a_line_too_long_is_answered();

    ( void )printf( "%u checks, %u failed\n", (unsigned int)checks, (unsigned int)failures );
    return ( 0U == failures ) ? 0 : 1;
}

/*!@}*/

/*============================================================================*/

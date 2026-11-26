/*!
    \file   ecat_port_fake.c
    \brief  A scripted ring, for the tests: the ring as ecat_port.h describes it, with no network.

    Two adapters: the first has a ring of two slaves on it, the second nothing.
    Slave 1 is a CiA 402 drive with a small object dictionary; slave 2 is simple
    I/O with no mailbox. The slaves keep to the EtherCAT state machine -- INIT,
    PRE-OP, SAFE-OP, OP one step at a time, any step down -- and refuse anything
    else as a real slave does, with the error bit and AL status 0x0011. SDOs
    answer with the abort codes a real slave sends, and need a mailbox, which
    INIT does not have.

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

#include "ecat_port.h"

#include <stdlib.h>
#include <string.h>

/*============================================================================*/
/*! \name Defines */
/*============================================================================*/
/*!@{*/

#define FAKE_SLAVES             ( 2U )      /*!< Slaves on the first adapter's ring */
#define FAKE_OBJECTS            ( 14U )     /*!< Entries in the drive's object dictionary */
#define FAKE_VALUE_LEN          ( 16U )     /*!< Longest value an entry holds */
#define FAKE_NO_ADAPTER         ( -1 )      /*!< ctx.adapter when none is open */

#define FAKE_AL_INVALID_CHANGE  ( 0x0011U ) /*!< AL status: invalid requested state change */

#define FAKE_ABORT_NO_OBJECT    ( 0x06020000UL )    /*!< SDO abort: object does not exist */
#define FAKE_ABORT_NO_SUB       ( 0x06090011UL )    /*!< SDO abort: sub-index does not exist */
#define FAKE_ABORT_READ_ONLY    ( 0x06010002UL )    /*!< SDO abort: attempt to write a read-only object */
#define FAKE_ABORT_LENGTH       ( 0x06070010UL )    /*!< SDO abort: data type does not match, length of service parameter does not match */

/*!@}*/

/*============================================================================*/
/*! \name Types */
/*============================================================================*/
/*!@{*/

/*! One entry of the drive's object dictionary. */
typedef struct
{
    uint16_t index;                 /*!< The object's index */
    uint8_t  sub;                   /*!< Its sub-index */
    bool     writable;              /*!< Whether an SDO download may change it */
    size_t   len;                   /*!< How many bytes its value is */
    uint8_t  value[FAKE_VALUE_LEN]; /*!< The value, little-endian as on the wire */
} FAKE_OBJECT_T;

/*! The fake master's state. */
struct ecat_port_ctx
{
    int32_t           adapter;                  /*!< Which adapter is open, or FAKE_NO_ADAPTER */
    size_t            found;                    /*!< How many slaves the last scan found */
    ECAT_PORT_SLAVE_T slaves[FAKE_SLAVES];      /*!< The slaves, as the ring has them */
    FAKE_OBJECT_T     objects[FAKE_OBJECTS];    /*!< The drive's (slave 1's) object dictionary */
};

/*!@}*/

/*============================================================================*/
/*! \name Static prototypes */
/*============================================================================*/
/*!@{*/

static void ring_reset( ECAT_PORT_CTX_T* const ctx_p );
static void object_set( FAKE_OBJECT_T* const object_p,
                        uint16_t const index,
                        uint8_t const sub,
                        bool const writable,
                        size_t const len,
                        uint32_t const value );
static FAKE_OBJECT_T* object_find( ECAT_PORT_CTX_T* const ctx_p,
                                   uint16_t const index,
                                   uint8_t const sub,
                                   uint32_t* const abort_p );
static bool change_allowed( uint16_t const from, uint16_t const to );
static ecat_port_sts_t mailbox_slave( ECAT_PORT_CTX_T* const ctx_p, uint16_t const position );

/*!@}*/

/*============================================================================*/
/*! \name Static data */
/*============================================================================*/
/*!@{*/

/*
    MISRA C:2023 Dir 4.12 / the coding standard's "no file-scope mutable state":
    deviation. The master's state is kept here rather than on a stack, because
    the real master's (SOEM's ecx_contextt) is far too large for one, and the
    fake keeps the same shape. There is one master per helper process, and only
    the protocol thread touches it.
*/
static ECAT_PORT_CTX_T fake_ctx = { .adapter = FAKE_NO_ADAPTER };   /*!< The one master */

/*! The adapters there are: the first has the ring on it, the second nothing. */
static ECAT_PORT_ADAPTER_T const fake_adapters[] =
{
    { .name = "\\Device\\NPF_{FAKE-0001}", .desc = "Fake Ethernet with a ring" },
    { .name = "\\Device\\NPF_{FAKE-0002}", .desc = "Fake Ethernet with nothing on it" }
};

/*!@}*/

/*============================================================================*/
/*! \name Required functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ECAT_PORT_CTX_T* ecat_port_instance( void )
{
    return &fake_ctx;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
char const* ecat_port_version( void )
{
    return "fake ring";
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.

    The fake has no driver to miss, unless ECAT_FAKE_NO_DRIVER is set in its
    environment: that is how the tests see what a machine without Npcap does.
*/
ecat_port_sts_t ecat_port_check( void )
{
    return ( NULL == getenv( "ECAT_FAKE_NO_DRIVER" ) ) ? ECAT_PORT_STS_OK : ECAT_PORT_STS_NO_DRIVER;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
size_t ecat_port_adapters( ECAT_PORT_ADAPTER_T list[], size_t const max )
{
    size_t const there = sizeof( fake_adapters ) / sizeof( fake_adapters[0U] );
    size_t const count = ( there < max ) ? there : max;

    for( size_t idx = 0U; idx < count; idx++ )
    {
        list[idx] = fake_adapters[idx];
    }
    return count;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ecat_port_sts_t ecat_port_open( ECAT_PORT_CTX_T* const ctx_p, char const name[] )
{
    ecat_port_sts_t sts = ECAT_PORT_STS_NO_ADAPTER;

    ecat_port_close( ctx_p );
    for( size_t idx = 0U; idx < ( sizeof( fake_adapters ) / sizeof( fake_adapters[0U] ) ); idx++ )
    {
        if( 0 == strcmp( name, fake_adapters[idx].name ) )
        {
            ctx_p->adapter = (int32_t)idx;
            ring_reset( ctx_p );
            sts = ECAT_PORT_STS_OK;
        }
        else
        {
            /* Not this one. */
        }
    }
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
void ecat_port_close( ECAT_PORT_CTX_T* const ctx_p )
{
    ctx_p->adapter = FAKE_NO_ADAPTER;
    ctx_p->found = 0U;
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ecat_port_sts_t ecat_port_scan( ECAT_PORT_CTX_T* const ctx_p, size_t* const count_p )
{
    ecat_port_sts_t sts;

    *count_p = 0U;
    if( FAKE_NO_ADAPTER == ctx_p->adapter )
    {
        sts = ECAT_PORT_STS_NOT_OPEN;
    }
    else if( 0 != ctx_p->adapter )
    {
        ctx_p->found = 0U;
        sts = ECAT_PORT_STS_NO_SLAVES;
    }
    else
    {
        /* A scan starts the ring again, as SOEM's does, and leaves it in PRE-OP. */
        ring_reset( ctx_p );
        ctx_p->found = FAKE_SLAVES;
        for( size_t idx = 0U; idx < FAKE_SLAVES; idx++ )
        {
            ctx_p->slaves[idx].state = ECAT_PORT_STATE_PREOP;
        }
        *count_p = ctx_p->found;
        sts = ECAT_PORT_STS_OK;
    }
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ecat_port_sts_t ecat_port_read_states( ECAT_PORT_CTX_T* const ctx_p )
{
    return ( FAKE_NO_ADAPTER == ctx_p->adapter ) ? ECAT_PORT_STS_NOT_OPEN : ECAT_PORT_STS_OK;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ecat_port_sts_t ecat_port_slave( ECAT_PORT_CTX_T* const ctx_p,
                                 uint16_t const position,
                                 ECAT_PORT_SLAVE_T* const slave_p )
{
    ecat_port_sts_t sts;

    if( ( 0U == position ) || ( position > ctx_p->found ) )
    {
        sts = ECAT_PORT_STS_NO_SLAVE;
    }
    else
    {
        *slave_p = ctx_p->slaves[position - 1U];
        sts = ECAT_PORT_STS_OK;
    }
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ecat_port_sts_t ecat_port_set_state( ECAT_PORT_CTX_T* const ctx_p,
                                     uint16_t const position,
                                     uint16_t const state,
                                     uint32_t const timeout_us )
{
    ecat_port_sts_t sts = ECAT_PORT_STS_OK;

    ( void )timeout_us;     /* the fake answers at once */
    if( FAKE_NO_ADAPTER == ctx_p->adapter )
    {
        sts = ECAT_PORT_STS_NOT_OPEN;
    }
    else if( position > ctx_p->found )
    {
        sts = ECAT_PORT_STS_NO_SLAVE;
    }
    else
    {
        size_t const first = ( 0U == position ) ? 0U : ( (size_t)position - 1U );
        size_t const last = ( 0U == position ) ? ctx_p->found : (size_t)position;

        for( size_t idx = first; idx < last; idx++ )
        {
            ECAT_PORT_SLAVE_T* const slave_p = &ctx_p->slaves[idx];
            uint16_t const now = (uint16_t)( slave_p->state & ECAT_PORT_STATE_MASK );

            if( change_allowed( now, state ) )
            {
                slave_p->state = state;
                slave_p->al_status = 0U;
            }
            else
            {
                slave_p->state = (uint16_t)( now | ECAT_PORT_STATE_ERROR );
                slave_p->al_status = FAKE_AL_INVALID_CHANGE;
                sts = ECAT_PORT_STS_STATE_REFUSED;
            }
        }
    }
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ecat_port_sts_t ecat_port_sdo_read( ECAT_PORT_CTX_T* const ctx_p,
                                    uint16_t const position,
                                    uint16_t const index,
                                    uint8_t const sub,
                                    uint8_t data[],
                                    size_t* const len_p,
                                    uint32_t* const abort_p )
{
    ecat_port_sts_t sts = mailbox_slave( ctx_p, position );

    *abort_p = 0U;
    if( ECAT_PORT_STS_OK == sts )
    {
        FAKE_OBJECT_T const* const object_p = object_find( ctx_p, index, sub, abort_p );

        if( NULL == object_p )
        {
            sts = ECAT_PORT_STS_SDO_ABORT;
        }
        else if( object_p->len > *len_p )
        {
            sts = ECAT_PORT_STS_TOO_LONG;
        }
        else
        {
            ( void )memcpy( data, object_p->value, object_p->len );
            *len_p = object_p->len;
        }
    }
    else
    {
        /* No mailbox to ask through. */
    }
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ecat_port_sts_t ecat_port_sdo_write( ECAT_PORT_CTX_T* const ctx_p,
                                     uint16_t const position,
                                     uint16_t const index,
                                     uint8_t const sub,
                                     uint8_t const data[],
                                     size_t const len,
                                     uint32_t* const abort_p )
{
    ecat_port_sts_t sts = mailbox_slave( ctx_p, position );

    *abort_p = 0U;
    if( ECAT_PORT_STS_OK == sts )
    {
        FAKE_OBJECT_T* const object_p = object_find( ctx_p, index, sub, abort_p );

        if( NULL == object_p )
        {
            sts = ECAT_PORT_STS_SDO_ABORT;
        }
        else if( !object_p->writable )
        {
            *abort_p = FAKE_ABORT_READ_ONLY;
            sts = ECAT_PORT_STS_SDO_ABORT;
        }
        else if( len != object_p->len )
        {
            *abort_p = FAKE_ABORT_LENGTH;
            sts = ECAT_PORT_STS_SDO_ABORT;
        }
        else
        {
            ( void )memcpy( object_p->value, data, len );
            if( 0x6060U == index )
            {
                /* The mode shown follows the mode asked for, as a drive's does. */
                uint32_t unused = 0U;
                FAKE_OBJECT_T* const shown_p = object_find( ctx_p, 0x6061U, 0U, &unused );

                if( NULL != shown_p )
                {
                    shown_p->value[0U] = data[0U];
                }
                else
                {
                    /* The dictionary always has 0x6061. */
                }
            }
            else
            {
                /* Nothing follows other objects. */
            }
        }
    }
    else
    {
        /* No mailbox to write through. */
    }
    return sts;
}

/*!@}*/

/*============================================================================*/
/*! \name Static functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Put the ring back as it is at power-up: slaves in INIT, the dictionary at its defaults.

    \param  ctx_p   The master.

    \return None.
*/
static void ring_reset( ECAT_PORT_CTX_T* const ctx_p )
{
    ECAT_PORT_SLAVE_T const drive =
    {
        .position = 1U, .name = "Fake drive", .vendor = 0x00000ACEU, .product = 0x00000402U,
        .revision = 0x00010000U, .state = ECAT_PORT_STATE_INIT, .al_status = 0U, .mailbox = true
    };
    ECAT_PORT_SLAVE_T const io =
    {
        .position = 2U, .name = "Fake IO", .vendor = 0x00000ACEU, .product = 0x00001001U,
        .revision = 0x00000001U, .state = ECAT_PORT_STATE_INIT, .al_status = 0U, .mailbox = false
    };
    size_t idx = 0U;

    ctx_p->slaves[0U] = drive;
    ctx_p->slaves[1U] = io;

    object_set( &ctx_p->objects[idx], 0x1000U, 0U, false, 4U, 0x00020192UL );    /* device type: CiA 402 drive */
    idx++;
    object_set( &ctx_p->objects[idx], 0x1008U, 0U, false, 0U, 0UL );            /* device name: set below */
    ( void )memcpy( ctx_p->objects[idx].value, "Fake drive", 10U );
    ctx_p->objects[idx].len = 10U;
    idx++;
    object_set( &ctx_p->objects[idx], 0x1018U, 0U, false, 1U, 4UL );            /* identity: highest sub */
    idx++;
    object_set( &ctx_p->objects[idx], 0x1018U, 1U, false, 4U, drive.vendor );
    idx++;
    object_set( &ctx_p->objects[idx], 0x1018U, 2U, false, 4U, drive.product );
    idx++;
    object_set( &ctx_p->objects[idx], 0x1018U, 3U, false, 4U, drive.revision );
    idx++;
    object_set( &ctx_p->objects[idx], 0x1018U, 4U, false, 4U, 12345UL );        /* serial number */
    idx++;
    object_set( &ctx_p->objects[idx], 0x6040U, 0U, true, 2U, 0UL );             /* controlword */
    idx++;
    object_set( &ctx_p->objects[idx], 0x6041U, 0U, false, 2U, 0x0250UL );       /* statusword: switch on disabled */
    idx++;
    object_set( &ctx_p->objects[idx], 0x6060U, 0U, true, 1U, 0UL );             /* modes of operation */
    idx++;
    object_set( &ctx_p->objects[idx], 0x6061U, 0U, false, 1U, 0UL );            /* modes of operation display */
    idx++;
    object_set( &ctx_p->objects[idx], 0x607AU, 0U, true, 4U, 0UL );             /* target position */
    idx++;
    object_set( &ctx_p->objects[idx], 0x6064U, 0U, false, 4U, 0UL );            /* position actual value */
    idx++;
    object_set( &ctx_p->objects[idx], 0x60FFU, 0U, true, 4U, 0UL );             /* target velocity */
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Fill in one dictionary entry with a number.

    \param  object_p    The entry.
    \param  index       Its index.
    \param  sub         Its sub-index.
    \param  writable    Whether an SDO download may change it.
    \param  len         How many bytes it is, up to 4.
    \param  value       Its value, stored little-endian.

    \return None.
*/
static void object_set( FAKE_OBJECT_T* const object_p,
                        uint16_t const index,
                        uint8_t const sub,
                        bool const writable,
                        size_t const len,
                        uint32_t const value )
{
    object_p->index = index;
    object_p->sub = sub;
    object_p->writable = writable;
    object_p->len = len;
    ( void )memset( object_p->value, 0, FAKE_VALUE_LEN );
    for( size_t idx = 0U; idx < len; idx++ )
    {
        object_p->value[idx] = (uint8_t)( value >> ( 8U * idx ) );
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  A dictionary entry, or the abort code a slave would send for it.

    \param  ctx_p   The master.
    \param  index   The object's index.
    \param  sub     Its sub-index.
    \param  abort_p The abort code, when there is no entry.

    \return The entry, or NULL.
*/
static FAKE_OBJECT_T* object_find( ECAT_PORT_CTX_T* const ctx_p,
                                   uint16_t const index,
                                   uint8_t const sub,
                                   uint32_t* const abort_p )
{
    FAKE_OBJECT_T* found_p = NULL;
    bool index_there = false;

    for( size_t idx = 0U; idx < FAKE_OBJECTS; idx++ )
    {
        FAKE_OBJECT_T* const object_p = &ctx_p->objects[idx];

        if( index == object_p->index )
        {
            index_there = true;
            if( sub == object_p->sub )
            {
                found_p = object_p;
            }
            else
            {
                /* Another sub of the same object. */
            }
        }
        else
        {
            /* Another object. */
        }
    }

    if( NULL != found_p )
    {
        /* Found: no abort. */
    }
    else if( index_there )
    {
        *abort_p = FAKE_ABORT_NO_SUB;
    }
    else
    {
        *abort_p = FAKE_ABORT_NO_OBJECT;
    }
    return found_p;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Whether a slave goes from one state to another when asked.

    \param  from    The state it is in.
    \param  to      The state asked for.

    \return true for a step up the INIT, PRE-OP, SAFE-OP, OP ladder, any step
            down it, PRE-OP to BOOT and back to INIT, and staying put.
*/
static bool change_allowed( uint16_t const from, uint16_t const to )
{
    bool allowed;

    if( ( from == to ) || ( ECAT_PORT_STATE_INIT == to ) )
    {
        allowed = true;
    }
    else if( ECAT_PORT_STATE_BOOT == to )
    {
        allowed = ( ECAT_PORT_STATE_PREOP == from );
    }
    else if( ECAT_PORT_STATE_BOOT == from )
    {
        allowed = false;    /* BOOT goes only back to INIT */
    }
    else if( to < from )
    {
        allowed = true;     /* any step down: OP, SAFE-OP, PRE-OP are 8, 4, 2 */
    }
    else
    {
        allowed = ( ( ECAT_PORT_STATE_INIT == from ) && ( ECAT_PORT_STATE_PREOP == to ) ) ||
                  ( ( ECAT_PORT_STATE_PREOP == from ) && ( ECAT_PORT_STATE_SAFEOP == to ) ) ||
                  ( ( ECAT_PORT_STATE_SAFEOP == from ) && ( ECAT_PORT_STATE_OP == to ) );
    }
    return allowed;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Whether a slave can be asked by SDO now.

    \param  ctx_p       The master.
    \param  position    1 for the first slave.

    \return ECAT_PORT_STS_OK; ECAT_PORT_STS_NOT_OPEN, _NO_SLAVE or _NO_MAILBOX;
            or ECAT_PORT_STS_TIMEOUT for a slave in INIT, whose mailbox is not
            running, so that a real master hears nothing back.
*/
static ecat_port_sts_t mailbox_slave( ECAT_PORT_CTX_T* const ctx_p, uint16_t const position )
{
    ecat_port_sts_t sts;

    if( FAKE_NO_ADAPTER == ctx_p->adapter )
    {
        sts = ECAT_PORT_STS_NOT_OPEN;
    }
    else if( ( 0U == position ) || ( position > ctx_p->found ) )
    {
        sts = ECAT_PORT_STS_NO_SLAVE;
    }
    else if( !ctx_p->slaves[position - 1U].mailbox )
    {
        sts = ECAT_PORT_STS_NO_MAILBOX;
    }
    else if( ECAT_PORT_STATE_INIT == ( ctx_p->slaves[position - 1U].state & ECAT_PORT_STATE_MASK ) )
    {
        sts = ECAT_PORT_STS_TIMEOUT;
    }
    else
    {
        sts = ECAT_PORT_STS_OK;
    }
    return sts;
}

/*!@}*/

/*============================================================================*/

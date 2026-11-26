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

    Process data as a real ring has it: the drive's PDO assignment is in its
    dictionary (0x1C12, 0x1C13, 0x1600, 0x1A00) for the helper to read, and its
    process data takes the same places. In SAFE-OP a slave sends inputs and
    ignores outputs; in OP it takes both. The drive walks the CiA 402 state
    machine from its controlword, however that arrives, and in Operation enabled
    follows its target: at once in profile position, by the target velocity
    each cycle in profile velocity. The I/O slave sends back as inputs the
    outputs it last took.

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

#include "ecat_os.h"

/*============================================================================*/
/*! \name Defines */
/*============================================================================*/
/*!@{*/

#define FAKE_SLAVES             ( 2U )      /*!< Slaves on the first adapter's ring */
#define FAKE_OBJECTS            ( 30U )     /*!< Entries in the drive's object dictionary */
#define FAKE_VALUE_LEN          ( 16U )     /*!< Longest value an entry holds */
#define FAKE_NO_ADAPTER         ( -1 )      /*!< ctx.adapter when none is open */

#define FAKE_AL_INVALID_CHANGE  ( 0x0011U ) /*!< AL status: invalid requested state change */

#define FAKE_ABORT_NO_OBJECT    ( 0x06020000UL )    /*!< SDO abort: object does not exist */
#define FAKE_ABORT_NO_SUB       ( 0x06090011UL )    /*!< SDO abort: sub-index does not exist */
#define FAKE_ABORT_READ_ONLY    ( 0x06010002UL )    /*!< SDO abort: attempt to write a read-only object */
#define FAKE_ABORT_LENGTH       ( 0x06070010UL )    /*!< SDO abort: data type does not match, length of service parameter does not match */

/* The process image, as 0x1600 and 0x1A00 lay the drive's out, the I/O slave's byte after it. */
#define FAKE_OUT_LEN            ( 12U )     /*!< Bytes of outputs: the drive's 11, the I/O slave's 1 */
#define FAKE_IN_LEN             ( 12U )     /*!< Bytes of inputs: the same */
#define FAKE_DRIVE_BITS         ( 88U )     /*!< The drive's process data each way: 16 + 8 + 32 + 32 bits */
#define FAKE_IO_BYTE            ( 11U )     /*!< The I/O slave's byte, in both parts */
#define FAKE_WKC_READ           ( 1U )      /*!< What a slave adds to the working counter for its inputs */
#define FAKE_WKC_WRITE          ( 2U )      /*!< ... and for taking its outputs */

/* The drive's controlword and statusword (CiA 402). */
#define FAKE_CW_VOLTAGE         ( 0x0002U ) /*!< Enable voltage: clear for Disable voltage */
#define FAKE_CW_SHUTDOWN_MASK   ( 0x0087U ) /*!< The bits that make Shutdown */
#define FAKE_CW_SHUTDOWN        ( 0x0006U ) /*!< Shutdown */
#define FAKE_CW_COMMAND_MASK    ( 0x008FU ) /*!< The bits that make Switch on and Enable operation */
#define FAKE_CW_SWITCH_ON       ( 0x0007U ) /*!< Switch on, or Disable operation */
#define FAKE_CW_ENABLE          ( 0x000FU ) /*!< Enable operation */
#define FAKE_SW_MASK            ( 0x006FU ) /*!< The statusword bits that say the state */
#define FAKE_SW_DISABLED        ( 0x0040U ) /*!< Switch on disabled */
#define FAKE_SW_READY           ( 0x0021U ) /*!< Ready to switch on */
#define FAKE_SW_SWITCHED_ON     ( 0x0023U ) /*!< Switched on */
#define FAKE_SW_ENABLED         ( 0x0027U ) /*!< Operation enabled */
#define FAKE_SW_ALWAYS          ( 0x0210U ) /*!< Remote and Voltage enabled, always set */
#define FAKE_MODE_PP            ( 1U )      /*!< Profile position */
#define FAKE_MODE_PV            ( 3U )      /*!< Profile velocity */
#define FAKE_PV_DIVIDER         ( 100 )     /*!< Each cycle in pv moves the position by the velocity over this */

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

/*! Where one object sits in the drive's process data. */
typedef struct
{
    uint16_t index;                 /*!< The object */
    size_t   byte;                  /*!< Its first byte in the drive's part of the image */
    size_t   len;                   /*!< How many bytes */
} FAKE_PDO_T;

/*! The fake master's state. */
struct ecat_port_ctx
{
    ECAT_OS_MUTEX_T   lock;                     /*!< The protocol thread and the cycle thread both use the ring */
    bool              lock_ready;               /*!< The lock has been made ready */
    int32_t           adapter;                  /*!< Which adapter is open, or FAKE_NO_ADAPTER */
    size_t            found;                    /*!< How many slaves the last scan found */
    bool              mapped;                   /*!< Whether the process data is mapped */
    uint8_t           io_out;                   /*!< The output byte the I/O slave last took */
    ECAT_PORT_SLAVE_T slaves[FAKE_SLAVES];      /*!< The slaves, as the ring has them */
    FAKE_OBJECT_T     objects[FAKE_OBJECTS];    /*!< The drive's (slave 1's) object dictionary */
};

/*!@}*/

/*============================================================================*/
/*! \name Static prototypes */
/*============================================================================*/
/*!@{*/

static void ring_reset( ECAT_PORT_CTX_T* const ctx_p );
static void close_ring( ECAT_PORT_CTX_T* const ctx_p );
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
static uint32_t value_get( ECAT_PORT_CTX_T* const ctx_p, uint16_t const index );
static void value_put( ECAT_PORT_CTX_T* const ctx_p, uint16_t const index, uint32_t const value );
static void drive_step( ECAT_PORT_CTX_T* const ctx_p );
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
    fake keeps the same shape. There is one master per helper process, shared
    by the protocol thread and the cycle thread under its lock.
*/
static ECAT_PORT_CTX_T fake_ctx = { .adapter = FAKE_NO_ADAPTER };   /*!< The one master */

/*! The adapters there are: the first has the ring on it, the second nothing. */
static ECAT_PORT_ADAPTER_T const fake_adapters[] =
{
    { .name = "\\Device\\NPF_{FAKE-0001}", .desc = "Fake Ethernet with a ring" },
    { .name = "\\Device\\NPF_{FAKE-0002}", .desc = "Fake Ethernet with nothing on it" }
};

/*! The drive's outputs, as 0x1600 maps them. */
static FAKE_PDO_T const drive_outputs[] =
{
    { 0x6040U, 0U, 2U },    /* controlword */
    { 0x6060U, 2U, 1U },    /* modes of operation */
    { 0x607AU, 3U, 4U },    /* target position */
    { 0x60FFU, 7U, 4U }     /* target velocity */
};

/*! The drive's inputs, as 0x1A00 maps them. */
static FAKE_PDO_T const drive_inputs[] =
{
    { 0x6041U, 0U, 2U },    /* statusword */
    { 0x6061U, 2U, 1U },    /* modes of operation display */
    { 0x6064U, 3U, 4U },    /* position actual value */
    { 0x606CU, 7U, 4U }     /* velocity actual value */
};

/*!@}*/

/*============================================================================*/
/*! \name Required functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.

    The first call makes the lock ready: it comes before any cycle thread exists.
*/
ECAT_PORT_CTX_T* ecat_port_instance( void )
{
    if( !fake_ctx.lock_ready )
    {
        ecat_os_mutex_init( &fake_ctx.lock );
        fake_ctx.lock_ready = true;
    }
    else
    {
        /* Ready. */
    }
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

    ecat_os_mutex_lock( &ctx_p->lock );
    close_ring( ctx_p );
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
    ecat_os_mutex_unlock( &ctx_p->lock );
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
void ecat_port_close( ECAT_PORT_CTX_T* const ctx_p )
{
    ecat_os_mutex_lock( &ctx_p->lock );
    close_ring( ctx_p );
    ecat_os_mutex_unlock( &ctx_p->lock );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ecat_port_sts_t ecat_port_scan( ECAT_PORT_CTX_T* const ctx_p, size_t* const count_p )
{
    ecat_port_sts_t sts;

    ecat_os_mutex_lock( &ctx_p->lock );
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
    ecat_os_mutex_unlock( &ctx_p->lock );
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ecat_port_sts_t ecat_port_read_states( ECAT_PORT_CTX_T* const ctx_p )
{
    ecat_port_sts_t sts;

    ecat_os_mutex_lock( &ctx_p->lock );
    sts = ( FAKE_NO_ADAPTER == ctx_p->adapter ) ? ECAT_PORT_STS_NOT_OPEN : ECAT_PORT_STS_OK;
    ecat_os_mutex_unlock( &ctx_p->lock );
    return sts;
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

    ecat_os_mutex_lock( &ctx_p->lock );
    if( ( 0U == position ) || ( position > ctx_p->found ) )
    {
        sts = ECAT_PORT_STS_NO_SLAVE;
    }
    else
    {
        *slave_p = ctx_p->slaves[position - 1U];
        sts = ECAT_PORT_STS_OK;
    }
    ecat_os_mutex_unlock( &ctx_p->lock );
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
    ecat_os_mutex_lock( &ctx_p->lock );
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
    ecat_os_mutex_unlock( &ctx_p->lock );
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
    ecat_port_sts_t sts;

    ecat_os_mutex_lock( &ctx_p->lock );
    sts = mailbox_slave( ctx_p, position );
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
    ecat_os_mutex_unlock( &ctx_p->lock );
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
    ecat_port_sts_t sts;

    ecat_os_mutex_lock( &ctx_p->lock );
    sts = mailbox_slave( ctx_p, position );
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
            drive_step( ctx_p );    /* the drive acts on a controlword or a mode however it came */
        }
    }
    else
    {
        /* No mailbox to write through. */
    }
    ecat_os_mutex_unlock( &ctx_p->lock );
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ecat_port_sts_t ecat_port_map( ECAT_PORT_CTX_T* const ctx_p, ECAT_PORT_IMAGE_T* const image_p )
{
    ecat_port_sts_t sts = ECAT_PORT_STS_OK;

    ecat_os_mutex_lock( &ctx_p->lock );
    ctx_p->mapped = false;
    if( FAKE_NO_ADAPTER == ctx_p->adapter )
    {
        sts = ECAT_PORT_STS_NOT_OPEN;
    }
    else if( 0U == ctx_p->found )
    {
        sts = ECAT_PORT_STS_NO_SLAVES;
    }
    else
    {
        for( size_t idx = 0U; idx < ctx_p->found; idx++ )
        {
            if( ECAT_PORT_STATE_PREOP != ctx_p->slaves[idx].state )
            {
                sts = ECAT_PORT_STS_BAD_STATE;
            }
            else
            {
                /* Ready to map. */
            }
        }
    }

    if( ECAT_PORT_STS_OK == sts )
    {
        /* As SOEM's mapping does: the slaves are asked for SAFE-OP once mapped. */
        for( size_t idx = 0U; idx < ctx_p->found; idx++ )
        {
            ctx_p->slaves[idx].state = ECAT_PORT_STATE_SAFEOP;
        }
        image_p->out_len = FAKE_OUT_LEN;
        image_p->in_len = FAKE_IN_LEN;
        image_p->expected_wkc = (uint16_t)( FAKE_SLAVES * ( FAKE_WKC_READ + FAKE_WKC_WRITE ) );
        ctx_p->io_out = 0U;
        ctx_p->mapped = true;
    }
    else
    {
        /* Nothing mapped. */
    }
    ecat_os_mutex_unlock( &ctx_p->lock );
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ecat_port_sts_t ecat_port_area( ECAT_PORT_CTX_T* const ctx_p,
                                uint16_t const position,
                                ECAT_PORT_AREA_T* const area_p )
{
    ecat_port_sts_t sts = ECAT_PORT_STS_OK;

    ecat_os_mutex_lock( &ctx_p->lock );
    if( !ctx_p->mapped || ( 0U == position ) || ( position > ctx_p->found ) )
    {
        sts = ECAT_PORT_STS_NO_SLAVE;
    }
    else if( 1U == position )
    {
        area_p->out_bit = 0U;
        area_p->out_bits = FAKE_DRIVE_BITS;
        area_p->in_bit = 0U;
        area_p->in_bits = FAKE_DRIVE_BITS;
    }
    else
    {
        area_p->out_bit = FAKE_IO_BYTE * 8U;
        area_p->out_bits = 8U;
        area_p->in_bit = FAKE_IO_BYTE * 8U;
        area_p->in_bits = 8U;
    }
    ecat_os_mutex_unlock( &ctx_p->lock );
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
void ecat_port_exchange( ECAT_PORT_CTX_T* const ctx_p,
                         uint8_t const outputs[],
                         uint8_t inputs[],
                         uint16_t* const wkc_p )
{
    uint16_t wkc = 0U;

    ecat_os_mutex_lock( &ctx_p->lock );
    if( ( 0 == ctx_p->adapter ) && ctx_p->mapped && ( FAKE_SLAVES == ctx_p->found ) )
    {
        uint16_t const drive = (uint16_t)( ctx_p->slaves[0U].state & ECAT_PORT_STATE_MASK );
        uint16_t const io = (uint16_t)( ctx_p->slaves[1U].state & ECAT_PORT_STATE_MASK );

        if( ECAT_PORT_STATE_OP == drive )
        {
            for( size_t idx = 0U; idx < ( sizeof( drive_outputs ) / sizeof( drive_outputs[0U] ) ); idx++ )
            {
                uint32_t value = 0U;

                for( size_t byte = 0U; byte < drive_outputs[idx].len; byte++ )
                {
                    value |= (uint32_t)outputs[drive_outputs[idx].byte + byte] << ( 8U * byte );
                }
                value_put( ctx_p, drive_outputs[idx].index, value );
            }
            wkc = (uint16_t)( wkc + FAKE_WKC_WRITE );
        }
        else
        {
            /* Outputs are not taken below OP. */
        }
        drive_step( ctx_p );
        if( ( ECAT_PORT_STATE_SAFEOP == drive ) || ( ECAT_PORT_STATE_OP == drive ) )
        {
            for( size_t idx = 0U; idx < ( sizeof( drive_inputs ) / sizeof( drive_inputs[0U] ) ); idx++ )
            {
                uint32_t const value = value_get( ctx_p, drive_inputs[idx].index );

                for( size_t byte = 0U; byte < drive_inputs[idx].len; byte++ )
                {
                    inputs[drive_inputs[idx].byte + byte] = (uint8_t)( value >> ( 8U * byte ) );
                }
            }
            wkc = (uint16_t)( wkc + FAKE_WKC_READ );
        }
        else
        {
            /* No inputs below SAFE-OP. */
        }

        if( ECAT_PORT_STATE_OP == io )
        {
            ctx_p->io_out = outputs[FAKE_IO_BYTE];
            wkc = (uint16_t)( wkc + FAKE_WKC_WRITE );
        }
        else
        {
            /* Outputs are not taken below OP. */
        }
        if( ( ECAT_PORT_STATE_SAFEOP == io ) || ( ECAT_PORT_STATE_OP == io ) )
        {
            inputs[FAKE_IO_BYTE] = ctx_p->io_out;
            wkc = (uint16_t)( wkc + FAKE_WKC_READ );
        }
        else
        {
            /* No inputs below SAFE-OP. */
        }
    }
    else
    {
        /* No ring, or nothing mapped: the frame comes back untouched. */
    }
    *wkc_p = wkc;
    ecat_os_mutex_unlock( &ctx_p->lock );
    return;
}

/*!@}*/

/*============================================================================*/
/*! \name Static functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Put the ring back as it is at power-up: slaves in INIT, nothing
            mapped, the dictionary at its defaults.

    \param  ctx_p   The master, locked.

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
    /* The dictionary: index, sub, writable, length, value. */
    static struct
    {
        uint16_t index;
        uint8_t  sub;
        bool     writable;
        uint8_t  len;
        uint32_t value;
    } const dictionary[FAKE_OBJECTS] =
    {
        { 0x1000U, 0U, false, 4U, 0x00020192UL },   /* device type: CiA 402 drive */
        { 0x1008U, 0U, false, 0U, 0UL },            /* device name: set below */
        { 0x1018U, 0U, false, 1U, 4UL },            /* identity: highest sub */
        { 0x1018U, 1U, false, 4U, 0x00000ACEUL },   /* vendor */
        { 0x1018U, 2U, false, 4U, 0x00000402UL },   /* product */
        { 0x1018U, 3U, false, 4U, 0x00010000UL },   /* revision */
        { 0x1018U, 4U, false, 4U, 12345UL },        /* serial number */
        { 0x1600U, 0U, false, 1U, 4UL },            /* first RxPDO: four entries */
        { 0x1600U, 1U, false, 4U, 0x60400010UL },   /* controlword, 16 bits */
        { 0x1600U, 2U, false, 4U, 0x60600008UL },   /* modes of operation, 8 bits */
        { 0x1600U, 3U, false, 4U, 0x607A0020UL },   /* target position, 32 bits */
        { 0x1600U, 4U, false, 4U, 0x60FF0020UL },   /* target velocity, 32 bits */
        { 0x1A00U, 0U, false, 1U, 4UL },            /* first TxPDO: four entries */
        { 0x1A00U, 1U, false, 4U, 0x60410010UL },   /* statusword, 16 bits */
        { 0x1A00U, 2U, false, 4U, 0x60610008UL },   /* modes of operation display, 8 bits */
        { 0x1A00U, 3U, false, 4U, 0x60640020UL },   /* position actual value, 32 bits */
        { 0x1A00U, 4U, false, 4U, 0x606C0020UL },   /* velocity actual value, 32 bits */
        { 0x1C12U, 0U, false, 1U, 1UL },            /* RxPDO assignment: one PDO */
        { 0x1C12U, 1U, false, 2U, 0x1600UL },
        { 0x1C13U, 0U, false, 1U, 1UL },            /* TxPDO assignment: one PDO */
        { 0x1C13U, 1U, false, 2U, 0x1A00UL },
        { 0x6040U, 0U, true, 2U, 0UL },             /* controlword */
        { 0x6041U, 0U, false, 2U, ( FAKE_SW_ALWAYS | FAKE_SW_DISABLED ) },     /* statusword */
        { 0x6060U, 0U, true, 1U, 0UL },             /* modes of operation */
        { 0x6061U, 0U, false, 1U, 0UL },            /* modes of operation display */
        { 0x6064U, 0U, false, 4U, 0UL },            /* position actual value */
        { 0x606CU, 0U, false, 4U, 0UL },            /* velocity actual value */
        { 0x607AU, 0U, true, 4U, 0UL },             /* target position */
        { 0x60FFU, 0U, true, 4U, 0UL },             /* target velocity */
        { 0x2100U, 0U, true, 2U, 0UL }              /* a manufacturer object, mapped nowhere */
    };

    ctx_p->slaves[0U] = drive;
    ctx_p->slaves[1U] = io;
    ctx_p->mapped = false;
    ctx_p->io_out = 0U;

    for( size_t idx = 0U; idx < FAKE_OBJECTS; idx++ )
    {
        object_set( &ctx_p->objects[idx], dictionary[idx].index, dictionary[idx].sub, dictionary[idx].writable,
                    dictionary[idx].len, dictionary[idx].value );
    }
    ( void )memcpy( ctx_p->objects[1U].value, "Fake drive", 10U );
    ctx_p->objects[1U].len = 10U;
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Close the adapter: nothing found, nothing mapped.

    \param  ctx_p   The master, locked.

    \return None.
*/
static void close_ring( ECAT_PORT_CTX_T* const ctx_p )
{
    ctx_p->adapter = FAKE_NO_ADAPTER;
    ctx_p->found = 0U;
    ctx_p->mapped = false;
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
    \brief  The value of a drive object at sub-index 0, as a number.

    \param  ctx_p   The master.
    \param  index   The object, one the dictionary has.

    \return Its value; 0 for an object it does not have.
*/
static uint32_t value_get( ECAT_PORT_CTX_T* const ctx_p, uint16_t const index )
{
    uint32_t unused = 0U;
    FAKE_OBJECT_T const* const object_p = object_find( ctx_p, index, 0U, &unused );
    uint32_t value = 0U;

    if( NULL != object_p )
    {
        for( size_t idx = 0U; idx < object_p->len; idx++ )
        {
            value |= (uint32_t)object_p->value[idx] << ( 8U * idx );
        }
    }
    else
    {
        /* Not in the dictionary. */
    }
    return value;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Set the value of a drive object at sub-index 0, keeping its length.

    \param  ctx_p   The master.
    \param  index   The object, one the dictionary has.
    \param  value   Its value.

    \return None.
*/
static void value_put( ECAT_PORT_CTX_T* const ctx_p, uint16_t const index, uint32_t const value )
{
    uint32_t unused = 0U;
    FAKE_OBJECT_T* const object_p = object_find( ctx_p, index, 0U, &unused );

    if( NULL != object_p )
    {
        for( size_t idx = 0U; idx < object_p->len; idx++ )
        {
            object_p->value[idx] = (uint8_t)( value >> ( 8U * idx ) );
        }
    }
    else
    {
        /* Not in the dictionary. */
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  One step of the drive: its CiA 402 state from its controlword, then its motion.

    \param  ctx_p   The master, locked.

    \return None.
*/
static void drive_step( ECAT_PORT_CTX_T* const ctx_p )
{
    uint32_t const control = value_get( ctx_p, 0x6040U );
    uint32_t const mode = value_get( ctx_p, 0x6060U );
    uint32_t state = value_get( ctx_p, 0x6041U ) & FAKE_SW_MASK;

    if( 0U == ( control & FAKE_CW_VOLTAGE ) )
    {
        state = FAKE_SW_DISABLED;
    }
    else if( FAKE_CW_SHUTDOWN == ( control & FAKE_CW_SHUTDOWN_MASK ) )
    {
        state = FAKE_SW_READY;
    }
    else if( FAKE_CW_SWITCH_ON == ( control & FAKE_CW_COMMAND_MASK ) )
    {
        if( ( FAKE_SW_READY == state ) || ( FAKE_SW_ENABLED == state ) )
        {
            state = FAKE_SW_SWITCHED_ON;
        }
        else
        {
            /* No transition from here. */
        }
    }
    else if( FAKE_CW_ENABLE == ( control & FAKE_CW_COMMAND_MASK ) )
    {
        if( FAKE_SW_SWITCHED_ON == state )
        {
            state = FAKE_SW_ENABLED;
        }
        else
        {
            /* Enable operation only from Switched on. */
        }
    }
    else
    {
        /* Nothing the fake acts on. */
    }
    value_put( ctx_p, 0x6041U, FAKE_SW_ALWAYS | state );
    value_put( ctx_p, 0x6061U, mode );

    if( ( FAKE_SW_ENABLED == state ) && ( FAKE_MODE_PP == mode ) )
    {
        value_put( ctx_p, 0x6064U, value_get( ctx_p, 0x607AU ) );
        value_put( ctx_p, 0x606CU, 0U );
    }
    else if( ( FAKE_SW_ENABLED == state ) && ( FAKE_MODE_PV == mode ) )
    {
        int32_t const velocity = (int32_t)value_get( ctx_p, 0x60FFU );
        int32_t const position = (int32_t)value_get( ctx_p, 0x6064U );

        value_put( ctx_p, 0x6064U, (uint32_t)( position + ( velocity / FAKE_PV_DIVIDER ) ) );
        value_put( ctx_p, 0x606CU, (uint32_t)velocity );
    }
    else
    {
        value_put( ctx_p, 0x606CU, 0U );    /* not moving */
    }
    return;
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

    \param  ctx_p       The master, locked.
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

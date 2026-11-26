/*!
    \file   ecat_port_soem.c
    \brief  The ring over SOEM 2.x and a real network card.

    SOEM's types are its own (int, uint16, boolean), and its functions take and
    return plain int: where they meet this file's fixed-width types the casts
    are written out, and each is in range because SOEM's limits (EC_MAXSLAVE,
    the mailbox size) are far below the types'.

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

#include <string.h>

/* SOEM first: it brings in winsock2.h, which must come before windows.h. */
#include "soem/soem.h"

#if defined( _WIN32 )
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif /* #if defined( _WIN32 ) */

/*============================================================================*/
/*! \name Defines */
/*============================================================================*/
/*!@{*/

#ifndef ECAT_SOEM_VERSION
#define ECAT_SOEM_VERSION       "unknown"   /*!< The SOEM release built against; the build sets it */
#endif /* #ifndef ECAT_SOEM_VERSION */

#define SOEM_PACKET_TOO_SMALL   ( 3U )      /*!< SOEM's packet error: the data container is too small for the value */
#define SOEM_IOMAP_LEN          ( 65536U )  /*!< SOEM's IOmap: SOEM maps into it without a bound, so far more than the image allows */
#define SOEM_GROUP              ( 0U )      /*!< All slaves are in SOEM's group 0 */

/*!@}*/

/*============================================================================*/
/*! \name Types */
/*============================================================================*/
/*!@{*/

/*! The SOEM master's state. */
struct ecat_port_ctx
{
    ecx_contextt      soem;                     /*!< SOEM's context: the port, the slave list, the error list */
    bool              open;                     /*!< Whether ecx_init() has opened an adapter */
    bool              mapped;                   /*!< Whether the slaves' process data is mapped into iomap */
    ECAT_PORT_IMAGE_T image;                    /*!< The image's size, once mapped */
    uint8             iomap[SOEM_IOMAP_LEN];    /*!< SOEM's process image: outputs, then inputs */
};

/*!@}*/

/*============================================================================*/
/*! \name Static prototypes */
/*============================================================================*/
/*!@{*/

static void copy_text( char dest[], size_t const max, char const src[] );
static ecat_port_sts_t mailbox_slave( ECAT_PORT_CTX_T* const ctx_p, uint16_t const position );
static ecat_port_sts_t sdo_failure( ECAT_PORT_CTX_T* const ctx_p, uint32_t* const abort_p );

/*!@}*/

/*============================================================================*/
/*! \name Static data */
/*============================================================================*/
/*!@{*/

/*
    MISRA C:2023 Dir 4.12 / the coding standard's "no file-scope mutable state":
    deviation. SOEM's ecx_contextt holds the whole slave list and its mailbox
    buffers, far too large for a stack, and the process image. There is one
    master per helper process. The protocol thread uses it, and the cycle
    thread too through ecat_port_exchange() alone, which SOEM allows alongside
    mailboxes and states; the protocol thread maps and closes only with no
    cycle running.
*/
static ECAT_PORT_CTX_T soem_ctx;    /*!< The one master */

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
    return &soem_ctx;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
char const* ecat_port_version( void )
{
    return "SOEM " ECAT_SOEM_VERSION;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.

    wpcap.dll and Packet.dll are delay-loaded (see CMakeLists.txt), so the
    helper starts without them. Npcap puts them in System32\Npcap, which is not
    searched by default: that folder is added, and wpcap loaded, here.
*/
ecat_port_sts_t ecat_port_check( void )
{
#if defined( _WIN32 )
    ecat_port_sts_t sts = ECAT_PORT_STS_NO_DRIVER;
    wchar_t folder[MAX_PATH];
    UINT const len = GetSystemDirectoryW( folder, MAX_PATH );

    if( ( 0U != len ) && ( ( len + 7U ) < MAX_PATH ) )
    {
        ( void )wcscat( folder, L"\\Npcap" );
        ( void )SetDllDirectoryW( folder );
        if( NULL != LoadLibraryW( L"wpcap.dll" ) )
        {
            sts = ECAT_PORT_STS_OK;     /* kept loaded for the life of the process */
        }
        else
        {
            /* Not installed. */
        }
    }
    else
    {
        /* No system folder to look in. */
    }
    return sts;
#else /* #if defined( _WIN32 ) */
    return ECAT_PORT_STS_OK;    /* raw sockets: a permission, found out when an adapter is opened */
#endif /* #if defined( _WIN32 ) */
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
size_t ecat_port_adapters( ECAT_PORT_ADAPTER_T list[], size_t const max )
{
    ec_adaptert* const first_p = ec_find_adapters();
    size_t count = 0U;

    for( ec_adaptert const* adapter_p = first_p; ( NULL != adapter_p ) && ( count < max ); adapter_p = adapter_p->next )
    {
        copy_text( list[count].name, ECAT_PORT_NAME_LEN, adapter_p->name );
        copy_text( list[count].desc, ECAT_PORT_NAME_LEN, adapter_p->desc );
        count++;
    }

    if( NULL != first_p )
    {
        ec_free_adapters( first_p );
    }
    else
    {
        /* No adapters, or no Npcap to list them with. */
    }
    return count;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
ecat_port_sts_t ecat_port_open( ECAT_PORT_CTX_T* const ctx_p, char const name[] )
{
    ecat_port_sts_t sts;

    ecat_port_close( ctx_p );
    ( void )memset( &ctx_p->soem, 0, sizeof( ctx_p->soem ) );
    if( ecx_init( &ctx_p->soem, name ) > 0 )
    {
        ctx_p->open = true;
        sts = ECAT_PORT_STS_OK;
    }
    else
    {
        sts = ECAT_PORT_STS_NO_ADAPTER;
    }
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.
*/
void ecat_port_close( ECAT_PORT_CTX_T* const ctx_p )
{
    if( ctx_p->open )
    {
        ecx_close( &ctx_p->soem );
        ctx_p->open = false;
    }
    else
    {
        /* Nothing open. */
    }
    ctx_p->soem.slavecount = 0;
    ctx_p->mapped = false;
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
    ctx_p->mapped = false;      /* a scan configures the ring afresh */
    if( !ctx_p->open )
    {
        sts = ECAT_PORT_STS_NOT_OPEN;
    }
    else if( ecx_config_init( &ctx_p->soem ) <= 0 )
    {
        ctx_p->soem.slavecount = 0;
        sts = ECAT_PORT_STS_NO_SLAVES;
    }
    else
    {
        /* ecx_config_init() asks for PRE-OP; wait for it, then read where each got to. */
        ( void )ecx_statecheck( &ctx_p->soem, 0U, EC_STATE_PRE_OP, EC_TIMEOUTSTATE );
        ( void )ecx_readstate( &ctx_p->soem );
        *count_p = (size_t)ctx_p->soem.slavecount;
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
    ecat_port_sts_t sts;

    if( !ctx_p->open )
    {
        sts = ECAT_PORT_STS_NOT_OPEN;
    }
    else
    {
        ( void )ecx_readstate( &ctx_p->soem );
        sts = ECAT_PORT_STS_OK;
    }
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

    if( ( 0U == position ) || ( (int32_t)position > ctx_p->soem.slavecount ) )
    {
        sts = ECAT_PORT_STS_NO_SLAVE;
    }
    else
    {
        ec_slavet const* const soem_p = &ctx_p->soem.slavelist[position];

        slave_p->position = position;
        copy_text( slave_p->name, ECAT_PORT_SLAVE_NAME_LEN, soem_p->name );
        slave_p->vendor = soem_p->eep_man;
        slave_p->product = soem_p->eep_id;
        slave_p->revision = soem_p->eep_rev;
        slave_p->state = soem_p->state;
        slave_p->al_status = soem_p->ALstatuscode;
        slave_p->mailbox = ( 0U != ( soem_p->mbx_proto & ECT_MBXPROT_COE ) ) && ( 0U != soem_p->mbx_l );
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
    ecat_port_sts_t sts;

    if( !ctx_p->open )
    {
        sts = ECAT_PORT_STS_NOT_OPEN;
    }
    else if( (int32_t)position > ctx_p->soem.slavecount )
    {
        sts = ECAT_PORT_STS_NO_SLAVE;
    }
    else
    {
        bool acknowledge = false;

        /* A slave showing an error takes no new state until the error is
           acknowledged, which is the same request with the ACK bit. */
        ( void )ecx_readstate( &ctx_p->soem );
        for( int32_t idx = ( 0U == position ) ? 1 : (int32_t)position;
             idx <= ( ( 0U == position ) ? ctx_p->soem.slavecount : (int32_t)position );
             idx++ )
        {
            if( 0U != ( ctx_p->soem.slavelist[idx].state & EC_STATE_ERROR ) )
            {
                acknowledge = true;
            }
            else
            {
                /* No error to acknowledge. */
            }
        }

        ctx_p->soem.slavelist[position].state = (uint16)( acknowledge ? ( state | EC_STATE_ACK ) : state );
        ( void )ecx_writestate( &ctx_p->soem, position );
        ( void )ecx_statecheck( &ctx_p->soem, position, state, (int)timeout_us );
        ( void )ecx_readstate( &ctx_p->soem );

        if( state == ctx_p->soem.slavelist[position].state )
        {
            sts = ECAT_PORT_STS_OK;
        }
        else
        {
            sts = ECAT_PORT_STS_STATE_REFUSED;
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
        int size = (int)*len_p;     /* SOEM's own type; the buffer is far smaller than INT_MAX */
        int const wkc = ecx_SDOread( &ctx_p->soem, position, index, sub, FALSE, &size, data, EC_TIMEOUTRXM );

        if( wkc > 0 )
        {
            *len_p = (size_t)size;
        }
        else
        {
            sts = sdo_failure( ctx_p, abort_p );
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
        int const wkc = ecx_SDOwrite( &ctx_p->soem, position, index, sub, FALSE, (int)len, data, EC_TIMEOUTRXM );

        if( wkc <= 0 )
        {
            sts = sdo_failure( ctx_p, abort_p );
        }
        else
        {
            /* Written. */
        }
    }
    else
    {
        /* No mailbox to write through. */
    }
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.

    ecx_config_map_group() reads each CoE slave's PDO assignment by SDO, lays
    the process data out in iomap, writes the sync managers and FMMUs, and asks
    each slave for SAFE-OP on its way.
*/
ecat_port_sts_t ecat_port_map( ECAT_PORT_CTX_T* const ctx_p, ECAT_PORT_IMAGE_T* const image_p )
{
    ecat_port_sts_t sts = ECAT_PORT_STS_OK;

    ctx_p->mapped = false;
    if( !ctx_p->open )
    {
        sts = ECAT_PORT_STS_NOT_OPEN;
    }
    else if( ctx_p->soem.slavecount <= 0 )
    {
        sts = ECAT_PORT_STS_NO_SLAVES;
    }
    else
    {
        ( void )ecx_readstate( &ctx_p->soem );
        for( int32_t idx = 1; idx <= ctx_p->soem.slavecount; idx++ )
        {
            if( EC_STATE_PRE_OP != ctx_p->soem.slavelist[idx].state )
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
        ec_groupt const* const group_p = &ctx_p->soem.grouplist[SOEM_GROUP];

        ( void )memset( ctx_p->iomap, 0, sizeof( ctx_p->iomap ) );
        ( void )ecx_config_map_group( &ctx_p->soem, ctx_p->iomap, SOEM_GROUP );
        ( void )ecx_statecheck( &ctx_p->soem, 0U, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE );
        ( void )ecx_readstate( &ctx_p->soem );

        if( ( group_p->Obytes > ECAT_PORT_IMAGE_LEN ) || ( group_p->Ibytes > ECAT_PORT_IMAGE_LEN ) )
        {
            sts = ECAT_PORT_STS_TOO_LONG;
        }
        else
        {
            image_p->out_len = (size_t)group_p->Obytes;
            image_p->in_len = (size_t)group_p->Ibytes;
            /* An LRW counts 1 for each slave that reads and 2 for each that writes. */
            image_p->expected_wkc = (uint16_t)( ( group_p->outputsWKC * 2U ) + group_p->inputsWKC );
            ctx_p->image = *image_p;
            ctx_p->mapped = true;
        }
    }
    else
    {
        /* Nothing mapped. */
    }
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
    ecat_port_sts_t sts;

    if( !ctx_p->mapped || ( 0U == position ) || ( (int32_t)position > ctx_p->soem.slavecount ) )
    {
        sts = ECAT_PORT_STS_NO_SLAVE;
    }
    else
    {
        ec_slavet const* const slave_p = &ctx_p->soem.slavelist[position];
        ec_groupt const* const group_p = &ctx_p->soem.grouplist[SOEM_GROUP];

        ( void )memset( area_p, 0, sizeof( *area_p ) );
        if( ( NULL != slave_p->outputs ) && ( 0U != slave_p->Obits ) )
        {
            area_p->out_bit = ( (uint32_t)( slave_p->outputs - group_p->outputs ) * 8U ) + slave_p->Ostartbit;
            area_p->out_bits = slave_p->Obits;
        }
        else
        {
            /* No outputs. */
        }
        if( ( NULL != slave_p->inputs ) && ( 0U != slave_p->Ibits ) )
        {
            area_p->in_bit = ( (uint32_t)( slave_p->inputs - group_p->inputs ) * 8U ) + slave_p->Istartbit;
            area_p->in_bits = slave_p->Ibits;
        }
        else
        {
            /* No inputs. */
        }
        sts = ECAT_PORT_STS_OK;
    }
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_port.h.

    SOEM keeps process data and mailboxes apart itself -- each frame has its
    own index in the port's buffers, under SOEM's own locks -- so this runs on
    the cycle thread while the protocol thread uses SDOs and states.
*/
void ecat_port_exchange( ECAT_PORT_CTX_T* const ctx_p,
                         uint8_t const outputs[],
                         uint8_t inputs[],
                         uint16_t* const wkc_p )
{
    *wkc_p = 0U;
    if( ctx_p->mapped )
    {
        ec_groupt const* const group_p = &ctx_p->soem.grouplist[SOEM_GROUP];
        int wkc;

        ( void )memcpy( group_p->outputs, outputs, ctx_p->image.out_len );
        ( void )ecx_send_processdata( &ctx_p->soem );
        wkc = ecx_receive_processdata( &ctx_p->soem, EC_TIMEOUTRET );
        ( void )memcpy( inputs, group_p->inputs, ctx_p->image.in_len );
        if( wkc > 0 )
        {
            *wkc_p = (uint16_t)wkc;
        }
        else
        {
            /* No frame came back. */
        }
    }
    else
    {
        /* Nothing mapped to send. */
    }
    return;
}

/*!@}*/

/*============================================================================*/
/*! \name Static functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Copy a string, cut short if it does not fit, always terminated.

    \param  dest    Where to.
    \param  max     How many \p dest holds, terminator included.
    \param  src     The string, terminated.

    \return None.
*/
static void copy_text( char dest[], size_t const max, char const src[] )
{
    size_t idx = 0U;

    while( ( ( idx + 1U ) < max ) && ( '\0' != src[idx] ) )
    {
        dest[idx] = src[idx];
        idx++;
    }
    dest[idx] = '\0';
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Whether a slave can be asked by SDO now.

    \param  ctx_p       The master.
    \param  position    1 for the first slave.

    \return ECAT_PORT_STS_OK, or ECAT_PORT_STS_NOT_OPEN, _NO_SLAVE or _NO_MAILBOX.
*/
static ecat_port_sts_t mailbox_slave( ECAT_PORT_CTX_T* const ctx_p, uint16_t const position )
{
    ecat_port_sts_t sts;
    ECAT_PORT_SLAVE_T slave;

    if( !ctx_p->open )
    {
        sts = ECAT_PORT_STS_NOT_OPEN;
    }
    else if( ECAT_PORT_STS_OK != ecat_port_slave( ctx_p, position, &slave ) )
    {
        sts = ECAT_PORT_STS_NO_SLAVE;
    }
    else if( !slave.mailbox )
    {
        sts = ECAT_PORT_STS_NO_MAILBOX;
    }
    else
    {
        sts = ECAT_PORT_STS_OK;
    }
    return sts;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Why an SDO transfer failed, from SOEM's error list, which it empties.

    \param  ctx_p   The master.
    \param  abort_p The abort code, when the slave sent one.

    \return ECAT_PORT_STS_SDO_ABORT, ECAT_PORT_STS_TOO_LONG, or ECAT_PORT_STS_TIMEOUT
            when SOEM noted nothing: no answer came.
*/
static ecat_port_sts_t sdo_failure( ECAT_PORT_CTX_T* const ctx_p, uint32_t* const abort_p )
{
    ecat_port_sts_t sts = ECAT_PORT_STS_TIMEOUT;
    ec_errort error;

    while( ecx_poperror( &ctx_p->soem, &error ) )
    {
        if( EC_ERR_TYPE_SDO_ERROR == error.Etype )
        {
            *abort_p = (uint32_t)error.AbortCode;
            sts = ECAT_PORT_STS_SDO_ABORT;
        }
        else if( ( EC_ERR_TYPE_PACKET_ERROR == error.Etype ) && ( SOEM_PACKET_TOO_SMALL == error.ErrorCode ) &&
                 ( ECAT_PORT_STS_SDO_ABORT != sts ) )
        {
            sts = ECAT_PORT_STS_TOO_LONG;
        }
        else
        {
            /* An emergency or another protocol's error: not this transfer's answer. */
        }
    }
    return sts;
}

/*!@}*/

/*============================================================================*/

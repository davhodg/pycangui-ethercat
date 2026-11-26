/*!
    \file   ecat_cycle.c
    \brief  The cyclic thread, and the outputs and inputs it shares with the protocol thread.

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

#include "ecat_cycle.h"

#include <string.h>

#include "ecat_os.h"

/*============================================================================*/
/*! \name Types */
/*============================================================================*/
/*!@{*/

/*! The cycle: its thread, and what it shares with the protocol thread under its lock. */
typedef struct
{
    ECAT_OS_MUTEX_T   lock;         /*!< Guards everything below but the thread */
    bool              lock_ready;   /*!< The lock has been made ready */
    ECAT_OS_THREAD_T  thread;       /*!< The cyclic thread */
    ECAT_PORT_CTX_T*  port_p;       /*!< The master it cycles */
    bool              stop;         /*!< Asked to stop */
    bool              reached;      /*!< expected_wkc has come back at least once */
    ECAT_CYCLE_SNAP_T shared;       /*!< Counters, outputs to send, inputs taken */
} CYCLE_T;

/*!@}*/

/*============================================================================*/
/*! \name Static prototypes */
/*============================================================================*/
/*!@{*/

static void cycle_thread( void* const arg_p );
static void make_ready( void );

/*!@}*/

/*============================================================================*/
/*! \name Static data */
/*============================================================================*/
/*!@{*/

/*
    MISRA C:2023 Dir 4.12 / the coding standard's "no file-scope mutable state":
    deviation. The cycle is shared by two threads, the protocol's and its own,
    and there is one per helper process; it holds two images of 4 KB.
*/
static CYCLE_T cycle;   /*!< The one cycle */

/*!@}*/

/*============================================================================*/
/*! \name Public functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_cycle.h.
*/
bool ecat_cycle_start( ECAT_PORT_CTX_T* const port_p,
                       ECAT_PORT_IMAGE_T const* const image_p,
                       uint32_t const period_us )
{
    bool started;

    ecat_cycle_stop();
    make_ready();

    ecat_os_mutex_lock( &cycle.lock );
    ( void )memset( &cycle.shared, 0, sizeof( cycle.shared ) );
    cycle.shared.running = true;
    cycle.shared.period_us = period_us;
    cycle.shared.expected_wkc = image_p->expected_wkc;
    cycle.shared.out_len = image_p->out_len;
    cycle.shared.in_len = image_p->in_len;
    cycle.port_p = port_p;
    cycle.stop = false;
    cycle.reached = false;
    ecat_os_mutex_unlock( &cycle.lock );

    started = ecat_os_thread_start( &cycle.thread, cycle_thread, &cycle, true );
    if( !started )
    {
        ecat_os_mutex_lock( &cycle.lock );
        cycle.shared.running = false;
        ecat_os_mutex_unlock( &cycle.lock );
    }
    else
    {
        /* Going. */
    }
    return started;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_cycle.h.
*/
void ecat_cycle_stop( void )
{
    make_ready();

    ecat_os_mutex_lock( &cycle.lock );
    cycle.stop = true;
    ecat_os_mutex_unlock( &cycle.lock );

    ecat_os_thread_join( &cycle.thread );

    ecat_os_mutex_lock( &cycle.lock );
    cycle.shared.running = false;
    ecat_os_mutex_unlock( &cycle.lock );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_cycle.h.
*/
bool ecat_cycle_put( uint32_t const bit, uint32_t const bits, uint8_t const data[] )
{
    bool ok;

    make_ready();
    ecat_os_mutex_lock( &cycle.lock );
    ok = cycle.shared.running && ( 0U != bits ) &&
         ( ( (uint64_t)bit + (uint64_t)bits ) <= ( (uint64_t)cycle.shared.out_len * 8U ) );
    if( ok )
    {
        for( uint32_t idx = 0U; idx < bits; idx++ )
        {
            uint32_t const at = bit + idx;
            uint8_t const mask = (uint8_t)( 1U << ( at % 8U ) );

            if( 0U != ( data[idx / 8U] & ( 1U << ( idx % 8U ) ) ) )
            {
                cycle.shared.outputs[at / 8U] |= mask;
            }
            else
            {
                cycle.shared.outputs[at / 8U] &= (uint8_t)~mask;
            }
        }
    }
    else
    {
        /* Not running, or not all in the outputs. */
    }
    ecat_os_mutex_unlock( &cycle.lock );
    return ok;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_cycle.h.
*/
void ecat_cycle_snapshot( ECAT_CYCLE_SNAP_T* const snap_p )
{
    make_ready();
    ecat_os_mutex_lock( &cycle.lock );
    *snap_p = cycle.shared;
    ecat_os_mutex_unlock( &cycle.lock );
    return;
}

/*!@}*/

/*============================================================================*/
/*! \name Static functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  The cyclic thread: exchange, count, wait for the next period, until asked to stop.

    The ring is worked outside the lock, on copies, so the protocol thread never
    waits on a frame; only the copying in and out is under it.

    \param  arg_p   The cycle.

    \return None.
*/
static void cycle_thread( void* const arg_p )
{
    CYCLE_T* const cycle_p = (CYCLE_T*)arg_p;
    uint8_t outputs[ECAT_PORT_IMAGE_LEN];
    uint8_t inputs[ECAT_PORT_IMAGE_LEN];
    ECAT_OS_TIMER_T timer;
    size_t out_len;
    size_t in_len;
    uint64_t period_us;
    uint64_t due_us;
    bool going = true;

    ( void )memset( inputs, 0, sizeof( inputs ) );
    ecat_os_mutex_lock( &cycle_p->lock );
    out_len = cycle_p->shared.out_len;
    in_len = cycle_p->shared.in_len;
    period_us = cycle_p->shared.period_us;
    ecat_os_mutex_unlock( &cycle_p->lock );

    ecat_os_timer_open( &timer );
    due_us = ecat_os_now_us();
    while( going )
    {
        uint16_t wkc = 0U;
        uint64_t now_us;

        ecat_os_sleep_until( &timer, due_us );
        due_us += period_us;

        ecat_os_mutex_lock( &cycle_p->lock );
        ( void )memcpy( outputs, cycle_p->shared.outputs, out_len );
        ecat_os_mutex_unlock( &cycle_p->lock );

        ecat_port_exchange( cycle_p->port_p, outputs, inputs, &wkc );

        ecat_os_mutex_lock( &cycle_p->lock );
        ( void )memcpy( cycle_p->shared.inputs, inputs, in_len );
        cycle_p->shared.cycles++;
        cycle_p->shared.wkc = wkc;
        if( wkc >= cycle_p->shared.expected_wkc )
        {
            cycle_p->reached = true;
        }
        else if( cycle_p->reached )
        {
            cycle_p->shared.wkc_errors++;   /* a slave dropped out of the exchange */
        }
        else
        {
            /* Not all in OP yet: short is expected until they are. */
        }

        now_us = ecat_os_now_us();
        if( now_us > due_us )
        {
            cycle_p->shared.late++;
            due_us = now_us;    /* start again from now, rather than run to catch up */
        }
        else
        {
            /* On time. */
        }
        going = !cycle_p->stop;
        ecat_os_mutex_unlock( &cycle_p->lock );
    }
    ecat_os_timer_close( &timer );
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Make the lock ready the first time it is needed.

    Only the protocol thread calls in here, and the cyclic thread does not
    exist until after this has run once, so the check needs no lock itself.

    \return None.
*/
static void make_ready( void )
{
    if( !cycle.lock_ready )
    {
        ecat_os_mutex_init( &cycle.lock );
        cycle.lock_ready = true;
    }
    else
    {
        /* Ready. */
    }
    return;
}

/*!@}*/

/*============================================================================*/

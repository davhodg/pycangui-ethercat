/*!
    \file   ecat_cycle.h
    \brief  The cyclic thread: the process image round the ring every period.

    Most drives allow Operation enabled only in OP, and a slave stays in OP only
    while process data reaches it every cycle, or its sync-manager watchdog drops
    it to SAFE-OP. So once the ring is mapped, a thread of its own sends the
    outputs and takes the inputs every period, whatever the protocol thread is
    doing -- an SDO can take hundreds of milliseconds.

    The protocol thread changes the outputs and copies out the inputs and the
    counters; it never waits on the ring for either.

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

#ifndef ECAT_CYCLE_H
#define ECAT_CYCLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecat_port.h"

/*============================================================================*/
/*! \name Defines */
/*============================================================================*/
/*!@{*/

#define ECAT_CYCLE_MIN_US       ( 1000U )       /*!< Shortest period: 1 ms */
#define ECAT_CYCLE_MAX_US       ( 100000U )     /*!< Longest period: 100 ms, inside a slave's usual watchdog */
#define ECAT_CYCLE_DEFAULT_US   ( 4000U )       /*!< The period unless asked otherwise: enough for pp and pv */

/*!@}*/

/*============================================================================*/
/*! \name Types */
/*============================================================================*/
/*!@{*/

/*! The cycle as it was at one moment. */
typedef struct
{
    bool     running;                       /*!< Whether the thread is going */
    uint32_t period_us;                     /*!< Its period */
    uint32_t cycles;                        /*!< Cycles since it started; wraps */
    uint16_t wkc;                           /*!< The last cycle's working counter */
    uint16_t expected_wkc;                  /*!< What it is when every slave takes part */
    uint32_t wkc_errors;                    /*!< Cycles short of expected_wkc, once it had been reached */
    uint32_t late;                          /*!< Cycles that ended after the next was due */
    size_t   out_len;                       /*!< Bytes of outputs */
    size_t   in_len;                        /*!< Bytes of inputs */
    uint8_t  outputs[ECAT_PORT_IMAGE_LEN];  /*!< The outputs being sent */
    uint8_t  inputs[ECAT_PORT_IMAGE_LEN];   /*!< The last inputs taken */
} ECAT_CYCLE_SNAP_T;

/*!@}*/

/*============================================================================*/
/*! \name Public functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Start the cycle over a mapped ring, its outputs all zero.

    Stops the one running first.

    \param  port_p      The master, mapped.
    \param  image_p     The image the map made.
    \param  period_us   The period, ECAT_CYCLE_MIN_US to ECAT_CYCLE_MAX_US.

    \return true when it is running.
*/
bool ecat_cycle_start( ECAT_PORT_CTX_T* const port_p,
                       ECAT_PORT_IMAGE_T const* const image_p,
                       uint32_t const period_us );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Stop the cycle, if it is running, and wait until it has.

    \return None.
*/
void ecat_cycle_stop( void );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Change some bits of the outputs, from the next cycle on.

    EtherCAT's order: the value's lowest bit goes to \p bit, its next to the
    bit after, a byte's bits counted from its lowest, so a value of whole bytes
    at a whole byte lands little-endian.

    \param  bit     The first bit, counted from the first bit of the outputs.
    \param  bits    How many.
    \param  data    The value, little-endian, at least (bits + 7) / 8 bytes.

    \return false when the cycle is not running, or the bits are not all in the outputs.
*/
bool ecat_cycle_put( uint32_t const bit, uint32_t const bits, uint8_t const data[] );

/*----------------------------------------------------------------------------*/
/*!
    \brief  The cycle as it is now: counters, outputs and inputs.

    \param  snap_p  Filled in.

    \return None.
*/
void ecat_cycle_snapshot( ECAT_CYCLE_SNAP_T* const snap_p );

/*!@}*/

#endif /* #ifndef ECAT_CYCLE_H */

/*============================================================================*/

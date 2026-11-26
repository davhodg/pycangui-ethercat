/*!
    \file   ecat_port.h
    \brief  The ring as the helper sees it: required functions over the EtherCAT master.

    The helper never calls SOEM directly. Everything it does to the ring goes
    through these functions, which have two implementations: ecat_port_soem.c
    over SOEM and a real network card, and ecat_port_fake.c, a scripted ring
    the tests run against. Which one is linked decides which helper is built.

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

#ifndef ECAT_PORT_H
#define ECAT_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*============================================================================*/
/*! \name Defines */
/*============================================================================*/
/*!@{*/

#define ECAT_PORT_NAME_LEN      ( 128U )    /*!< Longest adapter name or description, with its terminator */
#define ECAT_PORT_SLAVE_NAME_LEN ( 41U )    /*!< Longest slave name, with its terminator (SOEM's EC_MAXNAME + 1) */
#define ECAT_PORT_MAX_ADAPTERS  ( 32U )     /*!< Most adapters listed */
#define ECAT_PORT_MAX_SLAVES    ( 200U )    /*!< Most slaves on the ring (SOEM's EC_MAXSLAVE) */

#define ECAT_PORT_STATE_INIT    ( 0x01U )   /*!< EtherCAT state INIT */
#define ECAT_PORT_STATE_PREOP   ( 0x02U )   /*!< EtherCAT state PRE-OP */
#define ECAT_PORT_STATE_BOOT    ( 0x03U )   /*!< EtherCAT state BOOT */
#define ECAT_PORT_STATE_SAFEOP  ( 0x04U )   /*!< EtherCAT state SAFE-OP */
#define ECAT_PORT_STATE_OP      ( 0x08U )   /*!< EtherCAT state OP */
#define ECAT_PORT_STATE_MASK    ( 0x0FU )   /*!< The state bits of a slave's AL status register */
#define ECAT_PORT_STATE_ERROR   ( 0x10U )   /*!< Set beside the state when the slave refused a change */

/*!@}*/

/*============================================================================*/
/*! \name Types */
/*============================================================================*/
/*!@{*/

/*! What a ring operation came to. */
typedef enum
{
    ECAT_PORT_STS_OK = 0,           /*!< Done */
    ECAT_PORT_STS_NO_DRIVER,        /*!< What sends the frames is not installed: Npcap, on Windows */
    ECAT_PORT_STS_NOT_OPEN,         /*!< No adapter is open */
    ECAT_PORT_STS_NO_ADAPTER,       /*!< The adapter could not be opened (missing, or no permission, or no Npcap) */
    ECAT_PORT_STS_NO_SLAVES,        /*!< The scan found nothing on the ring */
    ECAT_PORT_STS_NO_SLAVE,         /*!< No slave at that position */
    ECAT_PORT_STS_NO_MAILBOX,       /*!< The slave has no mailbox, so no SDOs */
    ECAT_PORT_STS_SDO_ABORT,        /*!< The slave answered with an SDO abort code */
    ECAT_PORT_STS_TIMEOUT,          /*!< No answer in time */
    ECAT_PORT_STS_TOO_LONG,         /*!< The value does not fit the buffer */
    ECAT_PORT_STS_STATE_REFUSED,    /*!< The slave did not reach the state asked for */
    ECAT_PORT_STS_FAILED            /*!< Anything else the master reported */
} ecat_port_sts_t;

/*! One network adapter the master could open. */
typedef struct
{
    char name[ECAT_PORT_NAME_LEN];  /*!< What to open it by */
    char desc[ECAT_PORT_NAME_LEN];  /*!< What to call it on screen */
} ECAT_PORT_ADAPTER_T;

/*! One slave, as the last scan and state read found it. */
typedef struct
{
    uint16_t position;                      /*!< 1 for the first slave on the ring */
    char     name[ECAT_PORT_SLAVE_NAME_LEN];/*!< Its name from the SII */
    uint32_t vendor;                        /*!< Vendor ID from the SII */
    uint32_t product;                       /*!< Product code from the SII */
    uint32_t revision;                      /*!< Revision number from the SII */
    uint16_t state;                         /*!< ECAT_PORT_STATE_*, with ECAT_PORT_STATE_ERROR when set */
    uint16_t al_status;                     /*!< AL status code: why it refused a change; 0 when it did not */
    bool     mailbox;                       /*!< Whether it has a mailbox: CoE needs one */
} ECAT_PORT_SLAVE_T;

/*! The master's own state; defined by each implementation. */
typedef struct ecat_port_ctx ECAT_PORT_CTX_T;

/*!@}*/

/*============================================================================*/
/*! \name Required functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  The one master of this process.

    The master's state is far too large for a stack, so each implementation
    keeps the single instance itself and hands it out here.

    \return The master's context, never NULL.
*/
ECAT_PORT_CTX_T* ecat_port_instance( void );

/*----------------------------------------------------------------------------*/
/*!
    \brief  What the master is, for the hello: "SOEM 2.0.1", or the fake's name.

    \return A constant string.
*/
char const* ecat_port_version( void );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Whether the master can work on this machine at all.

    On Windows SOEM sends frames through Npcap, which each user installs; the
    helper starts without it, so that it can say so rather than fail to start.

    \return ECAT_PORT_STS_OK, or ECAT_PORT_STS_NO_DRIVER.
*/
ecat_port_sts_t ecat_port_check( void );

/*----------------------------------------------------------------------------*/
/*!
    \brief  The network adapters there are.

    \param  list    Filled with up to \p max adapters.
    \param  max     How many \p list holds.

    \return How many were filled in.
*/
size_t ecat_port_adapters( ECAT_PORT_ADAPTER_T list[], size_t const max );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Open an adapter, closing the one open before.

    \param  ctx_p   The master.
    \param  name    The adapter's name, as ecat_port_adapters() gave it.

    \return ECAT_PORT_STS_OK, or ECAT_PORT_STS_NO_ADAPTER.
*/
ecat_port_sts_t ecat_port_open( ECAT_PORT_CTX_T* const ctx_p, char const name[] );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Close the adapter, if one is open.

    \param  ctx_p   The master.

    \return None.
*/
void ecat_port_close( ECAT_PORT_CTX_T* const ctx_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Find the slaves on the ring and read what they say of themselves.

    Leaves them in PRE-OP where they get there, so their mailboxes work.

    \param  ctx_p   The master.
    \param  count_p How many were found.

    \return ECAT_PORT_STS_OK, ECAT_PORT_STS_NOT_OPEN or ECAT_PORT_STS_NO_SLAVES.
*/
ecat_port_sts_t ecat_port_scan( ECAT_PORT_CTX_T* const ctx_p, size_t* const count_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Read every slave's state and AL status code again.

    \param  ctx_p   The master.

    \return ECAT_PORT_STS_OK or ECAT_PORT_STS_NOT_OPEN.
*/
ecat_port_sts_t ecat_port_read_states( ECAT_PORT_CTX_T* const ctx_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  One slave, as the last scan and state read left it.

    \param  ctx_p       The master.
    \param  position    1 for the first slave.
    \param  slave_p     Filled in.

    \return ECAT_PORT_STS_OK or ECAT_PORT_STS_NO_SLAVE.
*/
ecat_port_sts_t ecat_port_slave( ECAT_PORT_CTX_T* const ctx_p,
                                 uint16_t const position,
                                 ECAT_PORT_SLAVE_T* const slave_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Ask one slave, or all of them, for a state, and wait for it.

    \param  ctx_p       The master.
    \param  position    1 for the first slave; 0 for all of them.
    \param  state       ECAT_PORT_STATE_INIT, _PREOP, _BOOT, _SAFEOP or _OP.
    \param  timeout_us  How long to wait for it.

    \return ECAT_PORT_STS_OK, or ECAT_PORT_STS_STATE_REFUSED with the slave's
            AL status code to read with ecat_port_slave().
*/
ecat_port_sts_t ecat_port_set_state( ECAT_PORT_CTX_T* const ctx_p,
                                     uint16_t const position,
                                     uint16_t const state,
                                     uint32_t const timeout_us );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Read an object by SDO upload.

    \param  ctx_p       The master.
    \param  position    1 for the first slave.
    \param  index       The object's index.
    \param  sub         Its sub-index.
    \param  data        Filled with the value as the slave sent it.
    \param  len_p       In: how many \p data holds. Out: how many were read.
    \param  abort_p     The SDO abort code, with ECAT_PORT_STS_SDO_ABORT.

    \return ECAT_PORT_STS_OK, or why not.
*/
ecat_port_sts_t ecat_port_sdo_read( ECAT_PORT_CTX_T* const ctx_p,
                                    uint16_t const position,
                                    uint16_t const index,
                                    uint8_t const sub,
                                    uint8_t data[],
                                    size_t* const len_p,
                                    uint32_t* const abort_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Write an object by SDO download.

    \param  ctx_p       The master.
    \param  position    1 for the first slave.
    \param  index       The object's index.
    \param  sub         Its sub-index.
    \param  data        The value, as the slave takes it.
    \param  len         How many bytes of \p data.
    \param  abort_p     The SDO abort code, with ECAT_PORT_STS_SDO_ABORT.

    \return ECAT_PORT_STS_OK, or why not.
*/
ecat_port_sts_t ecat_port_sdo_write( ECAT_PORT_CTX_T* const ctx_p,
                                     uint16_t const position,
                                     uint16_t const index,
                                     uint8_t const sub,
                                     uint8_t const data[],
                                     size_t const len,
                                     uint32_t* const abort_p );

/*!@}*/

#endif /* #ifndef ECAT_PORT_H */

/*============================================================================*/

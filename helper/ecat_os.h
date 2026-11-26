/*!
    \file   ecat_os.h
    \brief  What the helper needs from the operating system: a thread, a lock, a steady clock.

    Windows and POSIX differ in all three; nothing else in the helper should
    have to know which it is built for.

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

#ifndef ECAT_OS_H
#define ECAT_OS_H

#include <stdbool.h>
#include <stdint.h>

#if defined( _WIN32 )
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif /* #ifndef WIN32_LEAN_AND_MEAN */
#include <winsock2.h>   /* before windows.h, which would bring the old winsock.h */
#include <windows.h>
#else /* #if defined( _WIN32 ) */
#include <pthread.h>
#endif /* #if defined( _WIN32 ) */

/*============================================================================*/
/*! \name Types */
/*============================================================================*/
/*!@{*/

/*! What a thread runs. */
typedef void ( *ecat_os_thread_fn_t )( void* const arg_p );

/*! A thread that is joined when it is done with. */
typedef struct
{
#if defined( _WIN32 )
    HANDLE              handle;     /*!< The thread */
#else /* #if defined( _WIN32 ) */
    pthread_t           handle;     /*!< The thread */
#endif /* #if defined( _WIN32 ) */
    ecat_os_thread_fn_t fn;         /*!< What it runs */
    void*               arg_p;      /*!< What it runs with */
    bool                started;    /*!< Whether it is running, or ran and is not joined */
} ECAT_OS_THREAD_T;

/*! A lock between threads. */
typedef struct
{
#if defined( _WIN32 )
    CRITICAL_SECTION section;       /*!< The lock */
#else /* #if defined( _WIN32 ) */
    pthread_mutex_t  mutex;         /*!< The lock */
#endif /* #if defined( _WIN32 ) */
} ECAT_OS_MUTEX_T;

/*! What a thread waits with, to wake at a time to the microsecond or near it. */
typedef struct
{
#if defined( _WIN32 )
    HANDLE timer;                   /*!< A high-resolution waitable timer, or NULL */
    bool   coarse;                  /*!< No such timer: Sleep() with the timer period at 1 ms */
#else /* #if defined( _WIN32 ) */
    int    unused;                  /*!< POSIX sleeps to an absolute time with nothing kept */
#endif /* #if defined( _WIN32 ) */
} ECAT_OS_TIMER_T;

/*!@}*/

/*============================================================================*/
/*! \name Public functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Start a thread.

    \param  thread_p        The thread.
    \param  fn              What it runs.
    \param  arg_p           What it runs with.
    \param  high_priority   Ask for the highest priority the process may have;
                            where it may not (Linux without the right), it runs
                            at the normal one.

    \return true when it is running.
*/
bool ecat_os_thread_start( ECAT_OS_THREAD_T* const thread_p,
                           ecat_os_thread_fn_t const fn,
                           void* const arg_p,
                           bool const high_priority );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Wait for a thread to end, if it was started.

    \param  thread_p    The thread.

    \return None.
*/
void ecat_os_thread_join( ECAT_OS_THREAD_T* const thread_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Make a lock ready, unlocked.

    \param  mutex_p The lock.

    \return None.
*/
void ecat_os_mutex_init( ECAT_OS_MUTEX_T* const mutex_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Take a lock, waiting for it.

    \param  mutex_p The lock.

    \return None.
*/
void ecat_os_mutex_lock( ECAT_OS_MUTEX_T* const mutex_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Give a lock back.

    \param  mutex_p The lock.

    \return None.
*/
void ecat_os_mutex_unlock( ECAT_OS_MUTEX_T* const mutex_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  A clock that only goes forward, in microseconds from no set time.

    \return Now.
*/
uint64_t ecat_os_now_us( void );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Make ready what a thread waits with. Called on the thread that waits.

    \param  timer_p The timer.

    \return None.
*/
void ecat_os_timer_open( ECAT_OS_TIMER_T* const timer_p );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Wait until a time on ecat_os_now_us()'s clock; at once if it has passed.

    \param  timer_p The timer.
    \param  when_us The time.

    \return None.
*/
void ecat_os_sleep_until( ECAT_OS_TIMER_T* const timer_p, uint64_t const when_us );

/*----------------------------------------------------------------------------*/
/*!
    \brief  Let go of what a thread waited with.

    \param  timer_p The timer.

    \return None.
*/
void ecat_os_timer_close( ECAT_OS_TIMER_T* const timer_p );

/*!@}*/

#endif /* #ifndef ECAT_OS_H */

/*============================================================================*/

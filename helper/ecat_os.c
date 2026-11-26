/*!
    \file   ecat_os.c
    \brief  A thread, a lock and a steady clock, on Windows and on POSIX.

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

#if !defined( _WIN32 ) && !defined( _POSIX_C_SOURCE )
#define _POSIX_C_SOURCE 200809L     /* clock_nanosleep() and CLOCK_MONOTONIC */
#endif /* #if !defined( _WIN32 ) && !defined( _POSIX_C_SOURCE ) */

#include "ecat_os.h"

#include <stddef.h>

#if defined( _WIN32 )
#include <timeapi.h>
#else /* #if defined( _WIN32 ) */
#include <errno.h>
#include <sched.h>
#include <time.h>
#endif /* #if defined( _WIN32 ) */

/*============================================================================*/
/*! \name Defines */
/*============================================================================*/
/*!@{*/

#if defined( _WIN32 )
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION   ( 0x00000002UL )    /*!< Windows 10 1803 on; older SDKs lack the name */
#endif /* #ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */
#define OS_US_PER_TICK  ( 10 )      /*!< A waitable timer counts in 100 ns ticks */
#endif /* #if defined( _WIN32 ) */

#define OS_US_PER_S     ( 1000000U )    /*!< Microseconds in a second */
#define OS_NS_PER_US    ( 1000U )       /*!< Nanoseconds in a microsecond */

/*!@}*/

/*============================================================================*/
/*! \name Static prototypes */
/*============================================================================*/
/*!@{*/

#if defined( _WIN32 )
static DWORD WINAPI thread_entry( LPVOID const arg_p );
#else /* #if defined( _WIN32 ) */
static void* thread_entry( void* const arg_p );
#endif /* #if defined( _WIN32 ) */

/*!@}*/

/*============================================================================*/
/*! \name Public functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_os.h.
*/
bool ecat_os_thread_start( ECAT_OS_THREAD_T* const thread_p,
                           ecat_os_thread_fn_t const fn,
                           void* const arg_p,
                           bool const high_priority )
{
    thread_p->fn = fn;
    thread_p->arg_p = arg_p;
#if defined( _WIN32 )
    thread_p->handle = CreateThread( NULL, 0U, thread_entry, thread_p, 0U, NULL );
    thread_p->started = ( NULL != thread_p->handle );
    if( thread_p->started && high_priority )
    {
        ( void )SetThreadPriority( thread_p->handle, THREAD_PRIORITY_TIME_CRITICAL );
    }
    else
    {
        /* Not started, or the normal priority will do. */
    }
#else /* #if defined( _WIN32 ) */
    thread_p->started = ( 0 == pthread_create( &thread_p->handle, NULL, thread_entry, thread_p ) );
    if( thread_p->started && high_priority )
    {
        struct sched_param param;

        param.sched_priority = sched_get_priority_max( SCHED_FIFO );
        /* Needs root or CAP_SYS_NICE; without it the thread stays at the normal priority. */
        ( void )pthread_setschedparam( thread_p->handle, SCHED_FIFO, &param );
    }
    else
    {
        /* Not started, or the normal priority will do. */
    }
#endif /* #if defined( _WIN32 ) */
    return thread_p->started;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_os.h.
*/
void ecat_os_thread_join( ECAT_OS_THREAD_T* const thread_p )
{
    if( thread_p->started )
    {
#if defined( _WIN32 )
        ( void )WaitForSingleObject( thread_p->handle, INFINITE );
        ( void )CloseHandle( thread_p->handle );
#else /* #if defined( _WIN32 ) */
        ( void )pthread_join( thread_p->handle, NULL );
#endif /* #if defined( _WIN32 ) */
        thread_p->started = false;
    }
    else
    {
        /* Nothing to wait for. */
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_os.h.
*/
void ecat_os_mutex_init( ECAT_OS_MUTEX_T* const mutex_p )
{
#if defined( _WIN32 )
    InitializeCriticalSection( &mutex_p->section );
#else /* #if defined( _WIN32 ) */
    ( void )pthread_mutex_init( &mutex_p->mutex, NULL );
#endif /* #if defined( _WIN32 ) */
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_os.h.
*/
void ecat_os_mutex_lock( ECAT_OS_MUTEX_T* const mutex_p )
{
#if defined( _WIN32 )
    EnterCriticalSection( &mutex_p->section );
#else /* #if defined( _WIN32 ) */
    ( void )pthread_mutex_lock( &mutex_p->mutex );
#endif /* #if defined( _WIN32 ) */
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_os.h.
*/
void ecat_os_mutex_unlock( ECAT_OS_MUTEX_T* const mutex_p )
{
#if defined( _WIN32 )
    LeaveCriticalSection( &mutex_p->section );
#else /* #if defined( _WIN32 ) */
    ( void )pthread_mutex_unlock( &mutex_p->mutex );
#endif /* #if defined( _WIN32 ) */
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_os.h.
*/
uint64_t ecat_os_now_us( void )
{
#if defined( _WIN32 )
    LARGE_INTEGER count;
    LARGE_INTEGER rate;

    ( void )QueryPerformanceCounter( &count );
    ( void )QueryPerformanceFrequency( &rate );
    /* Seconds and the rest apart, so that the multiply cannot overflow. */
    return ( ( (uint64_t)count.QuadPart / (uint64_t)rate.QuadPart ) * OS_US_PER_S ) +
           ( ( ( (uint64_t)count.QuadPart % (uint64_t)rate.QuadPart ) * OS_US_PER_S ) / (uint64_t)rate.QuadPart );
#else /* #if defined( _WIN32 ) */
    struct timespec now;

    ( void )clock_gettime( CLOCK_MONOTONIC, &now );
    return ( (uint64_t)now.tv_sec * OS_US_PER_S ) + ( (uint64_t)now.tv_nsec / OS_NS_PER_US );
#endif /* #if defined( _WIN32 ) */
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_os.h.
*/
void ecat_os_timer_open( ECAT_OS_TIMER_T* const timer_p )
{
#if defined( _WIN32 )
    timer_p->timer = CreateWaitableTimerExW( NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS );
    timer_p->coarse = ( NULL == timer_p->timer );
    if( timer_p->coarse )
    {
        ( void )timeBeginPeriod( 1U );  /* Sleep() to the millisecond, not to 15.6 ms */
    }
    else
    {
        /* The timer is fine enough itself. */
    }
#else /* #if defined( _WIN32 ) */
    timer_p->unused = 0;
#endif /* #if defined( _WIN32 ) */
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_os.h.
*/
void ecat_os_sleep_until( ECAT_OS_TIMER_T* const timer_p, uint64_t const when_us )
{
    uint64_t const now_us = ecat_os_now_us();

    if( when_us > now_us )
    {
#if defined( _WIN32 )
        uint64_t const wait_us = when_us - now_us;

        if( !timer_p->coarse )
        {
            LARGE_INTEGER due;

            due.QuadPart = -(LONGLONG)( wait_us * OS_US_PER_TICK );     /* negative: from now */
            if( SetWaitableTimer( timer_p->timer, &due, 0, NULL, NULL, FALSE ) )
            {
                ( void )WaitForSingleObject( timer_p->timer, INFINITE );
            }
            else
            {
                Sleep( (DWORD)( wait_us / 1000U ) );
            }
        }
        else
        {
            Sleep( (DWORD)( wait_us / 1000U ) );
        }
#else /* #if defined( _WIN32 ) */
        struct timespec when;
        int sts;

        ( void )timer_p;
        when.tv_sec = (time_t)( when_us / OS_US_PER_S );
        when.tv_nsec = (long)( ( when_us % OS_US_PER_S ) * OS_NS_PER_US );
        do
        {
            sts = clock_nanosleep( CLOCK_MONOTONIC, TIMER_ABSTIME, &when, NULL );
        } while( EINTR == sts );
#endif /* #if defined( _WIN32 ) */
    }
    else
    {
        /* Already due. */
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \note   Interface documented in ecat_os.h.
*/
void ecat_os_timer_close( ECAT_OS_TIMER_T* const timer_p )
{
#if defined( _WIN32 )
    if( timer_p->coarse )
    {
        ( void )timeEndPeriod( 1U );
    }
    else
    {
        ( void )CloseHandle( timer_p->timer );
    }
#else /* #if defined( _WIN32 ) */
    ( void )timer_p;
#endif /* #if defined( _WIN32 ) */
    return;
}

/*!@}*/

/*============================================================================*/
/*! \name Static functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Where a thread starts: runs what ecat_os_thread_start() was given.

    \param  arg_p   The thread's ECAT_OS_THREAD_T.

    \return 0, or NULL: nothing is returned through it.
*/
#if defined( _WIN32 )
static DWORD WINAPI thread_entry( LPVOID const arg_p )
{
    ECAT_OS_THREAD_T const* const thread_p = (ECAT_OS_THREAD_T const*)arg_p;

    thread_p->fn( thread_p->arg_p );
    return 0U;
}
#else /* #if defined( _WIN32 ) */
static void* thread_entry( void* const arg_p )
{
    ECAT_OS_THREAD_T const* const thread_p = (ECAT_OS_THREAD_T const*)arg_p;

    thread_p->fn( thread_p->arg_p );
    return NULL;
}
#endif /* #if defined( _WIN32 ) */

/*!@}*/

/*============================================================================*/

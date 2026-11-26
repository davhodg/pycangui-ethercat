/*!
    \file   ecat_main.c
    \brief  The helper program: one local connection, one line at a time.

    Listens on 127.0.0.1 only, on a port the system picks, and prints that port
    as the first line of its output -- which is how pycangui's app.helper finds
    it. Takes one connection, answers each request line with one answer line,
    and exits when that connection closes, or when its stdin does: pycangui
    stops a helper by closing its stdin, so nothing is left holding the network
    card after pycangui has gone.

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

/*
    MISRA C:2023 Rule 21.6 (no standard I/O): deviation in this file only. The
    helper is a host program whose interface to pycangui is its standard output
    (the port) and standard input (closed to stop it).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined( _WIN32 )
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
typedef int sock_io_t;                              /*!< What recv() and send() return */
#define SOCK_BAD            INVALID_SOCKET
#define SOCK_CLOSE( s )     closesocket( s )
#define SOCK_SEND_FLAGS     ( 0 )
#define SOCK_IO_LEN( n )    ( (int)( n ) )          /*!< A length as recv() and send() take it */
#else /* #if defined( _WIN32 ) */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
typedef ssize_t sock_io_t;                          /*!< What recv() and send() return */
#define SOCK_BAD            ( -1 )
#define SOCK_CLOSE( s )     close( s )
#define SOCK_SEND_FLAGS     ( MSG_NOSIGNAL )
#define SOCK_IO_LEN( n )    ( n )                   /*!< A length as recv() and send() take it */
#endif /* #if defined( _WIN32 ) */

#include "ecat_helper.h"
#include "ecat_json.h"
#include "ecat_port.h"

/*============================================================================*/
/*! \name Defines */
/*============================================================================*/
/*!@{*/

#define MAIN_LINE_LEN   ( ECAT_JSON_TEXT_LEN + 1024U )  /*!< Longest request line: a value at its longest, and the rest */
#define MAIN_RECV_LEN   ( 4096U )                       /*!< Bytes read from the socket at a time */

/*!@}*/

/*============================================================================*/
/*! \name Types */
/*============================================================================*/
/*!@{*/

/*! The connection's state. */
typedef struct
{
    sock_t            sock;                 /*!< The connection */
    char              line[MAIN_LINE_LEN];  /*!< The request line so far */
    size_t            len;                  /*!< How much of line is used */
    bool              discarding;           /*!< A line was too long: drop the rest of it */
    ECAT_HELPER_CTX_T helper;               /*!< The protocol */
    ECAT_JSON_OUT_T   out;                  /*!< The answer being sent */
} MAIN_CONN_T;

/*!@}*/

/*============================================================================*/
/*! \name Static prototypes */
/*============================================================================*/
/*!@{*/

static bool net_start( void );
static sock_t listen_local( uint16_t* const port_p );
static bool watch_stdin( void );
static void serve( MAIN_CONN_T* const conn_p );
static bool take_bytes( MAIN_CONN_T* const conn_p, char const bytes[], size_t const count );
static bool send_answer( MAIN_CONN_T* const conn_p );

/*!@}*/

/*============================================================================*/
/*! \name Public functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  The helper: print the port, serve one connection, exit.

    \return 0 when the connection closed, 1 when the helper could not start.
*/
int main( void )
{
    /*
        MISRA C:2023 Dir 4.12 / the coding standard's "no file-scope mutable
        state": deviation. The connection holds a request, an answer and a line
        buffer, about 110 KB: static rather than on main's stack, one per process.
    */
    static MAIN_CONN_T conn;
    int code = 1;
    uint16_t port = 0U;

    if( net_start() )
    {
        sock_t const listener = listen_local( &port );

        if( SOCK_BAD != listener )
        {
            if( watch_stdin() )
            {
                ( void )printf( "%u\n", (unsigned int)port );
                ( void )fflush( stdout );

                conn.sock = accept( listener, NULL, NULL );
                ( void )SOCK_CLOSE( listener );     /* one connection only */
                if( SOCK_BAD != conn.sock )
                {
                    ecat_helper_init( &conn.helper, ecat_port_instance() );
                    serve( &conn );
                    ( void )SOCK_CLOSE( conn.sock );
                    code = 0;
                }
                else
                {
                    ( void )fprintf( stderr, "could not accept a connection\n" );
                }
            }
            else
            {
                ( void )SOCK_CLOSE( listener );
                ( void )fprintf( stderr, "could not watch stdin\n" );
            }
        }
        else
        {
            ( void )fprintf( stderr, "could not listen on 127.0.0.1\n" );
        }
    }
    else
    {
        ( void )fprintf( stderr, "could not start networking\n" );
    }

    ecat_port_close( ecat_port_instance() );
    return code;
}

/*!@}*/

/*============================================================================*/
/*! \name Static functions */
/*============================================================================*/
/*!@{*/

/*----------------------------------------------------------------------------*/
/*!
    \brief  Start the socket library, where there is one to start.

    \return true when sockets can be used.
*/
static bool net_start( void )
{
#if defined( _WIN32 )
    WSADATA wsa;

    return ( 0 == WSAStartup( MAKEWORD( 2, 2 ), &wsa ) );
#else /* #if defined( _WIN32 ) */
    return true;
#endif /* #if defined( _WIN32 ) */
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Listen on 127.0.0.1, on a port the system picks.

    \param  port_p  The port picked.

    \return The listening socket, or SOCK_BAD.
*/
static sock_t listen_local( uint16_t* const port_p )
{
    sock_t sock = socket( AF_INET, SOCK_STREAM, 0 );
    struct sockaddr_in addr;

    ( void )memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    addr.sin_port = 0U;

    if( SOCK_BAD == sock )
    {
        /* Nothing to listen with. */
    }
    else if( ( 0 != bind( sock, (struct sockaddr const*)&addr, sizeof( addr ) ) ) || ( 0 != listen( sock, 1 ) ) )
    {
        ( void )SOCK_CLOSE( sock );
        sock = SOCK_BAD;
    }
    else
    {
        socklen_t addr_len = (socklen_t)sizeof( addr );

        if( 0 == getsockname( sock, (struct sockaddr*)&addr, &addr_len ) )
        {
            *port_p = ntohs( addr.sin_port );
        }
        else
        {
            ( void )SOCK_CLOSE( sock );
            sock = SOCK_BAD;
        }
    }
    return sock;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Read stdin until it closes, then end the process.

    \param  unused  Thread argument, not used.

    \return Never returns.
*/
#if defined( _WIN32 )
static DWORD WINAPI stdin_thread( LPVOID const unused )
#else /* #if defined( _WIN32 ) */
static void* stdin_thread( void* const unused )
#endif /* #if defined( _WIN32 ) */
{
    ( void )unused;
#if defined( _WIN32 )
    while( EOF != getchar() )
    {
        /* Nothing is said on stdin: only its closing matters. */
    }
#else /* #if defined( _WIN32 ) */
    {
        /* read(), not getchar(): getchar() holds stdin's lock while it waits,
           and exit() -- from main, when the connection closes -- flushes every
           stream and so waits for that lock for ever (glibc). */
        char byte;

        while( read( STDIN_FILENO, &byte, 1U ) > 0 )
        {
            /* Nothing is said on stdin: only its closing matters. */
        }
    }
#endif /* #if defined( _WIN32 ) */
    /* pycangui has closed it: stop now, whatever the connection is doing. The
       system releases the network card with the process. */
    exit( 0 );
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Start the thread that ends the helper when stdin closes.

    \return true when it is running.
*/
static bool watch_stdin( void )
{
#if defined( _WIN32 )
    HANDLE const thread = CreateThread( NULL, 0U, stdin_thread, NULL, 0U, NULL );
    bool const started = ( NULL != thread );

    if( started )
    {
        ( void )CloseHandle( thread );
    }
    else
    {
        /* Not started. */
    }
    return started;
#else /* #if defined( _WIN32 ) */
    pthread_t thread;
    bool const started = ( 0 == pthread_create( &thread, NULL, stdin_thread, NULL ) );

    if( started )
    {
        ( void )pthread_detach( thread );
    }
    else
    {
        /* Not started. */
    }
    return started;
#endif /* #if defined( _WIN32 ) */
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Answer request lines until the connection closes.

    \param  conn_p  The connection.

    \return None.
*/
static void serve( MAIN_CONN_T* const conn_p )
{
    char bytes[MAIN_RECV_LEN];
    bool going = true;

    conn_p->len = 0U;
    conn_p->discarding = false;
    while( going )
    {
        sock_io_t const got = recv( conn_p->sock, bytes, SOCK_IO_LEN( sizeof( bytes ) ), 0 );

        if( got <= 0 )
        {
            going = false;      /* closed, or broken */
        }
        else
        {
            going = take_bytes( conn_p, bytes, (size_t)got );
        }
    }
    return;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Add bytes to the line, answering each line they complete.

    \param  conn_p  The connection.
    \param  bytes   What came.
    \param  count   How many.

    \return false when an answer could not be sent: the connection has gone.
*/
static bool take_bytes( MAIN_CONN_T* const conn_p, char const bytes[], size_t const count )
{
    bool ok = true;

    for( size_t idx = 0U; ok && ( idx < count ); idx++ )
    {
        char const c = bytes[idx];

        if( '\n' == c )
        {
            if( !conn_p->discarding )
            {
                ecat_helper_handle( &conn_p->helper, conn_p->line, conn_p->len, &conn_p->out );
                ok = send_answer( conn_p );
            }
            else
            {
                conn_p->discarding = false;     /* the end of the line that was too long */
            }
            conn_p->len = 0U;
        }
        else if( conn_p->discarding )
        {
            /* Still the line that was too long. */
        }
        else if( MAIN_LINE_LEN == conn_p->len )
        {
            ecat_helper_too_long( &conn_p->out );
            ok = send_answer( conn_p );
            conn_p->discarding = true;
            conn_p->len = 0U;
        }
        else
        {
            conn_p->line[conn_p->len] = c;
            conn_p->len++;
        }
    }
    return ok;
}

/*----------------------------------------------------------------------------*/
/*!
    \brief  Send the answer written, and its newline.

    \param  conn_p  The connection.

    \return false when it could not all be sent.
*/
static bool send_answer( MAIN_CONN_T* const conn_p )
{
    bool ok = true;
    size_t sent = 0U;

    /* The answer always leaves room for its terminator, which the newline replaces. */
    conn_p->out.buf[conn_p->out.len] = '\n';
    while( ok && ( sent < ( conn_p->out.len + 1U ) ) )
    {
        sock_io_t const done = send( conn_p->sock, &conn_p->out.buf[sent],
                                     SOCK_IO_LEN( ( conn_p->out.len + 1U ) - sent ), SOCK_SEND_FLAGS );

        if( done <= 0 )
        {
            ok = false;
        }
        else
        {
            sent += (size_t)done;
        }
    }
    return ok;
}

/*!@}*/

/*============================================================================*/

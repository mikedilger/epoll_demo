/**
 * THIS SOFTWARE AND DATA ARE COPYRIGHTED
 * Copyright © Optimal Computing Limited New Zealand, 2012.
 * Please see the file LICENSE for terms and conditions and restrictions.
 *
 * This is a generic linux network server, destined to be a graph database
 * server xframedb
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <signal.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>

#define handle_error_en( en, msg ) \
  do { errno = en; perror ( msg ) ; exit ( EXIT_FAILURE ); } while ( 0 )
#define handle_error( msg ) \
  do { perror ( msg ); exit ( EXIT_FAILURE ); } while ( 0 )
#define return_error_en( en,msg ) \
  do { errno = en; perror ( msg ); return -1; } while ( 0 )
#define return_error( msg ) \
  do { perror ( msg ); return -1; } while ( 0 )

// Maximum number of events to be returned from the epoll at
// once. I've seen values from 10 to 64. You usually get 1.
// But if the system hiccups and a bunch of network activity
// queues, you might get a bunch at once.
#define MAX_EVENTS 32

// Global variables
static int listenfd;
static int epollfd;
static int timerfd;
static int sigfd;
static struct epoll_event event, *events;
static long long unsigned int timercount = 0;

static int
create_and_bind ( char *port )
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int s, sfd;

  // Get an internet address on ( this host ) port, with these hints
  // Clear hints with zeroes ( as required by getaddrinfo ( ) )

  memset ( &hints, 0, sizeof ( struct addrinfo ) );
  hints.ai_family = AF_UNSPEC;     /* Return IPv4 and IPv6 choices */
  hints.ai_socktype = SOCK_STREAM; /* We want a TCP socket */
  hints.ai_flags = AI_PASSIVE;     /* Suitable to bind a server onto */

  s = getaddrinfo ( NULL, port, &hints, &result );
  if ( s != 0 )
  {
    fprintf ( stderr, "getaddrinfo: %s\n", gai_strerror ( s ) );
    return -1;
  }

  // For each addrinfo returned, try to open a socket and bind to the
  // associated address; keep going until it works
  for ( rp = result; rp != NULL; rp = rp->ai_next )
  {
    sfd = socket ( rp->ai_family, rp->ai_socktype, rp->ai_protocol );
    if ( sfd == -1 )
      continue;

    s = bind ( sfd, rp->ai_addr, rp->ai_addrlen );
    if ( s == 0 )
    {
      /* We managed to bind successfully! */
      break;
    }

    close ( sfd );
  }

  // Fail if nothing worked
  if ( rp == NULL )
    return_error ( "Could not bind\n" );

  // Clean up
  freeaddrinfo ( result );

  // And return our server file descriptor
  return sfd;
}

static int
make_socket_non_blocking ( int sfd )
{
  int flags, s;

  // Get the current flags on this socket
  flags = fcntl ( sfd, F_GETFL, 0 );
  if ( flags == -1 )
    return_error ( "fcntl" );

  // Add the non-block flag
  flags |= O_NONBLOCK;
  s = fcntl ( sfd, F_SETFL, flags );
  if ( s == -1 )
    return_error ( "fcntl" );

  return 0;
}

static int
accept_new_client ( int sfd )
{
  struct sockaddr in_addr;
  socklen_t in_len;
  int infd, s;
  char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

  // Grab an incoming connection socket and its address
  in_len = sizeof in_addr;
  infd = accept ( sfd, &in_addr, &in_len );
  if ( infd == -1 )
  {
    // If nothing was waiting
    if ( ( errno == EAGAIN ) || ( errno == EWOULDBLOCK ) )
    {
      // We have processed all incoming connections.
      return -1;
    }
    else
      return_error ( "accept" );
  }

  // Translate that sockets address to host/port
  s = getnameinfo ( &in_addr, in_len,
    hbuf, sizeof hbuf,
    sbuf, sizeof sbuf,
    NI_NUMERICHOST | NI_NUMERICSERV );
  if ( s == 0 )
  {
    printf ( "Accepted connection on descriptor %d "
      " ( host=%s, port=%s )\n", infd, hbuf, sbuf );
  }

  /* Make the incoming socket non-blocking (required for
     epoll edge-triggered mode */
  s = make_socket_non_blocking ( infd );
  if ( s == -1 )
    handle_error ( "Could not make socket non-blocking\n" );

  return infd;
}

static int
do_work ( int fd )
{
  /* We have data on the fd waiting to be read. Read and
     display it. We must read whatever data is available
     completely, as we are running in edge-triggered mode
     and won't get a notification again for the same data. */

  int s, done = 0;

  while ( 1 )
  {
    ssize_t count;
    char buf[512];

    count = read ( fd, buf, sizeof buf );
    if ( count == -1 )
    {
      /* If errno == EAGAIN, that means we have read all
         data. So go back to the main loop. */
      if ( errno != EAGAIN )
      {
        perror ( "read" );
        done = 1;
      }
      break;
    }
    else if ( count == 0 )
    {
      /* End of file. The remote has closed the connection. */
      done = 1;
      break;
    }

    /* Write the buffer to standard output */
    s = write ( 1, buf, count );
    if ( s == -1 )
      handle_error ( "write" );
  }

  if ( done )
  {
    printf ( "Closed connection on descriptor %d\n",fd );

    /* Closing the descriptor will make epoll remove it
       from the set of descriptors which are monitored. */
    close ( fd );
  }
}

int
main ( int argc, char *argv[] )
{
  int s;

  if ( argc != 2 )
  {
    fprintf ( stderr, "Usage: %s [port]\n", argv[0] );
    exit ( EXIT_FAILURE );
  }

  // Setup a server socket
  listenfd = create_and_bind ( argv[1] );
  if ( listenfd == -1 )
    handle_error ( "Could not create and bind listener socket.\n" );

  // Make server socket non-blocking
  s = make_socket_non_blocking ( listenfd );
  if ( s == -1 )
    handle_error ( "Could not make listener socket non-blocking.\n" );

  // Mark server socket as a listener, with maximum backlog queue
  s = listen ( listenfd, SOMAXCONN );
  if ( s == -1 )
    handle_error ( "Could not listen.\n" );

  // Setup an epoll instance
  epollfd = epoll_create1 ( 0 );
  if ( epollfd == -1 )
    handle_error ( "Could not create an epoll.\n");

  events = malloc ( MAX_EVENTS * sizeof ( struct epoll_event ) );

  // Mock up the event structure with our socket and
  // mark it for read ( EPOLLIN ) and edge triggered ( EPOLLET )
  event.data.fd = listenfd;
  event.events = EPOLLIN | EPOLLET;
  // And configure the epoll instance for those types of events
  s = epoll_ctl ( epollfd, EPOLL_CTL_ADD, listenfd, &event );
  if ( s == -1 )
    handle_error ( "Could not add file descriptor to epoll.\n" );

  // Create a timer that times out every 1 seconds.

  struct itimerspec tspec;
  tspec.it_value.tv_sec = 1;
  tspec.it_value.tv_nsec = 0;
  tspec.it_interval.tv_sec = 1;
  tspec.it_interval.tv_nsec = 0;

  timerfd = timerfd_create ( CLOCK_MONOTONIC, TFD_NONBLOCK );
  s = timerfd_settime ( timerfd, 0, &tspec, NULL );

  /* Start watching for events to read (EPOLLIN) on this timer
     socket in edge triggered mode (EPOLLET) on the same
     epoll instance we are already using */
  event.data.fd = timerfd;
  event.events = EPOLLIN | EPOLLET;
  s = epoll_ctl ( epollfd, EPOLL_CTL_ADD, timerfd, &event );
  if ( s == -1 )
    handle_error ( "Could not add file descriptor to epoll.\n" );

  // Adjust signal handling
  sigset_t mask;

  sigemptyset(&mask);
  sigaddset(&mask, SIGHUP);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGQUIT);

  /* Block signals so they aren't handled according to their
     default dispositions */
  if ( sigprocmask ( SIG_BLOCK, &mask, NULL ) == -1 )
    handle_error ( "Could not mask signals.\n" );

  /* Setup a signal fd, to watch signals with epoll */
  sigfd = signalfd ( -1, &mask, 0 );
  if ( sigfd == -1 )
    handle_error ( "Could not create signalfd.\n" );

  /* Start watching for events to read (EPOLLIN) on this signalfd
     socket in edge triggered mode (EPOLLET) on the same
     epoll instance we are already using */
  event.data.fd = sigfd;
  event.events = EPOLLIN | EPOLLET;
  s = epoll_ctl ( epollfd, EPOLL_CTL_ADD, sigfd, &event );
  if ( s == -1 )
    handle_error ( "Could not add file descriptor to epoll.\n" );

  /* The event loop */
  while ( 1 )
  {
    int n, i;

    // Wait for an event. -1 = wait forever
    // ( 0 would mean return immediately, otherwise value is in ms )
    n = epoll_wait ( epollfd, events, MAX_EVENTS, -1 );

    // For each event that the epoll instance just gave us
    for ( i = 0; i < n; i++ )
    {

      // If error, hangup, or NOT ready for read
      if ( ( events[i].events & EPOLLERR ) ||
        ( events[i].events & EPOLLHUP ) ||
        ( ! ( events[i].events & EPOLLIN ) ) )
      {
        /* An error has occured on this fd, or the socket is not
           ready for reading ( why were we notified then? ) */
        fprintf ( stderr, "epoll error\n" );
        close ( events[i].data.fd );
        continue;
      }

      // If the event is on our listening socket
      else if ( listenfd == events[i].data.fd )
      {
        /* We have a notification on the listening socket, which
           means one (or more) incoming connections. */
        while ( 1 )
        {
          int infd;

          infd = accept_new_client ( listenfd );

          if ( infd == -1 )
          {
            break;
          }

          /* Start watching for events to read ( EPOLLIN ) on this
             socket in edge triggered mode ( EPOLLET ) on the same
             epoll instance we are already using */
          event.data.fd = infd;
          event.events = EPOLLIN | EPOLLET;
          s = epoll_ctl ( epollfd, EPOLL_CTL_ADD, infd, &event );
          if ( s == -1 )
            handle_error ( "Could not add file descriptor to epoll.\n" );
        }
        continue; // next event please
      }

      // If the event is on our signal socket
      else if ( sigfd == events[i].data.fd )
      {
        ssize_t sz;
        struct signalfd_siginfo fdsi;

        sz = read(sigfd, &fdsi, sizeof(struct signalfd_siginfo));
        if (sz != sizeof(struct signalfd_siginfo))
          handle_error ( "read signal of wrong size" );

        switch (fdsi.ssi_signo) {
          case SIGHUP:
            fprintf ( stderr, "\nCaught SIGHUP.  Exiting.\n" );
            break;
          case SIGINT:
            fprintf ( stderr, "\nCaught SIGINT.  Exiting.\n" );
            break;
          case SIGQUIT:
            fprintf ( stderr, "\nCaught SIGQUIT.  Exiting.\n" );
            break;
        }

        close ( listenfd );
        close ( epollfd );

        exit ( EXIT_SUCCESS );
      }

      // If the event is on our timer socket
      else if ( timerfd == events[i].data.fd )
      {
        // Read 8-byte integer count of expirations
        while ( 1 )
        {
          ssize_t readcount;
          long long unsigned int expire_count;

          readcount = read ( timerfd, (void*)&expire_count, sizeof ( long long unsigned int ) );
          if ( readcount == -1 )
          {
            /* If errno == EAGAIN, that means we have read all
               data. So go back to the main loop. */
            if ( errno != EAGAIN )
            {
              perror ( "read" );
            }
            break;
          }
          else if ( readcount == 0 )
          {
            /* End of file. The remote has closed the connection. */
            break;
          }

          timercount += expire_count;

          /* Write the expiry count to standard output */
          printf ( "Timer: %llu\n",timercount );
        }

        continue; // next event please
      }

      else // The event was on a client socket
      {
        /* We have data on the fd waiting to be read. Take action.
           We must read whatever data is available completely,
           as we are running in edge-triggered mode and we won't
           get a notification again for the same data. */
        do_work ( events[i].data.fd );
      }
    }
  }

  // The event loop above doesn't have a provision to actually exit.
  return EXIT_FAILURE;
}

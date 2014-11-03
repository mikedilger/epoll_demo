epoll_demo
==========

This is an example demonstration of how to use epoll() on linux with maximum
functionality including:
  * Asynchronous I/O
  * edge-triggered mode
  * Handling signals, timers, and file/network I/O with the same syscall
  * Multi-threaded event loop

The code is a stripped-down version of a graph database I never finished,
so the variable and function names may not be the best choice for a demo,
but it should work.

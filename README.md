# C Timeout Server
A simple non-blocking TCP server demonstrating a POSIX timer-based client timeout system.
## Functionality
Connections are managed via two parallel arrays, `struct pollfd *pfds` and `timer_t *timers`.
The `i`th index of each array will represent one connection; `pfds[i]` stores the connection's file descriptor and I/O event flags for polling, and `timers[i]` is a timer object managing the communication timeout.
The following happens:
1. A POSIX timer is created for each connection "slot".
2. Upon accepting a connection request and initialising space on the parallel arrays, the server will arm the timer with the set timeout value.
3. Every time `poll()` detects an input event on the socket, the server will reset the timer back to its original value before processing the network data.
4. If the timer does not get reset within the timeout period (i.e., the client has not sent data in a while), the timer will raise a SIGUSR1 signal.
5. The SIGUSR1 handler sets a flag which notifies the server's event loop to check all connections for expired timers.
6. Upon finding the expired timer, the server will sever the connection, disarm the timer, and resume standard operation.
## Design
The obvious solution is using libevent, a powerful library specifically designed for non-blocking event-driven I/O with support for timeouts.
This program offers a dependency-free alternative (however should not be used in production, it is merely a demonstration).
### Constraints
*TL;DR, the program should compile and run on all modern UNIX and Unix-like operating systems.*
- It must conform to the C99 standard (ISO/IEC 9899:1999).
- POSIX extensions are permitted.
- Other third-party libraries are not allowed.
### Other solutions
A variety of other solutions were considered, however none work with non-blocking I/O in a polling environment.
If each client connection had its own thread, the second and third solutions would be viable.
| Solution       | Description | Event on idle | Drawbacks |
| -------------: | :---------- | :------------ | :-------- |
| `SO_KEEPALIVE` | A socket setting to enable protocol-specific keep-alive messages. | A **SIGPIPE** is raised. | Does not help when a connection is still established but the client is not sending messages. |
| `SO_RCVTIMEO`<br />`SO_SNDTIMEO` | Timeout for message sending and receiving. | Socket I/O system calls complete with a partial count or (if not data is transferred) the calls fail and set *errno* to **EAGAIN** or **EWOULDBLOCK**. | Only works with blocking I/O. |
| `select()` and `poll()` timeouts | Timeout value when polling sockets for I/O events. | Returns 0. | All clients are held to the same timeout. If just one client creates an I/O event, it resets the timer for all. |
## Compiling
Configuration variables such as the server's address and listening port, timeout value, and buffer sizes can be found at the top of client.c and server.c.

With `gcc`, the server is compiled as follows:
```sh
gcc -o server server.c -lrt
```
The client application is compiled with:
```sh
gcc -o client client.c
```
## Running
For simplicity, both programs support no command-line arguments so are executed with just `./server` and `./client`.
Configuration must be made with the aforementioned constants present near the top of the source files.

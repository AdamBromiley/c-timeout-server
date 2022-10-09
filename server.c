#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <poll.h>
#include <unistd.h>


/* Maximum number of clients (including the master socket). Must be > 1. */
static const size_t MAX_CONNECTIONS = 10U;

/* Default listening port. */
static const uint16_t PORT = 1337U;

/* Size of the server's receive buffer, including allocation for a 1-byte null
 * terminator (hence must be > 1).
 */
static const size_t BUFFER_SIZE = 1024U;

/* Default client timeout in seconds. */
static const time_t TIMEOUT = 30;

/* Signal to raise upon a client timeout. */
static const int TIMEOUT_SIGNAL = SIGUSR1;


/* Global flag to indicate that a client has timed out. */
static volatile sig_atomic_t timeout_triggered = 0;

/* Global flag to indicate that an interrupt signal has been delivered. */
static volatile sig_atomic_t interrupt_triggered = 0;


static int create_timers(timer_t *timers, size_t n);
static int destroy_timers(timer_t *timers, size_t n);
static int arm_timer(timer_t timer);
static int disarm_timer(timer_t timer);
static bool timer_expired(timer_t timer);

static int initialise_listening_socket(struct pollfd *pfds);
static int accept_connection(struct pollfd *pfds, timer_t *timers);
static void close_connection(struct pollfd *pfd, timer_t timer);

static int initialise_signal_handler(void (*signal_handler)(int), int signal);
static void interrupt_handler(int signal);
static void timeout_handler(int signal);

static int initialise_server(struct pollfd *pfds, timer_t *timers, size_t n);
static int event_loop(struct pollfd *pfds, timer_t *timers, size_t n);
static int shutdown_server(struct pollfd *pfds, timer_t *timers, size_t n);


static int create_timers(timer_t *timers, size_t n) {
    for (size_t i = 0U; i < n; ++i) {
        struct sigevent event = {
            .sigev_notify = SIGEV_SIGNAL,
            .sigev_signo = TIMEOUT_SIGNAL,
            .sigev_value.sival_ptr = timers[i]
        };

        if (timer_create(CLOCK_REALTIME, &event, &timers[i])) {
            perror("Failed to create timer");
            destroy_timers(timers, i);
            return 1;
        }
    }

    return 0;
}


static int destroy_timers(timer_t *timers, size_t n) {
    for (size_t i = 0U; i < n; ++i) {
        if (timer_delete(timers[i])) {
            perror("Failed to destroy timer");
            return 1;
        }
    }

    return 0;
}


static int arm_timer(timer_t timer) {
    struct itimerspec its = {
        .it_value.tv_sec = TIMEOUT
    };

    if (timer_settime(timer, 0, &its, NULL)) {
        perror("Failed to arm timer");
        return 1;
    }

    return 0;
}


static int disarm_timer(timer_t timer) {
    struct itimerspec its = {0};

    if (timer_settime(timer, 0, &its, NULL)) {
        perror("Failed to disarm timer");
        return 1;
    }

    return 0;
}


static bool timer_expired(timer_t timer) {
    struct itimerspec its;

    if (timer_gettime(timer, &its)) {
        perror("Failed to get state of timer");
        exit(EXIT_FAILURE);
    }

    if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0)
        return true;

    return false;
}


static int initialise_listening_socket(struct pollfd *pfds) {
    const int SOCK_OPT = 1;

    int flags;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(PORT)
    };

    /* Create server's master socket. */
    int s = socket(AF_INET, SOCK_STREAM, 0);

    if (s < 0) {
        perror("Failed to create socket");
        return 1;
    }

    /* Allow rebinding of the listening address and port. */
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const void *) &SOCK_OPT, (socklen_t) sizeof(SOCK_OPT))) {
        perror("Failed to set socket for reuse");
        close(s);
        return 1;
    }

    /* Set the socket's O_NONBLOCK flag so its I/O is nonblocking. */
    flags = fcntl(s, F_GETFL, 0);

    if (flags == -1) {
        perror("Failed to get socket flags");
        close(s);
        return 1;
    }

    if (fcntl(s, F_SETFL, flags | O_NONBLOCK)) {
        perror("Failed to set socket to nonblocking mode");
        close(s);
        return 1;
    }

    /* Bind the socket to its address. */
    if (bind(s, (struct sockaddr *) &addr, (socklen_t) sizeof(addr))) {
        perror("Failed to bind socket");
        close(s);
        return 1;
    }

    /* Set socket to listen. */
    if (listen(s, MAX_CONNECTIONS - 1U)) {
        perror("Failed to set socket to a listening state");
        close(s);
        return 1;
    }

    /* The first element of the pollfd (and timer_t) array will be reserved for
     * the server. Obviously, we will not arm a timer for this socket.
     */
    pfds[0].fd = s;
    pfds[0].events = POLLIN;
    return 0;
}


static int accept_connection(struct pollfd *pfds, timer_t *timers) {
    /* Accept connection request. */
    int s = accept(pfds[0].fd, NULL, NULL);

    if (s < 0) {
        perror("Failed to accept connection request");
        return 1;
    }

    /* Find spare slot for socket (we can skip the master socket at i = 0). */
    for (size_t i = 1U; i < MAX_CONNECTIONS; ++i) {
        struct pollfd *pfd = &pfds[i];

        if (pfd->fd < 0) {
            pfd->fd = s;
            pfd->events = POLLIN;

            /* Arm the client's timeout timer. */
            if (arm_timer(timers[i])) {
                close_connection(pfd, timers[i]);
                return 1;
            }

            fprintf(stderr, "Client %zu connected\n", i);
            return 0;
        }
    }

    fprintf(stderr, "Too many connections already accepted\n");
    close(s);
    return 1;
}


static void close_connection(struct pollfd *pfd, timer_t timer) {
    disarm_timer(timer);
    close(pfd->fd);
    pfd->fd = -1;
}


static int initialise_signal_handler(void (*signal_handler)(int), int signal) {
    struct sigaction action = {
        .sa_handler = signal_handler
    };

    if (sigemptyset(&action.sa_mask)) {
        perror("Failed to initialise the signal mask");
        return 1;
    }

    if (sigaction(signal, &action, NULL)) {
        fprintf(stderr, "Failed to change signal %d action to handler", signal);
        perror(NULL);
        return 1;
    }

    return 0;
}


static void interrupt_handler(int sig) {
    /* Avoid unused parameter warning. */
    (void) sig;
    interrupt_triggered = 1;
}


static void timeout_handler(int sig) {
    /* Avoid unused parameter warning. */
    (void) sig;
    timeout_triggered = 1;
}


static int initialise_server(struct pollfd *pfds, timer_t *timers, size_t n) {
    /* Initialising the socket array (-1 is used in this program to denote an
     * unused connection slot).
     */
    for (size_t i = 0U; i < n; ++i)
        pfds[i].fd = -1;

    fprintf(stderr, "Enabling timeout handler\n");
    if (initialise_signal_handler(timeout_handler, TIMEOUT_SIGNAL))
        return 1;
    
    fprintf(stderr, "Enabling interrupt handler\n");
    if (initialise_signal_handler(interrupt_handler, SIGINT))
        return 1;
    
    fprintf(stderr, "Creating timeout timers\n");
    if (create_timers(timers, n))
        return 1;

    fprintf(stderr, "Initialising listening socket\n");
    if (initialise_listening_socket(pfds)) {
        destroy_timers(timers, n);
        return 1;
    }

    fprintf(stderr, "Server initialised\n");
    return 0;
}


static int shutdown_server(struct pollfd *pfds, timer_t *timers, size_t n) {
    fprintf(stderr, "Closing all client connections\n");
    for (size_t i = 0U; i < MAX_CONNECTIONS; ++i)
        close_connection(&pfds[i], timers[i]);

    fprintf(stderr, "Destroying timeout timers\n");
    destroy_timers(timers, MAX_CONNECTIONS);

    fprintf(stderr, "Server shut down\n");
    return 0;
}


static int event_loop(struct pollfd *pfds, timer_t *timers, size_t n) {
    while (1) {
        int active;

        /* If an interrupt signal (Ctrl-C) is raised. */
        if (interrupt_triggered)
            return 0;

        /* After processing incoming data from each socket, we check to see if
         * any socket has timed out. This is done at the start of the loop so
         * that if poll() raises an EINTR error from the timeout alarm we can
         * check the timers.
         */
        if (timeout_triggered) {
            /* Reset flag at start of check so any timer can go off during the
             * check and just wait till after to get attended to.
             */
            timeout_triggered = 0;

            /* It is impossible to reliably count signals, so we must check
             * every single connection for a timeout.
             */
            for (size_t i = 1U; i < n; ++i) {
                timer_t timer = timers[i];
                struct pollfd *pfd = &pfds[i];

                if (pfd->fd >= 0 && timer_expired(timer)) {
                    fprintf(stderr, "Client %zu timed out\n", i);
                    close_connection(pfd, timer);
                }
            }
        }

        /* Poll sockets for any activity. */
        active = poll(pfds, (nfds_t) n, -1);

        if (active <= 0) {
            /* If poll() was interrupted by a signal (timer alarm or
             * interrupt), we just continue. Other errors can also be handled
             * gracefully
             */
            if (errno == EINTR)
                continue;
            
            perror("Failed to poll sockets");
            return 0;
        }

        /* Iterate over sockets until all active ones have been processed. Make
         * sure to break if the user raises an interrupt signal too.
         */
        for (size_t i = 0U; i < MAX_CONNECTIONS && active > 0 && !interrupt_triggered; ++i) {
            ssize_t ret;
            char buffer[BUFFER_SIZE];

            struct pollfd *pfd = &pfds[i];
            timer_t timer = timers[i];

            /* Skip empty client slots or clients without any I/O events. */
            if (pfd->fd < 0 || !pfd->revents)
                continue;

            /* Decrementing the active socket count allows the loop to
             * terminate early if all active sockets have been processed.
             */
            --active;

            /* 
             * We are only polling for input, so any other event flags set will
             * be relating to error events.
             */
            if (!(pfd->revents & POLLIN)) {
                close_connection(pfd, timer);
                continue;
            }

            /*
             * The 0th index is reserved for the master socket. Any read event
             * here will be for incoming connection requests.
             */
            if (i == 0U) {
                accept_connection(pfds, timers);
                continue;
            }

            /* 
             * Else: there is data to be received from a client. We reset their
             * read timeout.
             */
            if (arm_timer(timer)) {
                close_connection(pfd, timer);
                continue;
            }

            /* Read the client's data. Save the final byte for a null
             * terminator.
             */
            ret = recv(pfd->fd, buffer, BUFFER_SIZE - 1U, 0);

            if (ret == 0) {
                fprintf(stderr, "Client %zu disconnected\n", i);
                close_connection(pfd, timer);
                continue;
            } else if (ret < 0) {
                /* Since we are using signals to handle timeouts, we will
                 * explicitly handle EINTR. Other graceful error handling can
                 * also be made.
                 */
                if (errno == EINTR)
                    continue;

                fprintf(stderr, "Failed read client %zu's data", i);
                perror(NULL);
                return 1;
            }

            /* Ensure null-byte termination of the buffer. */
            buffer[ret] = '\0';
            printf("[Client %zu] %s\n", i, buffer);
        }
    }
}


int main(void) {
    int exit_status = EXIT_SUCCESS;

    struct pollfd pfds[MAX_CONNECTIONS];
    timer_t timers[MAX_CONNECTIONS];
    
    /* Initialise the pollfd array (stores socket numbers and event flags for
     * polling I/O) and timers array (maintains timeout timers for each client
     * connection).
     */
    if (initialise_server(pfds, timers, MAX_CONNECTIONS))
        return EXIT_FAILURE;

    /* Enter the main event loop. */
    exit_status = event_loop(pfds, timers, MAX_CONNECTIONS) ? EXIT_FAILURE : EXIT_SUCCESS;

    /* Close all connections and destroy the timers. */
    shutdown_server(pfds, timers, MAX_CONNECTIONS);
    return exit_status;
}
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <unistd.h>


/* Size of the send buffer. */
static const size_t BUFFER_SIZE = 1024U;

/* Server IPv4 address and listening port. */
static const char *ADDR = "127.0.0.1";
static const uint16_t PORT = 1337U;


/* Global flag to indicate that an interrupt signal has been delivered. */
static volatile sig_atomic_t interrupt_triggered = 0;


static int initialise_signal_handler(void (*signal_handler)(int), int signal);
static void interrupt_handler(int sig);

static int initialise_connection(void);
static int write_socket(int s, const void *buf, size_t n);


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


static int initialise_connection(void) {
    int s;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT)
    };

    s = socket(AF_INET, SOCK_STREAM, 0);

    if (s < 0) {
        perror("Failed to create socket");
        return -1;
    }

    if (inet_pton(AF_INET, ADDR, &addr.sin_addr.s_addr) != 1) {
        perror("Failed to parse address");
        return -1;
    }

    if (connect(s, (struct sockaddr *) &addr, (socklen_t) sizeof(addr))) {
        perror("Failed to connect with server");
        return -1;
    }

    return s;
}


static int write_socket(int s, const void *buf, size_t n) {
    size_t sent = 0;
    
    do {
        ssize_t ret = send(s, (const char *) buf + sent, n - sent, MSG_NOSIGNAL);

        if (ret < 0) {
            if (errno == EINTR)
                continue;
            else if (errno == ECONNRESET || errno == EPIPE)
                fprintf(stderr, "Server disconnect\n");
            else
                fprintf(stderr, "Failed to write to socket\n");

            return 1;
        }

        sent += (size_t) ret;
    } while (sent < n);

    return 0;
}


int main(void) {
    int s;
    int exit_status = EXIT_SUCCESS;

    fprintf(stderr, "Enabling interrupt handler\n");
    if (initialise_signal_handler(interrupt_handler, SIGINT))
        return EXIT_FAILURE;

    fprintf(stderr, "Connecting to server at %s:%" PRIu16 "\n", ADDR, PORT);
    s = initialise_connection();

    if (s < 0)
        return EXIT_FAILURE;

    fprintf(stderr, "Connection initialised\n");

    while (1) {
        char buffer[BUFFER_SIZE];

        /* If an interrupt signal (Ctrl-C) is raised. */
        if (interrupt_triggered)
            break;

        /* Input prompt. */
        fprintf(stderr, "> ");

        if (!fgets(buffer, sizeof(buffer), stdin)) {
            if (ferror(stdin)) {
                if (errno == EINTR)
                    continue;

                perror("Failed to read input");
                exit_status = EXIT_FAILURE;
                break;
            }

            /* End of file. */
            clearerr(stdin);
            continue;
        }

        /* Remove trailing newline, if exists, and skip the write() if of
         * null length.
         */
        buffer[strcspn(buffer, "\n")] = '\0';
        if (strlen(buffer) == 0)
            continue;

        if (write_socket(s, buffer, strlen(buffer))) {
            exit_status = EXIT_FAILURE;
            break;
        }
    }

    fprintf(stderr, "Closing connection\n");
    close(s);
    return exit_status;
}
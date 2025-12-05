#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <netdb.h>

#define SERVER_PORT 9000
#define BACKLOG 10
#define ENVCHAR_PATH "/dev/envchar"

static volatile sig_atomic_t shutdown_requested = 0;

/**********************
 * Shared Server State
 **********************/
typedef enum {
    MODE_AUTO,
    MODE_MANUAL
} server_mode_t;

typedef enum {
    OUTPUT_OFF,
    OUTPUT_ON
} output_state_t;

struct server_state {
    server_mode_t mode;
    output_state_t output;
    pthread_mutex_t lock;
};

static struct server_state g_state = {
    .mode = MODE_AUTO,
    .output = OUTPUT_OFF,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

static void state_lock(void)
{
    pthread_mutex_lock(&g_state.lock);
}

static void state_unlock(void)
{
    pthread_mutex_unlock(&g_state.lock);
}

/**********************
 * Command definitions
 **********************/
typedef enum {
    CMD_GET_MODE,
    CMD_SET_MODE,
    CMD_SET_OUTPUT,
    CMD_GET_LAST,
    CMD_INVALID
} command_type_t;

struct parsed_command {
    command_type_t type;
    long n;                  // for GET LAST N
    server_mode_t mode;      // for SET MODE
    output_state_t output;   // for SET OUTPUT
    char error_msg[128];     // optional error detail
};

static void trim_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' ' || line[len-1] == '\t')) {
        line[len-1] = '\0';
        len--;
    }
}

static void send_response(int fd, const char *msg)
{
    size_t len = strlen(msg);
    if (write(fd, msg, len) < 0) {
        // ignore errors for now
    }
}

// Reads the last N lines from /dev/envchar and sends them to client_fd.
static void send_last_n_records(int client_fd, long n)
{
    FILE *fp = fopen(ENVCHAR_PATH, "r");
    if (!fp) {
        send_response(client_fd, "ERR IO \"Cannot open /dev/envchar\"\n");
        return;
    }

    // Read all lines into a dynamic array
    char **lines = NULL;
    size_t count = 0;
    char *line = NULL;
    size_t len = 0;

    while (getline(&line, &len, fp) != -1) {
        lines = realloc(lines, sizeof(char*) * (count + 1));
        lines[count] = strdup(line);
        count++;
    }

    free(line);
    fclose(fp);

    // If no lines exist
    if (count == 0) {
        send_response(client_fd, "OK\nEND\n");
        free(lines);
        return;
    }

    // Compute start index
    long start = (n >= count) ? 0 : (count - n);

    // Send output
    send_response(client_fd, "OK\n");
    for (long i = start; i < count; i++) {
        send_response(client_fd, lines[i]);
    }
    send_response(client_fd, "END\n");

    // Cleanup
    for (size_t i = 0; i < count; i++) {
        free(lines[i]);
    }
    free(lines);
}

static void parse_command(const char *line, struct parsed_command *cmd)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = CMD_INVALID;

    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok1 = strtok_r(buf, " ", &saveptr);
    if (!tok1) {
        snprintf(cmd->error_msg, sizeof(cmd->error_msg), "Empty command");
        return;
    }

    if (strcmp(tok1, "GET") == 0) {
        char *tok2 = strtok_r(NULL, " ", &saveptr);
        if (!tok2) {
            snprintf(cmd->error_msg, sizeof(cmd->error_msg), "Missing argument after GET");
            return;
        }

        if (strcmp(tok2, "MODE") == 0) {
            cmd->type = CMD_GET_MODE;
            return;
        } else if (strcmp(tok2, "LAST") == 0) {
            char *tok3 = strtok_r(NULL, " ", &saveptr);
            if (!tok3) {
                snprintf(cmd->error_msg, sizeof(cmd->error_msg), "Missing N for GET LAST");
                return;
            }
            char *endp = NULL;
            long n = strtol(tok3, &endp, 10);
            if (*endp != '\0' || n <= 0) {
                snprintf(cmd->error_msg, sizeof(cmd->error_msg), "N must be positive integer");
                return;
            }
            cmd->type = CMD_GET_LAST;
            cmd->n = n;
            return;
        }

        snprintf(cmd->error_msg, sizeof(cmd->error_msg), "Unknown GET command");
        return;
    } else if (strcmp(tok1, "SET") == 0) {
        char *tok2 = strtok_r(NULL, " ", &saveptr);
        if (!tok2) {
            snprintf(cmd->error_msg, sizeof(cmd->error_msg), "Missing argument after SET");
            return;
        }

        if (strcmp(tok2, "MODE") == 0) {
            char *tok3 = strtok_r(NULL, " ", &saveptr);
            if (!tok3) {
                snprintf(cmd->error_msg, sizeof(cmd->error_msg), "Missing mode for SET MODE");
                return;
            }
            if (strcmp(tok3, "AUTO") == 0) {
                cmd->type = CMD_SET_MODE;
                cmd->mode = MODE_AUTO;
                return;
            } else if (strcmp(tok3, "MANUAL") == 0) {
                cmd->type = CMD_SET_MODE;
                cmd->mode = MODE_MANUAL;
                return;
            }
            snprintf(cmd->error_msg, sizeof(cmd->error_msg), "Unknown mode");
            return;
        } else if (strcmp(tok2, "OUTPUT") == 0) {
            char *tok3 = strtok_r(NULL, " ", &saveptr);
            if (!tok3) {
                snprintf(cmd->error_msg, sizeof(cmd->error_msg), "Missing value for SET OUTPUT");
                return;
            }
            if (strcmp(tok3, "ON") == 0) {
                cmd->type = CMD_SET_OUTPUT;
                cmd->output = OUTPUT_ON;
                return;
            } else if (strcmp(tok3, "OFF") == 0) {
                cmd->type = CMD_SET_OUTPUT;
                cmd->output = OUTPUT_OFF;
                return;
            }
            snprintf(cmd->error_msg, sizeof(cmd->error_msg), "Unknown output value");
            return;
        }

        snprintf(cmd->error_msg, sizeof(cmd->error_msg), "Unknown SET command");
        return;
    }

    snprintf(cmd->error_msg, sizeof(cmd->error_msg), "Unknown command");
}

static void handle_command(int client_fd, const struct parsed_command *cmd)
{
    char buf[256];

    switch (cmd->type) {
    case CMD_GET_MODE: {
        state_lock();
        server_mode_t m = g_state.mode;
        state_unlock();
        snprintf(buf, sizeof(buf), "OK MODE %s\n", (m == MODE_AUTO) ? "AUTO" : "MANUAL");
        send_response(client_fd, buf);
        break;
    }
    case CMD_SET_MODE: {
        state_lock();
        g_state.mode = cmd->mode;
        state_unlock();
        snprintf(buf, sizeof(buf), "OK MODE %s\n",
                 (cmd->mode == MODE_AUTO) ? "AUTO" : "MANUAL");
        send_response(client_fd, buf);
        break;
    }
    case CMD_SET_OUTPUT: {
        state_lock();
        if (g_state.mode != MODE_MANUAL) {
            state_unlock();
            send_response(client_fd,
                "ERR INVALID_STATE \"Cannot SET OUTPUT while in AUTO mode\"\n");
            break;
        }
        g_state.output = cmd->output;
        state_unlock();
        snprintf(buf, sizeof(buf), "OK OUTPUT %s\n",
                 (cmd->output == OUTPUT_ON) ? "ON" : "OFF");
        send_response(client_fd, buf);
        break;
    }
    case CMD_GET_LAST: {
        send_last_n_records(client_fd, cmd->n);
        break;
    }
    case CMD_INVALID:
    default:
        if (cmd->error_msg[0] != '\0') {
            snprintf(buf, sizeof(buf), "ERR INVALID_ARGUMENT \"%s\"\n", cmd->error_msg);
        } else {
            snprintf(buf, sizeof(buf), "ERR UNKNOWN_COMMAND\n");
        }
        send_response(client_fd, buf);
        break;
    }
}

/*******************************************************
 * Signal handler
 *******************************************************/
static void signal_handler(int signo)
{
    (void) signo;
    shutdown_requested = 1;
}

/*******************************************************
 * Install signal handlers
 *******************************************************/
static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;

    if (sigaction(SIGINT, &sa, NULL) == -1) return -1;
    if (sigaction(SIGTERM, &sa, NULL) == -1) return -1;

    return 0;
}

/*******************************************************
 * Worker thread
 *******************************************************/
struct worker_args {
    int client_fd;
    struct sockaddr_storage client_addr;
};

static void *worker_thread(void *arg)
{
    struct worker_args *w = arg;

    char host[64];
    getnameinfo((struct sockaddr*)&w->client_addr, sizeof(w->client_addr),
                host, sizeof(host), NULL, 0,
                NI_NUMERICHOST);

    printf("Accepted connection from %s\n", host);

    char buf[256];
    size_t used = 0;

    for (;;) {
        ssize_t rc = read(w->client_fd, buf + used, sizeof(buf) - 1 - used);
        if (rc < 0) {
            perror("read");
            break;
        }
        if (rc == 0) {
            // client closed
            break;
        }
        used += rc;
        buf[used] = '\0';

        // process complete lines
        char *start = buf;
        char *newline;

        while ((newline = strchr(start, '\n')) != NULL) {
            *newline = '\0';
            char line[256];
            strncpy(line, start, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';

            trim_line(line);

            if (line[0] != '\0') {
                struct parsed_command cmd;
                parse_command(line, &cmd);
                handle_command(w->client_fd, &cmd);
            }

            start = newline + 1;
        }

        // move remaining partial line to front
        size_t remaining = buf + used - start;
        memmove(buf, start, remaining);
        used = remaining;
    }

    close(w->client_fd);
    printf("Closed connection from %s\n", host);

    free(w);
    return NULL;
}

/*******************************************************
 * Main server routine
 *******************************************************/
int main(void)
{
    if (install_signal_handlers() == -1) {
        perror("install_signal_handlers");
        return EXIT_FAILURE;
    }

    g_state.mode = MODE_AUTO;
    g_state.output = OUTPUT_OFF;

    int server_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int optval = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in6 server_addr = {0};
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_addr = in6addr_any;
    server_addr.sin6_port = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("aesdsocket server listening on port %d\n", SERVER_PORT);

    while (!shutdown_requested) {
        struct worker_args *w = malloc(sizeof(*w));
        if (!w) continue;

        socklen_t addrlen = sizeof(w->client_addr);
        w->client_fd = accept(server_fd,
                              (struct sockaddr*)&w->client_addr,
                              &addrlen);

        if (w->client_fd < 0) {
            free(w);
            if (errno == EINTR && shutdown_requested)
                break;
            perror("accept");
            continue;
        }

        pthread_t thread;
        pthread_create(&thread, NULL, worker_thread, w);
        pthread_detach(thread);
    }

    printf("Shutting down server...\n");
    close(server_fd);
    return EXIT_SUCCESS;
}

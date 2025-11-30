#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#define ENVCHAR_PATH "/dev/envchar"
#define LOG_INTERVAL_SEC 5

static volatile sig_atomic_t exit_requested = 0;

static void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    if (sigaction(SIGINT, &sa, NULL) == -1) return -1;
    if (sigaction(SIGTERM, &sa, NULL) == -1) return -1;
    return 0;
}

static void format_timestamp(char *buf, size_t buflen)
{
    struct timespec ts;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);

    snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

int main(void)
{
    int fd;
    unsigned long seq = 0;
    int presence = 0; /* simulated presence bit */

    if (install_signal_handlers() == -1) {
        perror("signal");
        return EXIT_FAILURE;
    }

    while (!exit_requested) {
        fd = open(ENVCHAR_PATH, O_WRONLY | O_APPEND);
        if (fd < 0) {
            perror("open /dev/envchar");
            sleep(5);
            continue;
        }

        char tsbuf[32];
        char line[128];

        format_timestamp(tsbuf, sizeof(tsbuf));

        /* For now, just toggle presence to simulate a sensor */
        presence = !presence;
        seq++;

        int len = snprintf(line, sizeof(line),
                           "%s; presence=%d; seq=%lu\n",
                           tsbuf, presence, seq);

        if (write(fd, line, len) < 0) {
            perror("write /dev/envchar");
        }

        close(fd);

        for (int i = 0; i < LOG_INTERVAL_SEC && !exit_requested; i++)
            sleep(1);
    }

    return EXIT_SUCCESS;
}

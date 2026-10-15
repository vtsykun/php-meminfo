/*
 * Send a signal carrying a chosen dump id.
 *
 * The id decides the name of the dump file, so with this the target's file
 * handling can be exercised at a predictable path — which a .phpt cannot do,
 * because meminfo_dump_process() picks the id at random and PHP has no
 * sigqueue() binding.
 *
 * Build and use:
 *   cc -o sigqueue sigqueue.c
 *   ./sigqueue <pid> <signo> <id>
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    union sigval value;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <pid> <signo> <id>\n", argv[0]);
        return 2;
    }

    value.sival_int = atoi(argv[3]);

    if (sigqueue((pid_t) atoi(argv[1]), atoi(argv[2]), value) != 0) {
        perror("sigqueue");
        return 1;
    }

    return 0;
}

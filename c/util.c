#include "util.h"

#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>

bool does_object_exist(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

bool is_DS_Store_path(const char* path)
{
    if (!path)
        return false;

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    return strcmp(base, ".DS_Store") == 0;
}

void wait_for_ctrl_c(void) {
    sigset_t ss = {0};

    sigemptyset(&ss);
    sigaddset(&ss, SIGINT);
    sigaddset(&ss, SIGTERM);
    sigaddset(&ss, SIGQUIT);

    int received_signal = {0};
    sigwait(&ss, &received_signal);
}

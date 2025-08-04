// this length includes a terminating line feed
#define MAX_LINE_LENGTH 120

// in seconds
#define SHUTDOWN_TIMEOUT 10

#define CHILDREN_COUNT 3

const struct child_configuration child_configuration[CHILDREN_COUNT] = {
    {
        .command = { "/bin/sh", "-c", "while true; do sleep 5; echo 'hello'; done", NULL },
        .name = "SLEEPER",
        .receives_sigusr1 = 0,
        .receives_sigusr2 = 0,
        .termination_signal = SIGTERM,
        .is_startup_check = 0
    },
    {
        .command = { "/usr/bin/echo", "check done!", NULL },
        .name = "CHECK",
        .receives_sigusr1 = 0,
        .receives_sigusr2 = 0,
        .termination_signal = SIGTERM,
        .is_startup_check = 1
    },
    {
        .command = { "/usr/bin/sh", "-c", "echo doing check...; sleep 6", NULL },
        .name = "CHECK2",
        .receives_sigusr1 = 0,
        .receives_sigusr2 = 0,
        .termination_signal = SIGTERM,
        .is_startup_check = 1
    }
};




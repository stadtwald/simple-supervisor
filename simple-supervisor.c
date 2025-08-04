/*
 * Copyright (c) 2025 William Stadtwald Demchick <william.demchick@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#if __CHAR_BIT__ != 8
#error "we only support 8-bit bytes"
#endif

// this can safely be adjusted upwards if necessary
#define MAX_CHILD_COMMAND_ARGUMENT_COUNT 20

struct child_configuration {
    char *command[MAX_CHILD_COMMAND_ARGUMENT_COUNT + 1];
    const char *name;
    int receives_sigusr1;
    int receives_sigusr2;
    int termination_signal;
};

/*** BEGIN CONFIGURATION ***/

#define MAX_LINE_LENGTH 120

// this length includes a terminating line feed
const char *startup_check_command[] = { NULL };
const char *health_check_command[] = { NULL };
unsigned int shutdown_timeout = 10; // in seconds

#define CHILDREN_COUNT 1
const struct child_configuration child_configuration[CHILDREN_COUNT] = {
    {
        .command = { "/bin/sh", "-c", "while true; do sleep 5; echo 'hello'; done", NULL },
        .name = "SLEEPER",
        .receives_sigusr1 = 0,
        .receives_sigusr2 = 0,
        .termination_signal = SIGTERM
    }
};


/*** END CONFIGURATION ***/

char err_buffer[CHILDREN_COUNT][MAX_LINE_LENGTH + 1];
size_t err_buffer_position[CHILDREN_COUNT];
char out_buffer[CHILDREN_COUNT][MAX_LINE_LENGTH + 1];
size_t out_buffer_position[CHILDREN_COUNT];
int child_stdin[CHILDREN_COUNT];
int child_stdout[CHILDREN_COUNT];
int child_stderr[CHILDREN_COUNT];
pid_t child_pid[CHILDREN_COUNT];
int child_running[CHILDREN_COUNT];

int signal_r;
int signal_w;
int teardown_in_progress;
volatile sig_atomic_t termination_signal_received;
volatile sig_atomic_t sigusr1_received;
volatile sig_atomic_t sigusr2_received;
volatile sig_atomic_t sigalrm_received;

void signal_handler(int signum) {
    if(signum == SIGTERM || signum == SIGINT) {
        termination_signal_received = 1;
    } else if(signum == SIGUSR1) {
        sigusr1_received = 1;
    } else if(signum == SIGUSR2) {
        sigusr2_received = 1;
    } else if(signum == SIGALRM) {
        sigalrm_received = 1;
    }

    char buffer = 'X';
    write(signal_w, &buffer, 1);
}

__attribute__((noreturn))
void execute(const struct child_configuration *configuration, int p_in, int p_out, int p_err) {
    if(dup2(p_in, STDIN_FILENO) == -1) {
        err(1, "dup2() for stdin");
    }

    if(dup2(p_out, STDOUT_FILENO) == -1) {
        err(1, "dup2() for stdout");
    }

    if(dup2(p_err, STDERR_FILENO) == -1) {
        err(1, "dup2() for stderr");
    }

    execv(configuration->command[0], &configuration->command[0]);

    err(1, "execve()");
}

int setup_children() {
    int children_setup = 0;

    for(int i = 0; i < CHILDREN_COUNT; i += 1) {
        int p_err[2];
        int p_in[2];
        int p_out[2];

        if(pipe(&p_err[0]) == -1) {
            warn("pipe()");
            break;
        }

        if(pipe(&p_in[0]) == -1) {
            warn("pipe()");
            break;
        }

        if(pipe(&p_out[0]) == -1) {
            warn("pipe()");
            break;
        }

        child_stderr[i] = p_err[0];
        child_stdout[i] = p_out[0];
        close(p_in[1]);

        pid_t pid = fork();

        if(pid == -1) {
            warn("fork()");
            break;
        } else if(pid == 0) {
            close(child_stderr[i]);
            close(child_stdout[i]);

            execute(&child_configuration[i], p_in[0], p_out[1], p_err[1]);
        } else {
            close(p_in[0]);
            close(p_out[1]);
            close(p_err[1]);

            child_pid[i] = pid;
            child_running[i] = 1;
            children_setup += 1;
        }
    }

    return children_setup;
}

void setup_signal_handler() {
    {
        int fds[2];

        if(pipe(&fds[0]) == -1) {
            err(1, "pipe()");
        }

        signal_r = fds[0];
        signal_w = fds[1];

        if(fcntl(signal_w, F_SETFL, O_NONBLOCK) == -1) {
            err(1, "fcntl(%i, F_SETFL, O_NONBLOCK)", signal_w);
        }
    }

    struct sigaction sig;

    bzero(&sig, sizeof(struct sigaction));

    sig.sa_handler = signal_handler;

    if(sigemptyset(&sig.sa_mask) == -1) {
        errx(1, "could not clear signal mask");
    }

    if(sigaction(SIGTERM, &sig, NULL) == -1) {
        err(1, "could not set SIGTERM handler");
    }

    if(sigaction(SIGINT, &sig, NULL) == -1) {
        err(1, "could not set SIGINT handler");
    }

    if(sigaction(SIGUSR1, &sig, NULL) == -1) {
        err(1, "could not set SIGUSR1 handler");
    }

    if(sigaction(SIGUSR2, &sig, NULL) == -1) {
        err(1, "could not set SIGUSR2 handler");
    }

    if(sigaction(SIGCHLD, &sig, NULL) == -1) {
        err(1, "could not set SIGCHLD handler");
    }

    if(sigaction(SIGALRM, &sig, NULL) == -1) {
        err(1, "could not set SIGALRM handler");
    }
}

void teardown() {
    if(teardown_in_progress) {
        return;
    }

    teardown_in_progress = 1;

    for(int i = 0; i < CHILDREN_COUNT; i += 1) {
        if(!child_running[i]) {
            continue;
        }

        kill(child_pid[i], child_configuration[i].termination_signal);
    }

    alarm(shutdown_timeout);
}

__attribute__((noreturn))
void brutal_teardown() {
    for(int i = 0; i < CHILDREN_COUNT; i += 1) {
        if(child_running[i]) {
            kill(child_pid[i], SIGKILL);
        }
    }

    exit(1);
}

void reap(pid_t pid) {
    for(int i = 0; i < CHILDREN_COUNT; i += 1) {
        if(child_pid[i] == pid && child_running[i]) {
            child_pid[i] = -1;
            child_running[i] = 0;

            close(child_stderr[i]);
            close(child_stdout[i]);

            child_stderr[i] = -1;
            child_stdout[i] = -1;

            printf("[SYSTEM] Process for %s (%lli) has died.\n", child_configuration[i].name, (long long int)pid);

            break;
        }
    }
}

int pump_buffer(char *buffer, size_t *p_position, int output_fd, int input_fd, const char *child_name) {
    size_t buffer_space_left = MAX_LINE_LENGTH - *p_position;
    char tmp_buffer[MAX_LINE_LENGTH];
    ssize_t bytes_read = read(input_fd, &tmp_buffer[0], buffer_space_left);

    if(bytes_read == -1) {
        return -1;
    }

    if(bytes_read == 0) {
        *(buffer + *p_position) = 0;
        dprintf(output_fd, "[%s] %s\n", child_name, buffer);
        *p_position = 0;
        return 0;
    }

    char *inp = &tmp_buffer[0];
    char *outp = buffer + *p_position;

    for(int bytes_processed = 0; bytes_processed < bytes_read; bytes_processed += 1, inp += 1) {
        if(*inp == '\r') {
        } else if(*inp == '\n') {
            *outp = 0;
            dprintf(output_fd, "[%s] %s\n", child_name, buffer);
            *p_position = 0;
            outp = &buffer[0];
        } else if(*inp < ' ' || *inp == 127) {
            *outp = ' ';
            outp += 1;
            *p_position += 1;
        } else {
            *outp = *inp;
            outp += 1;
            *p_position += 1;
        }
    }

    if(buffer_space_left == 0) {
        *(buffer + *p_position) = 0;
        dprintf(output_fd, "[%s] %s\n", child_name, buffer);
        *p_position = 0;
    }

    return 1;
}

#define FLAVOUR_SIGNAL (-1)
#define FLAVOUR_STDOUT (1)
#define FLAVOUR_STDERR (2)

void event_loop() {
    while(1) {
        nfds_t fd_count = 0;
        struct pollfd fd[CHILDREN_COUNT * 2 + 1];
        int fd_child[CHILDREN_COUNT * 2 + 1];
        int fd_flavour[CHILDREN_COUNT * 2 + 1];

        fd[fd_count].fd = signal_r;
        fd[fd_count].events = POLLIN;
        fd[fd_count].revents = 0;
        fd_child[fd_count] = -1;
        fd_flavour[fd_count] = FLAVOUR_SIGNAL;
        
        fd_count += 1;

        for(int i = 0; i < CHILDREN_COUNT; i += 1) {
            if(!child_running[i]) {
                continue;
            }

            if(child_stdout[i] != -1) {
                fd[fd_count].fd = child_stdout[i];
                fd[fd_count].events = POLLIN;
                fd[fd_count].revents = 0;
                fd_child[fd_count] = i;
                fd_flavour[fd_count] = FLAVOUR_STDOUT;
                fd_count += 1;
            }

            if(child_stderr[i] != -1) {
                fd[fd_count].fd = child_stderr[i];
                fd[fd_count].events = POLLIN;
                fd[fd_count].revents = 0;
                fd_child[fd_count] = i;
                fd_flavour[fd_count] = FLAVOUR_STDERR;
                fd_count += 1;
            }
        }

        int status = poll(&fd[0], fd_count, -1);

        if(status == -1 && errno != EINTR) {
            warn("poll()");
        }

        if(status > 0) {
            for(int j = 0; j < fd_count; j += 1) {
                if((fd[j].revents & POLLIN) != POLLIN) {
                    continue;
                }

                if(fd_flavour[j] == FLAVOUR_SIGNAL) {
                    char dummy[1000];
                    read(signal_r, &dummy[0], 1000);
                    continue;
                }

                int child_i = fd_child[j];
                char *buffer = (fd_flavour[j] == FLAVOUR_STDOUT) ? &out_buffer[child_i][0] : &err_buffer[child_i][0];
                size_t *p_position = (fd_flavour[j] == FLAVOUR_STDOUT) ? &out_buffer_position[child_i] : &err_buffer_position[child_i];
                int output_fd = (fd_flavour[j] == FLAVOUR_STDOUT) ? STDOUT_FILENO : STDERR_FILENO;

                if(pump_buffer(buffer, p_position, output_fd, fd[j].fd, child_configuration[child_i].name) < 1) {
                    close(fd[j].fd);

                    if(fd_flavour[j] == FLAVOUR_STDOUT) {
                        child_stdout[child_i] = -1;
                    }

                    if(fd_flavour[j] == FLAVOUR_STDERR) {
                        child_stderr[child_i] = -1;
                    }
                }
            }
        }

        if(termination_signal_received) {
            termination_signal_received = 0;
            printf("[SYSTEM] Received request to terminate.\n");
            if(teardown_in_progress) {
                printf("[SYSTEM] Shutdown already in progress, so performing hard shutdown.\n");
                brutal_teardown();
            }
            printf("[SYSTEM] Performing soft shutdown.\n");
            teardown();
        }

        if(sigusr1_received) {
            sigusr1_received = 0;

            for(int i = 0; i < CHILDREN_COUNT; i += 1) {
                if(!child_running[i]) {
                    continue;
                }
                if(child_configuration[i].receives_sigusr1) {
                    printf("[SYSTEM] Passing SIGUSR1 to child %s (%lli).\n", child_configuration[i].name, (long long int)child_pid[i]);
                    kill(child_pid[i], SIGUSR1);
                }
            }
        }

        if(sigusr2_received) {
            sigusr2_received = 0;

            for(int i = 0; i < CHILDREN_COUNT; i += 1) {
                if(!child_running[i]) {
                    continue;
                }
                if(child_configuration[i].receives_sigusr2) {
                    printf("[SYSTEM] Passing SIGUSR2 to child %s (%lli).\n", child_configuration[i].name, (long long int)child_pid[i]);
                    kill(child_pid[i], SIGUSR2);
                }
            }
        }

        if(sigalrm_received) {
            sigalrm_received = 0;

            printf("[SYSTEM] Shutdown timeout has arrived, performing hard shutdown.\n");

            brutal_teardown();
        }

        while(1) {
            pid_t pid = waitpid(-1, NULL, WNOHANG);

            if(pid < 1) {
                break;
            }

            reap(pid);

            teardown();
        }

        {
            int some_child_running = 0;

            for(int i = 0; i < CHILDREN_COUNT; i += 1) {
                if(child_running[i]) {
                    some_child_running = 1;
                    break;
                }
            }

            if(!some_child_running) {
                printf("[SYSTEM] All child processes have terminated.\n");
                exit(1);
            }
        }
    }
}

int main(int argc, char **argv) {
    setup_signal_handler();

    int children_spawned = setup_children();

    if(children_spawned != CHILDREN_COUNT) {
        teardown();
    }

    event_loop();

    return 1;
}


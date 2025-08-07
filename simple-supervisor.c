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

// this can safely be adjusted upwards if necessary
#define MAX_CHILD_COMMAND_ARGUMENT_COUNT 20

struct child_configuration {
    char *command[MAX_CHILD_COMMAND_ARGUMENT_COUNT + 1];
    const char *name;
    int receives_sigusr1;
    int receives_sigusr2;
    int termination_signal;
    int is_startup_check;
};

#include "config.h"

#define PHASE_CHECK 0
#define PHASE_NORMAL 1

struct buffer {
    char buffer[MAX_LINE_LENGTH + 1];
    size_t position;
    int destination_fd;
    int source_fd;
};

struct child_state {
    struct buffer out_buffer;
    struct buffer err_buffer;
    pid_t pid;
    int running;
    const struct child_configuration *config;
};

struct child_state children[CHILDREN_COUNT];

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
#ifdef __OpenBSD__
    if(pledge("stdio exec", NULL) == -1) {
        err(1, "pledge()");
    }
#endif

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

int setup_children(int phase) {
    int rv = 0;

    for(int i = 0; i < CHILDREN_COUNT; i += 1) {
        const struct child_configuration *config = &child_configuration[i];
        int p_err[2];
        int p_in[2];
        int p_out[2];

        if(phase == PHASE_CHECK && !config->is_startup_check) {
            continue;
        }

        if(phase == PHASE_NORMAL && config->is_startup_check) {
            continue;
        }

        children[i].config = config;

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

        children[i].err_buffer.source_fd = p_err[0];
        children[i].err_buffer.destination_fd = STDERR_FILENO;
        children[i].out_buffer.source_fd = p_out[0];
        children[i].out_buffer.destination_fd = STDOUT_FILENO;
        close(p_in[1]);

        if(fcntl(p_err[0], F_SETFD, FD_CLOEXEC) == -1) {
            warn("fcntl(..., F_SETFD, FD_CLOEXEC)");
            rv = -1;
            break;
        }

        if(fcntl(p_out[0], F_SETFD, FD_CLOEXEC) == -1) {
            warn("fcntl(..., F_SETFD, FD_CLOEXEC)");
            rv = -1;
            break;
        }

        pid_t pid = fork();

        if(pid == -1) {
            warn("fork()");
            rv = -1;
            break;
        } else if(pid == 0) {
            execute(config, p_in[0], p_out[1], p_err[1]);
        } else {
            close(p_in[0]);
            close(p_out[1]);
            close(p_err[1]);

            children[i].pid = pid;
            children[i].running = 1;
            rv += 1;
        }
    }

    return rv;
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

    printf("[SYSTEM] Asking all processes to exit.\n");

    teardown_in_progress = 1;

    for(int i = 0; i < CHILDREN_COUNT; i += 1) {
        if(!children[i].running) {
            continue;
        }

        kill(children[i].pid, children[i].config->termination_signal);
    }

    alarm(SHUTDOWN_TIMEOUT);
}

__attribute__((noreturn))
void brutal_teardown() {
    for(int i = 0; i < CHILDREN_COUNT; i += 1) {
        if(children[i].running) {
            kill(children[i].pid, SIGKILL);
        }
    }

    exit(1);
}

void reap(pid_t pid, int exit_status) {
    for(int i = 0; i < CHILDREN_COUNT; i += 1) {
        if(children[i].pid == pid && children[i].running) {
            children[i].pid = -1;
            children[i].running = 0;

            close(children[i].err_buffer.source_fd);
            close(children[i].out_buffer.source_fd);

            children[i].err_buffer.source_fd = -1;
            children[i].out_buffer.source_fd = -1;

            if(children[i].config->is_startup_check) {
                if(exit_status == 0) {
                    printf("[SYSTEM] Process for %s (%lli) has indicated success.\n", children[i].config->name, (long long int)pid);
                } else {
                    printf("[SYSTEM] Process for %s (%lli) has indicated failure.\n", children[i].config->name, (long long int)pid);
                }
            } else {
                printf("[SYSTEM] Process for %s (%lli) has exited.\n", children[i].config->name, (long long int)pid);
            }

            break;
        }
    }
}

void flush_buffer(struct buffer *buffer, const char *child_name) {
    *(&buffer->buffer[0] + buffer->position) = 0;
    dprintf(buffer->destination_fd, "[%s] %s\n", child_name, &buffer->buffer[0]);
    buffer->position = 0;
}

int pump_buffer(struct buffer *buffer, const char *child_name) {
    size_t buffer_space_left = MAX_LINE_LENGTH - buffer->position;
    char tmp_buffer[MAX_LINE_LENGTH];
    ssize_t bytes_read = read(buffer->source_fd, &tmp_buffer[0], buffer_space_left);

    if(bytes_read == -1) {
        return -1;
    }

    if(bytes_read == 0) {
        flush_buffer(buffer, child_name);
        return 0;
    }

    char *inp = &tmp_buffer[0];
    char *outp = &buffer->buffer[0] + buffer->position;

    for(int bytes_processed = 0; bytes_processed < bytes_read; bytes_processed += 1, inp += 1) {
        if(*inp == '\r') {
        } else if(*inp == '\n') {
            flush_buffer(buffer, child_name);
            outp = &buffer->buffer[0];
        } else if(*inp < ' ' || *inp == 127) {
            *outp = ' ';
            outp += 1;
            buffer->position += 1;
        } else {
            *outp = *inp;
            outp += 1;
            buffer->position += 1;
        }
    }

    if(buffer_space_left == 0) {
        flush_buffer(buffer, child_name);
    }

    return 1;
}

void check_signals() {
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

        printf("[SYSTEM] Received SIGUSR1.\n");

        for(int i = 0; i < CHILDREN_COUNT; i += 1) {
            if(!children[i].running) {
                continue;
            }
            if(children[i].config->receives_sigusr1) {
                printf("[SYSTEM] Passing SIGUSR1 to child %s (%lli).\n", children[i].config->name, (long long int)children[i].pid);
                kill(children[i].pid, SIGUSR1);
            }
        }
    }

    if(sigusr2_received) {
        sigusr2_received = 0;

        printf("[SYSTEM] Received SIGUSR2.\n");

        for(int i = 0; i < CHILDREN_COUNT; i += 1) {
            if(!children[i].running) {
                continue;
            }
            if(children[i].config->receives_sigusr2) {
                printf("[SYSTEM] Passing SIGUSR2 to child %s (%lli).\n", children[i].config->name, (long long int)children[i].pid);
                kill(children[i].pid, SIGUSR2);
            }
        }
    }

    if(sigalrm_received) {
        sigalrm_received = 0;

        printf("[SYSTEM] Shutdown timeout has arrived, performing hard shutdown.\n");

        brutal_teardown();
    }
}

void check_for_terminations(int phase) {
    while(1) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if(pid < 1) {
            break;
        }

        reap(pid, status);

        if(WEXITSTATUS(status) != 0 || phase != PHASE_CHECK) {
            teardown();
        }
    }
}

int check_pending() {
    int some_child_running = 0;

    for(int i = 0; i < CHILDREN_COUNT; i += 1) {
        if(children[i].running) {
            some_child_running = 1;
            break;
        }
    }

    return some_child_running;
}

#define FLAVOUR_SIGNAL (-1)
#define FLAVOUR_STDOUT (1)
#define FLAVOUR_STDERR (2)

struct poll_data {
    nfds_t count;
    struct pollfd entry[CHILDREN_COUNT * 2 + 1];
    struct child_state *child[CHILDREN_COUNT * 2 + 1];
    int flavour[CHILDREN_COUNT * 2 + 1];
};

void setup_poll(struct poll_data *data) {
    data->count = 0;

    data->entry[data->count].fd = signal_r;
    data->entry[data->count].events = POLLIN;
    data->entry[data->count].revents = 0;
    data->child[data->count] = NULL;
    data->flavour[data->count] = FLAVOUR_SIGNAL;        
    data->count += 1;

    for(int i = 0; i < CHILDREN_COUNT; i += 1) {
        struct child_state *child = &children[i];

        if(!child->running) {
            continue;
        }

        if(child->out_buffer.source_fd != -1) {
            data->entry[data->count].fd = child->out_buffer.source_fd;
            data->entry[data->count].events = POLLIN;
            data->entry[data->count].revents = 0;
            data->child[data->count] = child;
            data->flavour[data->count] = FLAVOUR_STDOUT;
            data->count += 1;
        }

        if(child->err_buffer.source_fd != -1) {
            data->entry[data->count].fd = child->err_buffer.source_fd;
            data->entry[data->count].events = POLLIN;
            data->entry[data->count].revents = 0;
            data->child[data->count] = child;
            data->flavour[data->count] = FLAVOUR_STDERR;
            data->count += 1;
        }
    }
}

void handle_io(struct poll_data *data) {
    for(int j = 0; j < data->count; j += 1) {
        if((data->entry[j].revents & POLLIN) != POLLIN) {
            continue;
        }

        if(data->flavour[j] == FLAVOUR_SIGNAL) {
            char dummy[1000];
            read(signal_r, &dummy[0], 1000);
            continue;
        }

        struct child_state *child = data->child[j];
        struct buffer *buffer = (data->flavour[j] == FLAVOUR_STDOUT) ? &child->out_buffer : &child->err_buffer;

        if(pump_buffer(buffer, child->config->name) < 1) {
            close(data->entry[j].fd);

            if(data->flavour[j] == FLAVOUR_STDOUT) {
                child->out_buffer.source_fd = -1;
            }

            if(data->flavour[j] == FLAVOUR_STDERR) {
                child->err_buffer.source_fd = -1;
            }
        }
    } 
}

int pump(int phase) {
    struct poll_data data;

    setup_poll(&data);

    int status = poll(&data.entry[0], data.count, -1);

    if(status == -1 && errno != EINTR) {
        warn("poll()");
    }

    if(status > 0) {
        handle_io(&data);
    }

    check_signals();
    check_for_terminations(phase);

    return check_pending();
}

void startup_check() {
    int result = setup_children(PHASE_CHECK);

    if(result == -1) {
        printf("[SYSTEM] Not all check commands could be spawned.\n");
        teardown();
    } else if(result == 0) {
        return;
    }

    while(pump(PHASE_CHECK)) {}

    if(!teardown_in_progress) {
        printf("[SYSTEM] All startup checks have passed.\n");
    }
}

void normal_phase() {
    int result = setup_children(PHASE_NORMAL);

    if(result == -1) {
        printf("[SYSTEM] Not all children could be spawned.\n");
        teardown();
    } else if(result == 0) {
        printf("[SYSTEM] No children specified in configuration, exiting.\n");
        return;
    } else {
        printf("[SYSTEM] All processes have been spawned.\n");
    }

#ifdef __OpenBSD__
    if(pledge("stdio proc", NULL) == -1) {
        err(1, "pledge()");
    }
#endif

    while(pump(PHASE_NORMAL)) {};

    printf("[SYSTEM] All child processes have exited.\n");
}

int main(int argc, char **argv) {
#ifdef __OpenBSD__
    if(unveil("/", "x") == -1) {
        err(1, "unveil()");
    }

    if(pledge("stdio proc exec", NULL) == -1) {
        err(1, "pledge()");
    }
#endif

    if(argc > 1) {
        errx(1, "no command line arguments accepted");
    }

    setup_signal_handler();

    startup_check();

    if(teardown_in_progress) {
        printf("[SYSTEM] Startup check failed, shutting down.\n");
        return 1;
    }

    normal_phase();

    return 1;
}


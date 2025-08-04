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

struct buffer {
    char buffer[MAX_LINE_LENGTH + 1];
    size_t position;
};

struct child_state {
    struct buffer out_buffer;
    struct buffer err_buffer;
    int stdout;
    int stderr;
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
        const struct child_configuration *config = &child_configuration[i];
        int p_err[2];
        int p_in[2];
        int p_out[2];

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

        children[i].stderr = p_err[0];
        children[i].stdout = p_out[0];
        close(p_in[1]);

        pid_t pid = fork();

        if(pid == -1) {
            warn("fork()");
            break;
        } else if(pid == 0) {
            close(children[i].stderr);
            close(children[i].stdout);

            execute(config, p_in[0], p_out[1], p_err[1]);
        } else {
            close(p_in[0]);
            close(p_out[1]);
            close(p_err[1]);

            children[i].pid = pid;
            children[i].running = 1;
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

    printf("[SYSTEM] Asking all processes to exit.\n");

    teardown_in_progress = 1;

    for(int i = 0; i < CHILDREN_COUNT; i += 1) {
        if(!children[i].running) {
            continue;
        }

        kill(children[i].pid, children[i].config->termination_signal);
    }

    alarm(shutdown_timeout);
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

void reap(pid_t pid) {
    for(int i = 0; i < CHILDREN_COUNT; i += 1) {
        if(children[i].pid == pid && children[i].running) {
            children[i].pid = -1;
            children[i].running = 0;

            close(children[i].stderr);
            close(children[i].stdout);

            children[i].stderr = -1;
            children[i].stdout = -1;

            printf("[SYSTEM] Process for %s (%lli) has exited.\n", children[i].config->name, (long long int)pid);

            break;
        }
    }
}

void flush_buffer(struct buffer *buffer, int output_fd, const char *child_name) {
    *(&buffer->buffer[0] + buffer->position) = 0;
    dprintf(output_fd, "[%s] %s\n", child_name, &buffer->buffer[0]);
    buffer->position = 0;
}

int pump_buffer(struct buffer *buffer, int output_fd, int input_fd, const char *child_name) {
    size_t buffer_space_left = MAX_LINE_LENGTH - buffer->position;
    char tmp_buffer[MAX_LINE_LENGTH];
    ssize_t bytes_read = read(input_fd, &tmp_buffer[0], buffer_space_left);

    if(bytes_read == -1) {
        return -1;
    }

    if(bytes_read == 0) {
        flush_buffer(buffer, output_fd, child_name);
        return 0;
    }

    char *inp = &tmp_buffer[0];
    char *outp = &buffer->buffer[0] + buffer->position;

    for(int bytes_processed = 0; bytes_processed < bytes_read; bytes_processed += 1, inp += 1) {
        if(*inp == '\r') {
        } else if(*inp == '\n') {
            flush_buffer(buffer, output_fd, child_name);
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
        flush_buffer(buffer, output_fd, child_name);
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
        struct child_state *fd_child[CHILDREN_COUNT * 2 + 1];
        int fd_flavour[CHILDREN_COUNT * 2 + 1];

        fd[fd_count].fd = signal_r;
        fd[fd_count].events = POLLIN;
        fd[fd_count].revents = 0;
        fd_child[fd_count] = NULL;
        fd_flavour[fd_count] = FLAVOUR_SIGNAL;
        
        fd_count += 1;

        for(int i = 0; i < CHILDREN_COUNT; i += 1) {
            if(!children[i].running) {
                continue;
            }

            if(children[i].stdout != -1) {
                fd[fd_count].fd = children[i].stdout;
                fd[fd_count].events = POLLIN;
                fd[fd_count].revents = 0;
                fd_child[fd_count] = &children[i];
                fd_flavour[fd_count] = FLAVOUR_STDOUT;
                fd_count += 1;
            }

            if(children[i].stderr != -1) {
                fd[fd_count].fd = children[i].stderr;
                fd[fd_count].events = POLLIN;
                fd[fd_count].revents = 0;
                fd_child[fd_count] = &children[i];
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

                struct child_state *child = fd_child[j];
                struct buffer *buffer = (fd_flavour[j] == FLAVOUR_STDOUT) ? &child->out_buffer : &child->err_buffer;
                int output_fd = (fd_flavour[j] == FLAVOUR_STDOUT) ? STDOUT_FILENO : STDERR_FILENO;

                if(pump_buffer(buffer, output_fd, fd[j].fd, child->config->name) < 1) {
                    close(fd[j].fd);

                    if(fd_flavour[j] == FLAVOUR_STDOUT) {
                        child->stdout = -1;
                    }

                    if(fd_flavour[j] == FLAVOUR_STDERR) {
                        child->stderr = -1;
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
                if(children[i].running) {
                    some_child_running = 1;
                    break;
                }
            }

            if(!some_child_running) {
                printf("[SYSTEM] All child processes have exited.\n");
                exit(1);
            }
        }
    }
}

int main(int argc, char **argv) {
    setup_signal_handler();

    int children_spawned = setup_children();

    if(children_spawned != CHILDREN_COUNT) {
        printf("[SYSTEM] Not all children could be spawned.\n");
        teardown();
    } else {
        printf("[SYSTEM] All processes have been spawned.\n");
    }

    event_loop();

    return 1;
}


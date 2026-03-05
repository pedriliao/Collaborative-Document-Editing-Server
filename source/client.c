/**
 * client.c - Console-based editor client for the ZOIT Docs collaborative system.
 *
 * Architecture Overview:
 *   - Main thread: Handles user input from stdin, sends commands to the server
 *     via the client-to-server FIFO, and processes local debugging commands.
 *   - Listener thread: Continuously reads server broadcasts from the
 *     server-to-client FIFO, applies successful edits to the local document
 *     copy, and logs all received messages.
 *
 * Connection Protocol:
 *   1. Send SIGRTMIN to the server PID to request a connection.
 *   2. Block and wait for SIGRTMIN+1 (server's "FIFOs ready" signal).
 *   3. Open FIFO_C2S_<pid> for writing and FIFO_S2C_<pid> for reading.
 *   4. Send username, receive role/version/document from server.
 *
 * Local Document:
 *   The client maintains a local copy of the document using the same
 *   markdown engine. Successful edits from version broadcasts are applied
 *   locally to keep the view in sync without re-transmitting the full document.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/time.h>
#include <sys/select.h>
#include "markdown.h"
#include "document.h"
#include "command.h"

#define MAX_LOG_ENTRIES 50
#define MAX_LOG_ENTRY_LEN 256
#define MAX_LINE 512

/* Client state machine for handling special server responses */
volatile enum { MODE_NORMAL, MODE_WAIT_DOC, MODE_WAIT_PERM } client_mode = MODE_NORMAL;

document *doc = NULL;           /* Local document copy */
uint64_t local_version = 0;    /* Tracks the current committed version locally */
char role[16];                  /* Client's permission level ("read" or "write") */
CommandLog global_log[100];     /* Local log of version broadcasts */
pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;  /* Protects log writes */

static char fifo_c2s_path[64], fifo_s2c_path[64];  /* FIFO paths for cleanup */
FILE *logfp;                    /* File handle for persistent edit logging */

/* ========== I/O Helpers ========== */

/**
 * read_line - Read a single newline-terminated line from a file descriptor.
 *
 * Reads one byte at a time until '\n' or EOF. Handles EINTR gracefully
 * (retries on signal interruption). Always null-terminates the output.
 *
 * @fd:     File descriptor to read from
 * @buf:    Output buffer
 * @maxlen: Maximum bytes to read (including null terminator)
 * Returns: Number of bytes read, 0 on EOF, -1 on error
 */
ssize_t read_line(int fd, char *buf, size_t maxlen) {
    size_t i = 0;
    char c;
    while (i + 1 < maxlen) {
        ssize_t r = read(fd, &c, 1);
        if (r == 1) {
            buf[i++] = c;
            if (c == '\n') break;
        } else if (r == 0) {
            break;  /* EOF */
        } else if (errno == EINTR) {
            continue;  /* Interrupted by signal, retry */
        } else {
            return -1;
        }
    }
    buf[i] = '\0';
    return i;
}

/* ========== Logging ========== */

/**
 * init_log_file - Create a user-specific log file under the home directory.
 *
 * The log file records all EDIT messages received from the server for
 * debugging and audit purposes.
 *
 * @username: Client username (used as filename prefix)
 */
void init_log_file(const char *username) {
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Cannot find HOME env variable\n");
        exit(1);
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.txt", home, username);

    logfp = fopen(path, "w");
    if (!logfp) {
        perror("Failed to open log file");
        exit(1);
    }
}

/* ========== Cleanup and Signal Handling ========== */

/**
 * cleanup - Remove FIFO files from the filesystem.
 *
 * Called on normal exit (via atexit) and on signal-triggered shutdown
 * to prevent stale named pipes from lingering.
 */
void cleanup(void) {
    unlink(fifo_c2s_path);
    unlink(fifo_s2c_path);
}

/**
 * sigint_handler - Handle SIGINT (Ctrl+C) for graceful shutdown.
 *
 * Performs FIFO cleanup and exits immediately using _exit() to avoid
 * calling non-async-signal-safe functions.
 */
void sigint_handler(int sig) {
    (void)sig;
    cleanup();
    _exit(1);
}

/* ========== Local Command Execution ========== */

/**
 * handle_command_local - Apply an editing command to the local document copy.
 *
 * Mirrors the server-side command dispatch logic but operates on the
 * client's local document instance using local_version. Called when
 * the listener thread receives a successful EDIT broadcast.
 *
 * @doc:     Local document instance
 * @cmd_str: Raw command string (e.g., "INSERT 0 Hello")
 * Returns: 0 on success, negative error code on failure
 */
int handle_command_local(document *doc, char *cmd_str) {
    char *copy = strdup(cmd_str);
    char *command = strtok(copy, " ");
    int rc = 0;

    if (strcmp(command, "INSERT") == 0) {
        char *pos_str = strtok(NULL, " ");
        char *text = strtok(NULL, "");
        if (pos_str && text) {
            size_t pos = atoi(pos_str);
            rc = markdown_insert(doc, local_version, pos, text);
        }
    } else if (strcmp(command, "DEL") == 0) {
        char *pos_str = strtok(NULL, " ");
        char *len_str = strtok(NULL, " ");
        if (pos_str && len_str) {
            size_t pos = atoi(pos_str);
            size_t len = atoi(len_str);
            rc = markdown_delete(doc, local_version, pos, len);
        }
    } else if (strcmp(command, "HEADING") == 0) {
        char *level_str = strtok(NULL, " ");
        char *pos_str = strtok(NULL, " ");
        if (level_str && pos_str) {
            size_t level = atoi(level_str);
            size_t pos = atoi(pos_str);
            rc = markdown_heading(doc, local_version, level, pos);
        }
    } else if (strcmp(command, "NEWLINE") == 0) {
        char *pos_str = strtok(NULL, " ");
        if (pos_str) {
            size_t pos = atoi(pos_str);
            rc = markdown_newline(doc, local_version, pos);
        }
    } else if (strcmp(command, "BOLD") == 0) {
        char *start_str = strtok(NULL, " ");
        char *end_str = strtok(NULL, " ");
        if (start_str && end_str) {
            size_t start = atoi(start_str);
            size_t end = atoi(end_str);
            rc = markdown_bold(doc, local_version, start, end);
        }
    } else if (strcmp(command, "ITALIC") == 0) {
        char *start_str = strtok(NULL, " ");
        char *end_str = strtok(NULL, " ");
        if (start_str && end_str) {
            size_t start = atoi(start_str);
            size_t end = atoi(end_str);
            rc = markdown_italic(doc, local_version, start, end);
        }
    } else if (strcmp(command, "ORDERED_LIST") == 0) {
        char *pos_str = strtok(NULL, " ");
        if (pos_str) {
            size_t pos = atoi(pos_str);
            rc = markdown_ordered_list(doc, local_version, pos);
        }
    } else if (strcmp(command, "UNORDERED_LIST") == 0) {
        char *pos_str = strtok(NULL, " ");
        if (pos_str) {
            size_t pos = atoi(pos_str);
            rc = markdown_unordered_list(doc, local_version, pos);
        }
    } else if (strcmp(command, "CODE") == 0) {
        char *start_str = strtok(NULL, " ");
        char *end_str = strtok(NULL, " ");
        if (start_str && end_str) {
            size_t start = atoi(start_str);
            size_t end = atoi(end_str);
            rc = markdown_code(doc, local_version, start, end);
        }
    } else if (strcmp(command, "BLOCKQUOTE") == 0) {
        char *pos_str = strtok(NULL, " ");
        if (pos_str) {
            size_t pos = atoi(pos_str);
            rc = markdown_blockquote(doc, local_version, pos);
        }
    } else if (strcmp(command, "HORIZONTAL_RULE") == 0) {
        char *pos_str = strtok(NULL, " ");
        if (pos_str) {
            size_t pos = atoi(pos_str);
            rc = markdown_horizontal_rule(doc, local_version, pos);
        }
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        rc = 1;
    }

    free(copy);
    return rc;
}

/* ========== Log Management ========== */

/**
 * write_log - Append an edit entry to the version log in a thread-safe manner.
 *
 * @log:          Target version log
 * @edit_command: The formatted log entry string
 */
void write_log(CommandLog *log, const char *edit_command) {
    if (!log) return;

    pthread_mutex_lock(&log_lock);

    if (log->entry_count >= MAX_LOG_ENTRIES) {
        fprintf(stderr, "Log full for this version.\n");
        pthread_mutex_unlock(&log_lock);
        return;
    }

    char log_entry[256];
    int written = snprintf(log_entry, sizeof(log_entry), "%s", edit_command);
    if ((size_t)written >= sizeof(log_entry)) {
        fprintf(stderr, "Warning: log_entry truncated. Total length = %d bytes\n", written);
    }

    strncpy(log->entries[log->entry_count], log_entry, MAX_LOG_ENTRY_LEN - 1);
    log->entries[log->entry_count][MAX_LOG_ENTRY_LEN - 1] = '\0';
    log->entry_count++;

    pthread_mutex_unlock(&log_lock);
}

/* ========== Server Listener Thread ========== */

/**
 * server_listener - Background thread that receives and processes server broadcasts.
 *
 * Handles three types of messages:
 *   - VERSION <N>:  Start of a new version broadcast
 *   - EDIT ...:     Individual command result; if SUCCESS and version matches,
 *                   apply the edit locally to keep the document in sync
 *   - END:          End of version broadcast; increment local version
 *
 * Also handles special modes (MODE_WAIT_DOC, MODE_WAIT_PERM) for
 * debugging commands that expect multi-line responses.
 *
 * @arg: Pointer to the server-to-client file descriptor
 */
void *server_listener(void *arg) {
    int fd_s2c = *((int *)arg);
    char line[512];
    uint64_t broadcast_version = 0;

    while (true) {
        ssize_t len = read_line(fd_s2c, line, sizeof(line));
        if (len <= 0) break;  /* Server disconnected or error */
        line[strcspn(line, "\r\n")] = '\0';

        /* Handle special response modes for DOC? and PERM? commands */
        if (client_mode == MODE_WAIT_DOC || client_mode == MODE_WAIT_PERM) {
            printf("%s\n", line);
            if (strcmp(line, "END") == 0 || client_mode == MODE_WAIT_PERM)
                client_mode = MODE_NORMAL;
            continue;
        }

        /* Parse version broadcast header */
        if (strncmp(line, "VERSION ", 8) == 0) {
            broadcast_version = strtoull(line + 8, NULL, 10);
            write_log(&global_log[broadcast_version], line);
            continue;
        }

        /* End of version broadcast: commit local version */
        if (strcmp(line, "END") == 0) {
            write_log(&global_log[broadcast_version], line);
            markdown_increment_version(doc);
            continue;
        }

        /* Process EDIT entries: log and apply successful edits locally */
        if (strncmp(line, "EDIT ", 5) == 0) {
            fprintf(logfp, "%s\n", line);
            fflush(logfp);
            write_log(&global_log[broadcast_version], line);

            /* Extract and apply only successful edits matching current version */
            char *success_ptr = strstr(line, " SUCCESS");
            if (success_ptr && broadcast_version == doc->version) {
                *success_ptr = '\0';
                char *command_part = line + 5;     /* Skip "EDIT " */
                char *cmd_start = strchr(command_part, ' ');  /* Skip username */
                if (cmd_start) {
                    handle_command_local(doc, cmd_start + 1);
                }
            }
        }
    }

    close(fd_s2c);
    return NULL;
}

/* ========== Entry Point ========== */

/**
 * main - Client entry point.
 *
 * Usage: ./client <server_pid> <username>
 *
 * Connection sequence:
 *   1. Register cleanup handlers for SIGINT and normal exit.
 *   2. Send SIGRTMIN to server, block-wait for SIGRTMIN+1 response.
 *   3. Open bidirectional FIFOs and send username.
 *   4. Receive authentication result (role or rejection).
 *   5. Receive initial document state (version + length + content).
 *   6. Initialize local document copy and start listener thread.
 *   7. Enter interactive command loop reading from stdin.
 */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_pid> <username>\n", argv[0]);
        exit(1);
    }

    pid_t client_pid = getpid();
    snprintf(fifo_c2s_path, sizeof(fifo_c2s_path), "FIFO_C2S_%d", client_pid);
    snprintf(fifo_s2c_path, sizeof(fifo_s2c_path), "FIFO_S2C_%d", client_pid);

    /* Register cleanup handlers */
    atexit(cleanup);
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* Signal the server and wait for FIFO readiness confirmation */
    pid_t server_pid = atoi(argv[1]);
    char *username = argv[2];
    kill(server_pid, SIGRTMIN);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGRTMIN + 1);
    sigprocmask(SIG_BLOCK, &set, NULL);

    int sig;
    sigwait(&set, &sig);  /* Block until server signals back */

    /* Open bidirectional FIFOs */
    char fifo_c2s[64], fifo_s2c[64];
    snprintf(fifo_c2s, sizeof(fifo_c2s), "FIFO_C2S_%d", client_pid);
    snprintf(fifo_s2c, sizeof(fifo_s2c), "FIFO_S2C_%d", client_pid);

    int fd_c2s = open(fifo_c2s, O_WRONLY);
    int fd_s2c = open(fifo_s2c, O_RDONLY);
    if (fd_c2s < 0 || fd_s2c < 0) {
        perror("open FIFO failed");
        exit(1);
    }

    /* Send username as the first message */
    dprintf(fd_c2s, "%s\n", username);

    /* Receive authentication response: role string */
    char line[MAX_LINE];
    if (read_line(fd_s2c, line, sizeof(line)) <= 0) {
        perror("reading role");
        exit(1);
    }

    /* Check for rejection */
    if (strcmp(line, "Reject UNAUTHORISED\n") == 0) {
        printf("%s\n", line);
        close(fd_c2s);
        close(fd_s2c);
        cleanup();
        return 0;
    }
    sscanf(line, "%s", role);

    /* Receive current document version */
    if (read_line(fd_s2c, line, sizeof(line)) <= 0) {
        perror("reading version");
        exit(1);
    }
    local_version = strtoull(line, NULL, 10);
    printf("receive local version is %lu\n", local_version);

    /* Receive document length */
    if (read_line(fd_s2c, line, sizeof(line)) <= 0) {
        perror("reading length");
        exit(1);
    }
    size_t doc_len = strtoull(line, NULL, 10);
    printf("receive doc len is %zu\n", doc_len);

    /* Receive full document content (length-prefixed byte stream) */
    char *buf = malloc(doc_len + 1);
    if (!buf) {
        perror("malloc");
        exit(1);
    }
    size_t got = 0;
    while (got < doc_len) {
        ssize_t r = read(fd_s2c, buf + got, doc_len - got);
        if (r > 0) {
            got += r;
        } else if (r == 0) {
            fprintf(stderr, "unexpected EOF\n");
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            perror("reading document");
            free(buf);
            exit(1);
        }
    }
    buf[got] = '\0';
    printf("now doc is %s\n", buf);

    /* Initialize local document copy with received content */
    doc = markdown_init();
    markdown_insert(doc, 0, 0, buf);
    doc->version = local_version;
    free(buf);

    printf("Connected as %s (%s mode). Local version = %lu\n", username, role, local_version);
    init_log_file(username);

    /* Start the background listener thread for server broadcasts */
    pthread_t tid;
    int *fd_ptr = malloc(sizeof(int));
    *fd_ptr = fd_s2c;
    pthread_create(&tid, NULL, server_listener, fd_ptr);

    /* Interactive command loop: read user input and forward to server */
    char input[256];
    while (fgets(input, sizeof(input), stdin)) {
        input[strcspn(input, "\n")] = '\0';

        if (strcmp(input, "DISCONNECT") == 0) {
            dprintf(fd_c2s, "DISCONNECT\n");
            pthread_cancel(tid);
            break;
        } else if (strcmp(input, "DOC?") == 0) {
            dprintf(fd_c2s, "DOC?\n");
            client_mode = MODE_WAIT_DOC;
        } else if (strcmp(input, "PERM?") == 0) {
            dprintf(fd_c2s, "PERM?\n");
            client_mode = MODE_WAIT_PERM;
        } else if (strcmp(input, "LOG?") == 0) {
            /* Print local log (no server round-trip needed) */
            pthread_mutex_lock(&log_lock);
            for (int v = 0; v < 100; v++) {
                if (global_log[v].entry_count == 0) continue;
                for (int i = 0; i < global_log[v].entry_count; i++) {
                    printf("%s\n", global_log[v].entries[i]);
                }
            }
            pthread_mutex_unlock(&log_lock);
        } else {
            /* Forward all other commands (edits) to the server */
            dprintf(fd_c2s, "%s\n", input);
        }
    }

    /* Graceful shutdown: close FIFOs, free resources */
    close(fd_c2s);
    close(fd_s2c);
    cleanup();
    free(fd_ptr);
    markdown_free(doc);
    pthread_join(tid, NULL);
    return 0;
}

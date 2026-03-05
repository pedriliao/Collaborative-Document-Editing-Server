/**
 * server.c - Authoritative document host for the ZOIT Docs collaborative editor.
 *
 * Architecture Overview:
 *   - Main thread: Waits for SIGRTMIN signals from connecting clients and
 *     spawns a dedicated POSIX thread for each new client.
 *   - Per-client threads: Handle FIFO-based bidirectional communication,
 *     authenticate users via roles.txt, and queue incoming commands.
 *   - Timer thread: Periodically collects commands from all client queues,
 *     sorts them by timestamp, executes them atomically, and broadcasts
 *     version updates to all connected clients.
 *   - Terminal thread: Accepts operator commands (DOC?, LOG?, QUIT) via stdin.
 *
 * Communication Protocol:
 *   - Named FIFOs: FIFO_C2S_<pid> (client→server), FIFO_S2C_<pid> (server→client)
 *   - Signaling: SIGRTMIN (client→server: "I want to connect"),
 *                SIGRTMIN+1 (server→client: "FIFOs are ready")
 *
 * Synchronization:
 *   - Each client thread has a mutex-protected command queue.
 *   - The timer thread locks each client's queue to drain commands.
 *   - Commands are globally sorted by nanosecond timestamps before execution.
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
#define MAX_CLIENTS 32

/* ========== Global State ========== */

Client clients[MAX_CLIENTS];       /* Array of client slots (fixed-size pool) */
int interval = 1;                  /* Sync interval in milliseconds */
Command *global_queue;             /* Merged command queue for current sync round */
size_t global_queue_len;           /* Number of commands in global queue */
CommandLog global_log[100];        /* Version-indexed command logs for broadcast */
int version_count = 0;             /* Total number of committed versions */
document *doc;                     /* The authoritative document instance */
static sigset_t signal_mask;       /* Signal mask for SIGRTMIN/SIGRTMIN+1 */

/* ========== Utility Functions ========== */

/**
 * get_timestamp_ns - Get the current wall-clock time in nanoseconds.
 *
 * Used to assign arrival timestamps to client commands, enabling
 * deterministic global ordering during the synchronization phase.
 *
 * Returns: uint64_t nanosecond timestamp
 */
static uint64_t get_timestamp_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ========== Client Authentication ========== */

/**
 * check_roles - Authenticate a client against roles.txt and initialize session.
 *
 * Workflow:
 *   1. Parse roles.txt to find the client's username and permission level.
 *   2. If found: send role, current version, and full document content via FIFO.
 *   3. If not found: send "Reject UNAUTHORISED", wait 1 second, clean up, and
 *      terminate the client thread.
 *
 * @cli: Pointer to the client structure (username must already be set)
 */
void check_roles(Client *cli) {
    FILE *fp = fopen("roles.txt", "r");
    if (fp == NULL) {
        perror("fopen");
        exit(1);
    }

    char line[1000];
    bool if_find = false;
    char real_role[16] = {0};

    /* Scan roles.txt line by line for a matching username */
    while (fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        char *name = strtok(line, " \t");
        char *role = strtok(NULL, " \t");

        if (strcmp(name, cli->username) == 0) {
            if_find = true;
            strncpy(real_role, role, sizeof(real_role) - 1);
            real_role[sizeof(real_role) - 1] = '\0';
            break;
        }
    }
    fclose(fp);

    if (!if_find) {
        /* Unauthorized: reject and clean up */
        printf("did not find client name %s\n", cli->username);
        char *false_message = "Reject UNAUTHORISED\n";
        write(cli->s2c, false_message, strlen(false_message));
        sleep(1);  /* Wait before cleanup (spec requirement, non-blocking to process) */
        cli->connect = false;
        close(cli->s2c);
        close(cli->c2s);

        /* Remove FIFO files from filesystem */
        char fifo_c2s[64], fifo_s2c[64];
        snprintf(fifo_c2s, sizeof(fifo_c2s), "FIFO_C2S_%d", cli->pid);
        snprintf(fifo_s2c, sizeof(fifo_s2c), "FIFO_S2C_%d", cli->pid);
        unlink(fifo_c2s);
        unlink(fifo_s2c);
        pthread_exit(NULL);
    }

    strncpy(cli->role, real_role, sizeof(cli->role) - 1);
    cli->role[sizeof(cli->role) - 1] = '\0';
    printf("find client name %s, role %s\n", cli->username, real_role);

    /* Send initial handshake payload: role, version, document length, document content */
    dprintf(cli->s2c, "%s\n", cli->role);
    dprintf(cli->s2c, "%lu\n", doc->version);

    char *flat = markdown_flatten(doc);
    size_t doc_len = strlen(flat);
    dprintf(cli->s2c, "%lu\n", doc_len);
    write(cli->s2c, flat, doc_len);

    fprintf(stderr, "[Server] Sent role: %s, version: %lu, len: %lu bytes\n",
            cli->role, doc->version, strlen(flat));
    free(flat);
}

/* ========== Per-Client Thread ========== */

/**
 * client_thread - Main loop for handling a single client connection.
 *
 * After authentication (check_roles), this thread continuously reads
 * commands from the client's FIFO. Special commands (DISCONNECT, PERM?,
 * DOC?) are handled immediately. Editing commands are timestamped and
 * queued for the next synchronization round.
 *
 * On disconnect, the thread cleans up FIFOs, frees the command queue,
 * and detaches itself.
 *
 * @arg: Pointer to the Client structure for this connection
 */
void *client_thread(void *arg) {
    Client *cli = (Client *)arg;
    check_roles(cli);

    char buffer[1024];
    printf("[Server] Client '%s', role = %s\n", cli->username, cli->role);

    while (cli->connect) {
        size_t bytes = read(cli->c2s, buffer, sizeof(buffer) - 1);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            char *newline = strchr(buffer, '\n');
            if (newline) *newline = '\0';

            printf("%s\n", buffer);

            /* Handle metadata/debugging commands immediately */
            if (strcmp("DISCONNECT", buffer) == 0) {
                cli->connect = false;
                break;
            } else if (strcmp(buffer, "PERM?") == 0) {
                dprintf(cli->s2c, "%s\n", cli->role);
                continue;
            } else if (strcmp(buffer, "DOC?") == 0) {
                char *flat = markdown_flatten(doc);
                dprintf(cli->s2c, "%s\n", flat);
                free(flat);
                continue;
            }

            /* Queue editing command with timestamp for global ordering */
            Command *now_comm = malloc(sizeof *now_comm);
            now_comm->comm = strdup(buffer);
            now_comm->client = cli;
            now_comm->timestamp = get_timestamp_ns();

            pthread_mutex_lock(&cli->queue_lock);
            /* Grow queue if at capacity */
            if (cli->queue_len + 1 == cli->queue_cap) {
                cli->queue_cap *= 2;
                Command *new_cli_comm = realloc(cli->queue,
                                                sizeof(Command) * cli->queue_cap);
                if (new_cli_comm == NULL) {
                    perror("realloc");
                    free(now_comm->comm);
                    free(now_comm);
                    continue;
                }
            }
            cli->queue[cli->queue_len++] = *now_comm;
            free(now_comm);
            pthread_mutex_unlock(&cli->queue_lock);
        }
    }

    /* Clean up: close FIFOs, unlink files, free queue, detach thread */
    close(cli->s2c);
    close(cli->c2s);

    char fifo_c2s[64], fifo_s2c[64];
    snprintf(fifo_c2s, sizeof(fifo_c2s), "FIFO_C2S_%d", cli->pid);
    snprintf(fifo_s2c, sizeof(fifo_s2c), "FIFO_S2C_%d", cli->pid);
    unlink(fifo_c2s);
    unlink(fifo_s2c);

    for (size_t i = 0; i < cli->queue_len; ++i) {
        free(cli->queue[i].comm);
    }
    free(cli->queue);
    pthread_detach(pthread_self());
    return NULL;
}

/* ========== Client Connection Setup ========== */

/**
 * make_client_thread - Initialize FIFO communication and spawn a client thread.
 *
 * Called when the main thread receives SIGRTMIN from a new client process.
 * Creates bidirectional named FIFOs, signals the client that pipes are ready,
 * reads the client's username, and starts the handler thread.
 *
 * @client_pid: PID of the connecting client process
 */
void make_client_thread(pid_t client_pid) {
    /* Construct FIFO paths using client PID */
    char fifo_c2s[64], fifo_s2c[64];
    snprintf(fifo_c2s, sizeof(fifo_c2s), "FIFO_C2S_%d", client_pid);
    snprintf(fifo_s2c, sizeof(fifo_s2c), "FIFO_S2C_%d", client_pid);

    /* Clean up any stale FIFOs and create fresh ones */
    unlink(fifo_c2s);
    unlink(fifo_s2c);
    mkfifo(fifo_c2s, 0666);
    mkfifo(fifo_s2c, 0666);

    /* Signal the client that FIFOs are ready */
    kill(client_pid, SIGRTMIN + 1);

    /* Open FIFOs (blocks until client opens the other end) */
    int fd_c2s = open(fifo_c2s, O_RDONLY);
    int fd_s2c = open(fifo_s2c, O_WRONLY);

    /* Read the client's username (first message after connection) */
    char name_buf[32];
    ssize_t n = read(fd_c2s, name_buf, sizeof(name_buf) - 1);
    if (n <= 0) {
        perror("read username");
        close(fd_c2s);
        close(fd_s2c);
        return;
    }
    printf("client name %s\n", name_buf);
    name_buf[n] = '\0';

    /* Find an empty client slot and initialize the Client structure */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].connect) {
            Client *cli = &clients[i];
            cli->c2s = fd_c2s;
            cli->s2c = fd_s2c;
            cli->connect = true;
            cli->queue_cap = 4;
            cli->queue_len = 0;
            cli->queue = malloc(sizeof(Command) * cli->queue_cap);
            pthread_mutex_init(&cli->queue_lock, NULL);

            strncpy(cli->username, name_buf, sizeof(cli->username) - 1);
            cli->username[sizeof(cli->username) - 1] = '\0';
            cli->username[strcspn(cli->username, "\r\n")] = '\0';

            cli->role[0] = '\0';
            cli->pid = client_pid;

            pthread_create(&cli->thread, NULL, client_thread, cli);
            break;
        }
    }
}

/* ========== Command Processing ========== */

/**
 * compare_timestamp - qsort comparator for ordering commands by arrival time.
 *
 * Ensures deterministic execution order when multiple clients submit
 * commands targeting the same document version.
 */
int compare_timestamp(const void *a, const void *b) {
    Command *ca = (Command *)a;
    Command *cb = (Command *)b;

    if (ca->timestamp < cb->timestamp) return -1;
    if (ca->timestamp > cb->timestamp) return 1;
    return 0;
}

/**
 * handle_command - Parse and execute a single editing command.
 *
 * Dispatches the command string to the appropriate markdown_* function.
 * Checks that the client has "write" permission before applying any edit.
 *
 * Supported commands:
 *   INSERT, DEL, NEWLINE, HEADING, BOLD, ITALIC, ORDERED_LIST,
 *   UNORDERED_LIST, CODE, BLOCKQUOTE, LINK, HORIZONTAL_RULE
 *
 * @cmd: The command to execute (includes raw string and client reference)
 * Returns: 0 on success, negative error code on failure, 1 for unauthorized
 */
int handle_command(Command *cmd) {
    Client *cli = cmd->client;
    char *copy = strdup(cmd->comm);

    char *command = strtok(copy, " ");
    if (!command) {
        free(copy);
        return 1;
    }

    /* Permission check: only "write" role can modify the document */
    if (strcmp(cli->role, "write") != 0) {
        free(copy);
        return 1;
    }

    int rc = 0;

    if (strcmp(command, "INSERT") == 0) {
        char *pos_str = strtok(NULL, " ");
        char *text = NULL;
        if (pos_str) {
            text = pos_str + strlen(pos_str) + 1;
            if (*text == '\0') text = NULL;
        }
        if (pos_str && text) {
            size_t pos = atoi(pos_str);
            rc = markdown_insert(doc, doc->version, pos, text);
        }
    } else if (strcmp(command, "DEL") == 0) {
        char *pos_str = strtok(NULL, " ");
        char *len_str = strtok(NULL, " ");
        if (pos_str && len_str) {
            size_t pos = atoi(pos_str);
            size_t len = atoi(len_str);
            rc = markdown_delete(doc, doc->version, pos, len);
        }
    } else if (strcmp(command, "NEWLINE") == 0) {
        char *pos_str = strtok(NULL, " ");
        if (pos_str) {
            size_t pos = atoi(pos_str);
            rc = markdown_newline(doc, doc->version, pos);
        }
    } else if (strcmp(command, "HEADING") == 0) {
        char *level_str = strtok(NULL, " ");
        char *pos_str = strtok(NULL, " ");
        if (level_str && pos_str) {
            size_t level = atoi(level_str);
            size_t pos = atoi(pos_str);
            rc = markdown_heading(doc, doc->version, level, pos);
        }
    } else if (strcmp(command, "BOLD") == 0) {
        char *start_str = strtok(NULL, " ");
        char *end_str = strtok(NULL, " ");
        if (start_str && end_str) {
            size_t start = atoi(start_str);
            size_t end = atoi(end_str);
            rc = markdown_bold(doc, doc->version, start, end);
        }
    } else if (strcmp(command, "ITALIC") == 0) {
        char *start_str = strtok(NULL, " ");
        char *end_str = strtok(NULL, " ");
        if (start_str && end_str) {
            size_t start = atoi(start_str);
            size_t end = atoi(end_str);
            rc = markdown_italic(doc, doc->version, start, end);
        }
    } else if (strcmp(command, "ORDERED_LIST") == 0) {
        char *pos_str = strtok(NULL, " ");
        if (pos_str) {
            size_t pos = atoi(pos_str);
            rc = markdown_ordered_list(doc, doc->version, pos);
        }
    } else if (strcmp(command, "UNORDERED_LIST") == 0) {
        char *pos_str = strtok(NULL, " ");
        if (pos_str) {
            size_t pos = atoi(pos_str);
            rc = markdown_unordered_list(doc, doc->version, pos);
        }
    } else if (strcmp(command, "CODE") == 0) {
        char *start_str = strtok(NULL, " ");
        char *end_str = strtok(NULL, " ");
        if (start_str && end_str) {
            size_t start = atoi(start_str);
            size_t end = atoi(end_str);
            rc = markdown_code(doc, doc->version, start, end);
        }
    } else if (strcmp(command, "BLOCKQUOTE") == 0) {
        char *start_str = strtok(NULL, " ");
        if (start_str) {
            size_t start = atoi(start_str);
            rc = markdown_blockquote(doc, doc->version, start);
        }
    } else if (strcmp(command, "LINK") == 0) {
        char *start_str = strtok(NULL, " ");
        char *end_str = strtok(NULL, " ");
        char *url = strtok(NULL, "");
        if (start_str && end_str && url) {
            size_t start = atoi(start_str);
            size_t end = atoi(end_str);
            rc = markdown_link(doc, doc->version, start, end, url);
        }
    } else if (strcmp(command, "HORIZONTAL_RULE") == 0) {
        char *pos_str = strtok(NULL, " ");
        if (pos_str) {
            size_t pos = atoi(pos_str);
            rc = markdown_horizontal_rule(doc, doc->version, pos);
        }
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        rc = 1;
    }

    free(copy);
    return rc;
}

/**
 * broadcast_version - Send the latest version log to all connected clients.
 *
 * Format per client:
 *   VERSION <N>\n
 *   EDIT <user> <command> SUCCESS|Reject <reason>\n
 *   ...
 *   END\n
 */
void broadcast_version() {
    int broadcast_ver = version_count - 1;
    CommandLog *log = &global_log[broadcast_ver];

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].connect) continue;

        dprintf(clients[i].s2c, "VERSION %d\n", broadcast_ver);
        for (int j = 0; j < log->entry_count; ++j) {
            dprintf(clients[i].s2c, "%s\n", log->entries[j]);
        }
        dprintf(clients[i].s2c, "END\n");
    }
}

/**
 * collect_and_process_commands - Core synchronization routine.
 *
 * Called periodically by the timer thread. This function:
 *   1. Frees the previous global queue to prevent memory leaks.
 *   2. Drains all per-client command queues into a single merged array.
 *   3. Sorts commands by nanosecond timestamp for deterministic ordering.
 *   4. Executes each command, logging results (SUCCESS or Reject <reason>).
 *   5. Increments the document version and broadcasts the update.
 *
 * If no commands were queued, the function returns without incrementing
 * the version (no empty broadcasts).
 */
void collect_and_process_commands() {
    /* Free previous round's global queue */
    if (global_queue != NULL) {
        for (size_t i = 0; i < global_queue_len; i++) {
            free(global_queue[i].comm);
        }
        free(global_queue);
        global_queue = NULL;
        global_queue_len = 0;
    }

    /* Merge all client queues into a single array */
    size_t cap = 128;
    Command *merge = malloc(sizeof(Command) * cap);
    size_t merge_len = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *cli = &clients[i];
        if (!cli->connect) continue;

        pthread_mutex_lock(&cli->queue_lock);
        for (size_t k = 0; k < cli->queue_len; k++) {
            if (merge_len + 1 >= cap) {
                cap *= 2;
                Command *tmp = realloc(merge, sizeof(Command) * cap);
                if (!tmp) {
                    perror("realloc failed");
                    break;
                }
                merge = tmp;
            }
            merge[merge_len++] = cli->queue[k];
        }
        cli->queue_len = 0;  /* Queue drained */
        pthread_mutex_unlock(&cli->queue_lock);
    }

    /* Sort by timestamp for deterministic global execution order */
    qsort(merge, merge_len, sizeof(Command), compare_timestamp);

    if (merge_len == 0) {
        free(merge);
        return;  /* No commands → no version bump */
    }

    global_queue = merge;
    global_queue_len = merge_len;

    if (version_count >= MAX_LOG_ENTRIES) {
        fprintf(stderr, "ERROR: global_log overflow, version_count=%d\n", version_count);
        return;
    }

    /* Initialize log for this version */
    CommandLog *log = &global_log[version_count];
    log->version = version_count;
    log->entry_count = 0;

    /* Execute each command and record the result */
    for (size_t i = 0; i < merge_len; ++i) {
        Command *cmd = &merge[i];
        int result = handle_command(cmd);

        /* Map return code to human-readable result string */
        char reject_buf[256];
        if (result != 0) {
            if (result == -1) {
                snprintf(reject_buf, sizeof(reject_buf), "Reject INVALID_POSITION\n");
            } else if (result == -2) {
                snprintf(reject_buf, sizeof(reject_buf), "Reject DELETED_POSITION\n");
            } else if (result == -3) {
                snprintf(reject_buf, sizeof(reject_buf), "Reject OUTDATED_VERSION\n");
            } else if (result == 1) {
                if (strcmp(cmd->client->role, "write") != 0) {
                    snprintf(reject_buf, sizeof(reject_buf), "Reject UNAUTHORISED\n");
                }
            }
            write(cmd->client->s2c, reject_buf, strlen(reject_buf));
        } else {
            snprintf(reject_buf, sizeof(reject_buf), "SUCCESS\n");
        }

        /* Build and store log entry: "EDIT <user> <command> <result>" */
        char log_entry[256];
        int written = snprintf(log_entry, sizeof(log_entry), "EDIT %s %s %s",
                               cmd->client->username, cmd->comm, reject_buf);

        if ((size_t)written >= sizeof(log_entry)) {
            fprintf(stderr, "Warning: log_entry truncated. Total length = %d bytes\n", written);
        }

        if (log->entry_count < 50) {
            strncpy(log->entries[log->entry_count], log_entry, sizeof(log->entries[0]) - 1);
            log->entries[log->entry_count][sizeof(log->entries[0]) - 1] = '\0';
            log->entry_count++;
        }
    }

    /* Commit the new version and broadcast to all clients */
    markdown_increment_version(doc);
    version_count++;
    broadcast_version();
}

/* ========== Background Threads ========== */

/**
 * timer_thread - Periodically triggers global command collection and processing.
 *
 * Runs in a dedicated thread. Sleeps for `interval` milliseconds between
 * each synchronization round, ensuring time-based version progression.
 */
void *timer_thread(void *arg) {
    (void)arg;
    while (1) {
        usleep(interval * 1000);  /* Convert ms to us */
        collect_and_process_commands();
    }
    return NULL;
}

/**
 * termi - Handle interactive terminal commands from the server operator.
 *
 * Supported commands:
 *   DOC?  - Print the current document state to stdout
 *   LOG?  - Print the full command log for all versions
 *   QUIT  - Shut down the server (only if no clients are connected)
 *           Saves document to doc.md before exit
 */
void *termi(void *arg) {
    char line[100];
    (void)arg;

    while (fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\n")] = '\0';

        if (strcmp(line, "DOC?") == 0) {
            char *flat = markdown_flatten(doc);
            printf("%s\n", flat);
            free(flat);
        } else if (strcmp(line, "LOG?") == 0) {
            for (int v = 0; v < version_count; ++v) {
                printf("VERSION %d\n", v);
                for (int i = 0; i < global_log[v].entry_count; ++i) {
                    printf("%s", global_log[v].entries[i]);
                }
                printf("END\n");
            }
        } else if (strcmp(line, "QUIT") == 0) {
            int active = 0;
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (clients[i].connect) active++;
            }

            if (active > 0) {
                printf("QUIT rejected, %d clients still connected.\n", active);
            } else {
                /* Save document to doc.md and shut down */
                char *flat = markdown_flatten(doc);
                FILE *fp = fopen("doc.md", "w");
                if (fp) {
                    fwrite(flat, 1, strlen(flat), fp);
                    fclose(fp);
                    printf("Document saved to doc.md\n");
                } else {
                    perror("fopen doc.md");
                }
                free(flat);
                markdown_free(doc);
                printf("Server shutting down.\n");
                exit(0);
            }
        } else {
            printf("Unknown server command: %s\n", line);
        }
    }
    return NULL;
}

/* ========== Entry Point ========== */

/**
 * main - Server entry point.
 *
 * Usage: ./server <TIME_INTERVAL>
 *
 * Initialization sequence:
 *   1. Parse the sync interval from command-line arguments.
 *   2. Print server PID for client connection.
 *   3. Block SIGRTMIN/SIGRTMIN+1 to prevent signal loss during setup.
 *   4. Initialize an empty document.
 *   5. Start the timer thread (periodic sync) and terminal thread (stdin).
 *   6. Enter main loop: wait for SIGRTMIN and spawn client threads.
 */
int main(int argc, char **argv) {
    if (argc != 2) exit(1);

    interval = atoi(argv[1]);
    printf("Server PID: %d\n", getpid());

    /* Block real-time signals before creating any threads to prevent race conditions */
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGRTMIN);
    sigaddset(&signal_mask, SIGRTMIN + 1);
    pthread_sigmask(SIG_BLOCK, &signal_mask, NULL);

    /* Initialize empty document */
    doc = markdown_init();
    if (!doc) {
        printf("init failed\n");
        return 1;
    }
    printf("init success, version = %ld\n", doc->version);

    /* Start background threads */
    pthread_t timer_tid;
    pthread_create(&timer_tid, NULL, timer_thread, NULL);

    pthread_t wait_terminal;
    pthread_create(&wait_terminal, NULL, termi, NULL);

    /* Main loop: accept new client connections via real-time signals */
    while (1) {
        siginfo_t info;
        int signo = sigwaitinfo(&signal_mask, &info);
        if (signo == SIGRTMIN) {
            pid_t client_pid = info.si_pid;
            printf("new one client\n");
            make_client_thread(client_pid);
        }
    }

    markdown_free(doc);
    return 0;
}

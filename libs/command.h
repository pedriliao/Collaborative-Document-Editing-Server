#ifndef COMMAND_H
#define COMMAND_H

/**
 * command.h - Structures for client management, command queuing, and logging.
 *
 * Defines the data types used by the server to:
 *   - Track connected clients and their FIFO communication channels
 *   - Queue incoming commands with nanosecond timestamps for global ordering
 *   - Log executed commands per version for broadcast to all clients
 */

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>

#define MAX_LOG_ENTRIES 50
#define MAX_LOG_ENTRY_LEN 256

/* Forward declaration to resolve circular dependency between Client and Command */
typedef struct Command Command;

/**
 * struct Client - Represents a connected client session.
 *
 * Each client communicates with the server via two named FIFOs:
 *   - c2s (Client-to-Server): Commands sent from client to server
 *   - s2c (Server-to-Client): Responses and broadcasts from server to client
 *
 * @c2s:         File descriptor for the client-to-server FIFO
 * @s2c:         File descriptor for the server-to-client FIFO
 * @thread:      POSIX thread handling this client's command loop
 * @queue:       Dynamic array of pending commands from this client
 * @queue_len:   Number of commands currently in the queue
 * @queue_cap:   Allocated capacity of the command queue
 * @username:    Authenticated username (max 31 chars + null terminator)
 * @connect:     Whether the client is still connected
 * @pid:         Client process ID (used for FIFO naming and signal delivery)
 * @queue_lock:  Mutex protecting concurrent access to the command queue
 * @role:        Permission level ("read" or "write") from roles.txt
 */
typedef struct Client {
    int c2s, s2c;
    pthread_t thread;
    Command *queue;
    size_t queue_len;
    size_t queue_cap;
    char username[32];
    bool connect;
    pid_t pid;
    pthread_mutex_t queue_lock;
    char role[16];
} Client;

/**
 * struct Command - A timestamped editing command from a client.
 *
 * Commands are queued per-client thread and later merged into a global
 * queue sorted by timestamp for deterministic execution order.
 *
 * @comm:       The raw command string (e.g., "INSERT 0 Hello")
 * @client:     Pointer to the originating client
 * @timestamp:  Nanosecond-precision timestamp for global ordering
 */
struct Command {
    char *comm;
    Client *client;
    uint64_t timestamp;
};

/**
 * struct CommandLog - Log of all commands executed in a single version.
 *
 * Used for broadcasting version updates to all connected clients.
 * Each entry contains the formatted "EDIT <user> <command> <result>" string.
 *
 * @version:      The version number this log corresponds to
 * @entries:      Array of log entry strings
 * @entry_count:  Number of entries recorded
 */
typedef struct {
    uint64_t version;
    char entries[MAX_LOG_ENTRIES][MAX_LOG_ENTRY_LEN];
    int entry_count;
} CommandLog;

#endif /* COMMAND_H */

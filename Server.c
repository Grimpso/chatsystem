/*
 * ============================================================
 *  Multi-Threaded TCP Chat Server
 *  Author  : Siddhant Mishra
 *  File    : server.c
 *
 *  Build (Windows - MinGW):
 *      gcc server.c -o server.exe -lws2_32
 *
 *  Build (Linux):
 *      gcc server.c -o server -lpthread
 *
 *  Run:
 *      Windows : server.exe
 *      Linux   : ./server
 * ============================================================
 */

/* -------- Platform Detection -------- */
#ifdef _WIN32
    #define PLATFORM_WINDOWS
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif

    typedef SOCKET      sock_t;
    typedef HANDLE      thread_t;

    #define INVALID_SOCK    INVALID_SOCKET
    #define CLOSE_SOCK(s)   closesocket(s)
    #define SOCK_ERR        SOCKET_ERROR

    /* Windows thread wrapper */
    #define THREAD_FUNC     DWORD WINAPI
    #define THREAD_RET      return 0

    static thread_t create_thread(LPTHREAD_START_ROUTINE fn, void *arg) {
        return CreateThread(NULL, 0, fn, arg, 0, NULL);
    }

#else
    #define PLATFORM_LINUX
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <pthread.h>

    typedef int         sock_t;
    typedef pthread_t   thread_t;

    #define INVALID_SOCK    (-1)
    #define CLOSE_SOCK(s)   close(s)
    #define SOCK_ERR        (-1)

    #define THREAD_FUNC     void*
    #define THREAD_RET      return NULL

    static thread_t create_thread(void *(*fn)(void *), void *arg) {
        thread_t tid;
        pthread_create(&tid, NULL, fn, arg);
        pthread_detach(tid);
        return tid;
    }
#endif

/* -------- Common Headers -------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------- Configuration -------- */
#define PORT            8080
#define MAX_CLIENTS     10
#define BUFFER_SIZE     1024
#define NAME_SIZE       32

/* -------- Message Frame Structure --------
 *  Every message sent over the wire uses this custom frame:
 *
 *  [ HEADER (4 bytes) ][ NAME (32 bytes) ][ DATA (up to 988 bytes) ]
 *   Magic: 0xABCD        sender username    actual chat text
 *
 *  This simulates a real embedded/protocol framing approach.
 * ------------------------------------------*/
#define MAGIC_HEADER    0xABCD

#pragma pack(push, 1)   /* Ensure no padding — important for wire protocol */
typedef struct {
    unsigned short  magic;              /* 0xABCD — frame start marker      */
    unsigned short  data_len;           /* length of the 'data' field only   */
    char            name[NAME_SIZE];    /* sender's username                 */
    char            data[BUFFER_SIZE];  /* actual message text               */
} msg_frame_t;
#pragma pack(pop)

/* -------- Client Slot -------- */
typedef struct {
    sock_t  fd;
    char    name[NAME_SIZE];
    int     active;
} client_t;

/* -------- Global State -------- */
static client_t clients[MAX_CLIENTS];

#ifdef PLATFORM_WINDOWS
static CRITICAL_SECTION clients_lock;
#else
static pthread_mutex_t  clients_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

/* -------- Mutex Helpers -------- */
static void lock()   {
#ifdef PLATFORM_WINDOWS
    EnterCriticalSection(&clients_lock);
#else
    pthread_mutex_lock(&clients_lock);
#endif
}
static void unlock() {
#ifdef PLATFORM_WINDOWS
    LeaveCriticalSection(&clients_lock);
#else
    pthread_mutex_unlock(&clients_lock);
#endif
}

/* -------- Utility: Send raw bytes reliably -------- */
static int send_all(sock_t fd, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

/* -------- Utility: Receive raw bytes reliably -------- */
static int recv_all(sock_t fd, char *buf, int len) {
    int received = 0;
    while (received < len) {
        int n = recv(fd, buf + received, len - received, 0);
        if (n <= 0) return -1;
        received += n;
    }
    return received;
}

/* -------- Broadcast a frame to all clients except 'skip_fd' -------- */
static void broadcast(msg_frame_t *frame, sock_t skip_fd) {
    int total = sizeof(msg_frame_t);
    lock();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].fd != skip_fd) {
            if (send_all(clients[i].fd, (char *)frame, total) < 0) {
                printf("[Server] Failed to send to client %d\n", i);
            }
        }
    }
    unlock();
}

/* -------- Add client to the table -------- */
static int add_client(sock_t fd, const char *name) {
    lock();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            clients[i].fd     = fd;
            clients[i].active = 1;
            strncpy(clients[i].name, name, NAME_SIZE - 1);
            unlock();
            return i;
        }
    }
    unlock();
    return -1;   /* Server full */
}

/* -------- Remove client from the table -------- */
static void remove_client(sock_t fd) {
    lock();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].fd == fd) {
            clients[i].active = 0;
            memset(clients[i].name, 0, NAME_SIZE);
            break;
        }
    }
    unlock();
}

/* -------- Per-client Thread Handler -------- */
THREAD_FUNC client_handler(void *arg) {
    sock_t  fd   = (sock_t)(size_t)arg;
    msg_frame_t  frame;
    char    name[NAME_SIZE] = {0};

    /* Step 1: First message from client must be its name */
    int r = recv_all(fd, (char *)&frame, sizeof(frame));
    if (r <= 0 || frame.magic != MAGIC_HEADER) {
        printf("[Server] Handshake failed. Dropping client.\n");
        CLOSE_SOCK(fd);
        THREAD_RET;
    }
    strncpy(name, frame.name, NAME_SIZE - 1);

    /* Step 2: Register client */
    int slot = add_client(fd, name);
    if (slot < 0) {
        printf("[Server] Server full. Rejecting %s\n", name);
        CLOSE_SOCK(fd);
        THREAD_RET;
    }
    printf("[Server] '%s' joined. (slot %d)\n", name, slot);

    /* Step 3: Announce join to all other clients */
    msg_frame_t announce;
    memset(&announce, 0, sizeof(announce));
    announce.magic = MAGIC_HEADER;
    snprintf(announce.name, NAME_SIZE, "SERVER");
    snprintf(announce.data, BUFFER_SIZE, "*** %s has joined the chat ***", name);
    announce.data_len = (unsigned short)strlen(announce.data);
    broadcast(&announce, fd);

    /* Step 4: Message loop */
    while (1) {
        memset(&frame, 0, sizeof(frame));
        r = recv_all(fd, (char *)&frame, sizeof(frame));

        if (r <= 0) {
            printf("[Server] '%s' disconnected.\n", name);
            break;
        }

        /* Validate frame magic */
        if (frame.magic != MAGIC_HEADER) {
            printf("[Server] Bad frame from '%s'. Ignoring.\n", name);
            continue;
        }

        /* Null-terminate safely */
        frame.data[BUFFER_SIZE - 1] = '\0';

        printf("[%s]: %s\n", name, frame.data);

        /* Check for quit command */
        if (strncmp(frame.data, "/quit", 5) == 0) break;

        /* Broadcast to others */
        broadcast(&frame, fd);
    }

    /* Step 5: Cleanup */
    remove_client(fd);
    CLOSE_SOCK(fd);

    /* Announce leave */
    memset(&announce, 0, sizeof(announce));
    announce.magic = MAGIC_HEADER;
    snprintf(announce.name, NAME_SIZE, "SERVER");
    snprintf(announce.data, BUFFER_SIZE, "*** %s has left the chat ***", name);
    announce.data_len = (unsigned short)strlen(announce.data);
    broadcast(&announce, INVALID_SOCK);

    THREAD_RET;
}

/* -------- Main -------- */
int main(void) {

#ifdef PLATFORM_WINDOWS
    /* Initialize Winsock */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[Server] WSAStartup failed.\n");
        return 1;
    }
    InitializeCriticalSection(&clients_lock);
#endif

    /* Zero out client table */
    memset(clients, 0, sizeof(clients));

    /* Create TCP socket */
    sock_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCK) {
        printf("[Server] socket() failed.\n");
        return 1;
    }

    /* Allow port reuse */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));

    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCK_ERR) {
        printf("[Server] bind() failed.\n");
        CLOSE_SOCK(server_fd);
        return 1;
    }

    /* Listen */
    if (listen(server_fd, MAX_CLIENTS) == SOCK_ERR) {
        printf("[Server] listen() failed.\n");
        CLOSE_SOCK(server_fd);
        return 1;
    }

    printf("============================================\n");
    printf("  Multi-Threaded TCP Chat Server\n");
    printf("  Listening on port %d | Max clients: %d\n", PORT, MAX_CLIENTS);
    printf("============================================\n");

    /* Accept loop */
    while (1) {
        struct sockaddr_in client_addr;
#ifdef PLATFORM_WINDOWS
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif
        sock_t client_fd = accept(server_fd,
                                  (struct sockaddr *)&client_addr,
                                  &addr_len);
        if (client_fd == INVALID_SOCK) {
            printf("[Server] accept() failed. Continuing...\n");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        printf("[Server] New connection from %s\n", ip);

        /* Spawn a thread for this client */
        create_thread(client_handler, (void *)(size_t)client_fd);
    }

    CLOSE_SOCK(server_fd);

#ifdef PLATFORM_WINDOWS
    DeleteCriticalSection(&clients_lock);
    WSACleanup();
#endif

    return 0;
}

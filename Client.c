/*
 * ============================================================
 *  Multi-Threaded TCP Chat Client
 *  Author  : Siddhant Mishra
 *  File    : client.c
 *
 *  Build (Windows - MinGW):
 *      gcc client.c -o client.exe -lws2_32
 *
 *  Build (Linux):
 *      gcc client.c -o client -lpthread
 *
 *  Run:
 *      Windows : client.exe
 *      Linux   : ./client
 *
 *  Commands:
 *      /quit    → disconnect and exit
 *      /list    → server will show active users (future feature)
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
#define SERVER_IP       "127.0.0.1"
#define PORT            8080
#define BUFFER_SIZE     1024
#define NAME_SIZE       32

/* -------- Message Frame (must match server.c) -------- */
#define MAGIC_HEADER    0xABCD

#pragma pack(push, 1)
typedef struct {
    unsigned short  magic;
    unsigned short  data_len;
    char            name[NAME_SIZE];
    char            data[BUFFER_SIZE];
} msg_frame_t;
#pragma pack(pop)

/* -------- Globals -------- */
static sock_t   g_sock;
static char     g_name[NAME_SIZE];
static int      g_running = 1;

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

/* -------- Build and send a frame -------- */
static int send_frame(sock_t fd, const char *name, const char *data) {
    msg_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    frame.magic    = MAGIC_HEADER;
    frame.data_len = (unsigned short)strlen(data);
    strncpy(frame.name, name, NAME_SIZE - 1);
    strncpy(frame.data, data, BUFFER_SIZE - 1);

    return send_all(fd, (char *)&frame, sizeof(frame));
}

/* -------- Receive Thread: prints incoming messages -------- */
THREAD_FUNC recv_thread(void *arg) {
    (void)arg;
    msg_frame_t frame;

    while (g_running) {
        memset(&frame, 0, sizeof(frame));
        int r = recv_all(g_sock, (char *)&frame, sizeof(frame));

        if (r <= 0) {
            if (g_running) {
                printf("\n[!] Disconnected from server.\n");
            }
            g_running = 0;
            break;
        }

        if (frame.magic != MAGIC_HEADER) continue;  /* Bad frame, skip */

        frame.data[BUFFER_SIZE - 1] = '\0';
        frame.name[NAME_SIZE   - 1] = '\0';

        /* Print message with sender label */
        printf("\r[%-12s]: %s\n> ", frame.name, frame.data);
        fflush(stdout);
    }

    THREAD_RET;
}

/* -------- Main -------- */
int main(void) {

#ifdef PLATFORM_WINDOWS
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[Client] WSAStartup failed.\n");
        return 1;
    }
#endif

    /* Get username */
    printf("============================================\n");
    printf("  Multi-Threaded TCP Chat Client\n");
    printf("  Connecting to %s:%d\n", SERVER_IP, PORT);
    printf("  Type /quit to exit\n");
    printf("============================================\n");
    printf("Enter your name: ");
    fflush(stdout);
    fgets(g_name, NAME_SIZE, stdin);
    g_name[strcspn(g_name, "\n")] = '\0';   /* Strip newline */

    if (strlen(g_name) == 0) {
        printf("[Client] Name cannot be empty.\n");
        return 1;
    }

    /* Create TCP socket */
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock == INVALID_SOCK) {
        printf("[Client] socket() failed.\n");
        return 1;
    }

    /* Connect to server */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(PORT);

    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        printf("[Client] Invalid server IP.\n");
        return 1;
    }

    if (connect(g_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCK_ERR) {
        printf("[Client] connect() failed. Is the server running?\n");
        CLOSE_SOCK(g_sock);
        return 1;
    }

    printf("[Client] Connected! Welcome, %s!\n\n", g_name);

    /* Handshake: send our name to the server as first frame */
    if (send_frame(g_sock, g_name, "HANDSHAKE") < 0) {
        printf("[Client] Handshake failed.\n");
        CLOSE_SOCK(g_sock);
        return 1;
    }

    /* Start receive thread */
    create_thread(recv_thread, NULL);

    /* Main send loop */
    char input[BUFFER_SIZE];
    while (g_running) {
        printf("> ");
        fflush(stdout);

        if (!fgets(input, BUFFER_SIZE, stdin)) break;
        input[strcspn(input, "\n")] = '\0';   /* Strip newline */

        if (strlen(input) == 0) continue;     /* Skip empty lines */

        /* Send frame */
        if (send_frame(g_sock, g_name, input) < 0) {
            printf("[Client] Send failed. Connection lost.\n");
            break;
        }

        /* Handle quit */
        if (strncmp(input, "/quit", 5) == 0) {
            printf("[Client] Disconnecting...\n");
            g_running = 0;
            break;
        }
    }

    g_running = 0;
    CLOSE_SOCK(g_sock);

#ifdef PLATFORM_WINDOWS
    WSACleanup();
#endif

    printf("[Client] Goodbye, %s!\n", g_name);
    return 0;
}

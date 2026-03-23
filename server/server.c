#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../protocolo.h"

#define MAX_CLIENTES 100
#define BACKLOG 16

typedef struct {
    char username[32];
    char ip[INET_ADDRSTRLEN];
    char status[16];
    int sockfd;
    int activo;
    time_t ultimo_mensaje;
} Cliente;

static Cliente lista[MAX_CLIENTES];
static int num_clientes = 0;
static int server_fd = -1;
static volatile sig_atomic_t server_running = 1;
static pthread_mutex_t mutex_lista = PTHREAD_MUTEX_INITIALIZER;

static ssize_t send_all(int fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    size_t total = 0;

    while (total < len) {
        ssize_t sent = send(fd, ptr + total, len - total, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            return -1;
        }
        total += (size_t)sent;
    }

    return (ssize_t)total;
}

static ssize_t recv_all(int fd, void *buf, size_t len) {
    char *ptr = (char *)buf;
    size_t total = 0;

    while (total < len) {
        ssize_t recvd = recv(fd, ptr + total, len - total, 0);
        if (recvd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (recvd == 0) {
            return 0;
        }
        total += (size_t)recvd;
    }

    return (ssize_t)total;
}

static void fill_packet(ChatPacket *pkt, uint8_t cmd, const char *sender, const char *target, const char *payload) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->command = cmd;

    if (sender != NULL) {
        strncpy(pkt->sender, sender, sizeof(pkt->sender) - 1);
    }
    if (target != NULL) {
        strncpy(pkt->target, target, sizeof(pkt->target) - 1);
    }
    if (payload != NULL) {
        strncpy(pkt->payload, payload, sizeof(pkt->payload) - 1);
        pkt->payload_len = (uint16_t)strnlen(pkt->payload, sizeof(pkt->payload));
    }
}

static int send_packet(int fd, const ChatPacket *pkt) {
    return (send_all(fd, pkt, sizeof(*pkt)) == (ssize_t)sizeof(*pkt)) ? 0 : -1;
}

static int status_valido(const char *status) {
    return strcmp(status, STATUS_ACTIVO) == 0 || strcmp(status, STATUS_OCUPADO) == 0 || strcmp(status, STATUS_INACTIVO) == 0;
}

static int buscar_cliente_index_por_username(const char *username) {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo && strcmp(lista[i].username, username) == 0) {
            return i;
        }
    }
    return -1;
}

static int buscar_cliente_index_por_sockfd(int sockfd) {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo && lista[i].sockfd == sockfd) {
            return i;
        }
    }
    return -1;
}

static int username_o_ip_existe(const char *username, const char *ip) {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (!lista[i].activo) {
            continue;
        }
        if (strcmp(lista[i].username, username) == 0 || strcmp(lista[i].ip, ip) == 0) {
            return 1;
        }
    }
    return 0;
}

static void enviar_error(int sockfd, const char *target_user, const char *mensaje) {
    ChatPacket pkt;
    fill_packet(&pkt, CMD_ERROR, "SERVER", target_user, mensaje);
    send_packet(sockfd, &pkt);
}

static void enviar_ok(int sockfd, const char *target_user, const char *mensaje) {
    ChatPacket pkt;
    fill_packet(&pkt, CMD_OK, "SERVER", target_user, mensaje);
    send_packet(sockfd, &pkt);
}

static void broadcast_packet(const ChatPacket *pkt) {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (!lista[i].activo) {
            continue;
        }
        send_packet(lista[i].sockfd, pkt);
    }
}

static void notificar_desconexion(const char *username) {
    ChatPacket pkt;
    fill_packet(&pkt, CMD_DISCONNECTED, "SERVER", "ALL", username);

    pthread_mutex_lock(&mutex_lista);
    broadcast_packet(&pkt);
    pthread_mutex_unlock(&mutex_lista);
}

static void remover_cliente_por_sockfd(int sockfd, int notificar) {
    char username[32] = {0};

    pthread_mutex_lock(&mutex_lista);
    int idx = buscar_cliente_index_por_sockfd(sockfd);
    if (idx >= 0) {
        strncpy(username, lista[idx].username, sizeof(username) - 1);
        lista[idx].activo = 0;
        num_clientes--;
    }
    pthread_mutex_unlock(&mutex_lista);

    if (notificar && username[0] != '\0') {
        notificar_desconexion(username);
    }
}

static void manejar_cmd_broadcast(const ChatPacket *in_pkt, int sockfd) {
    ChatPacket out;
    fill_packet(&out, CMD_MSG, in_pkt->sender, "ALL", in_pkt->payload);

    pthread_mutex_lock(&mutex_lista);
    int idx = buscar_cliente_index_por_sockfd(sockfd);
    if (idx >= 0) {
        lista[idx].ultimo_mensaje = time(NULL);
        if (strcmp(lista[idx].status, STATUS_INACTIVO) == 0) {
            strncpy(lista[idx].status, STATUS_ACTIVO, sizeof(lista[idx].status) - 1);
        }
    }

    broadcast_packet(&out);
    pthread_mutex_unlock(&mutex_lista);
}

static void manejar_cmd_direct(const ChatPacket *in_pkt, int sockfd) {
    ChatPacket out;
    int destino_sockfd = -1;

    pthread_mutex_lock(&mutex_lista);
    int idx_emisor = buscar_cliente_index_por_sockfd(sockfd);
    if (idx_emisor >= 0) {
        lista[idx_emisor].ultimo_mensaje = time(NULL);
        if (strcmp(lista[idx_emisor].status, STATUS_INACTIVO) == 0) {
            strncpy(lista[idx_emisor].status, STATUS_ACTIVO, sizeof(lista[idx_emisor].status) - 1);
        }
    }

    int idx_destino = buscar_cliente_index_por_username(in_pkt->target);
    if (idx_destino >= 0) {
        destino_sockfd = lista[idx_destino].sockfd;
    }
    pthread_mutex_unlock(&mutex_lista);

    if (destino_sockfd < 0) {
        enviar_error(sockfd, in_pkt->sender, "Destinatario no conectado");
        return;
    }

    fill_packet(&out, CMD_MSG, in_pkt->sender, in_pkt->target, in_pkt->payload);
    send_packet(destino_sockfd, &out);
}

static void manejar_cmd_list(const ChatPacket *in_pkt, int sockfd) {
    char buffer[957];
    buffer[0] = '\0';

    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (!lista[i].activo) {
            continue;
        }

        char item[128];
        snprintf(item, sizeof(item), "%s,%s;", lista[i].username, lista[i].status);

        if (strlen(buffer) + strlen(item) < sizeof(buffer)) {
            strcat(buffer, item);
        } else {
            break;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    ChatPacket out;
    fill_packet(&out, CMD_USER_LIST, "SERVER", in_pkt->sender, buffer);
    send_packet(sockfd, &out);
}

static void manejar_cmd_info(const ChatPacket *in_pkt, int sockfd) {
    char info[957];
    int encontrado = 0;

    pthread_mutex_lock(&mutex_lista);
    int idx = buscar_cliente_index_por_username(in_pkt->target);
    if (idx >= 0) {
        snprintf(info, sizeof(info), "%s,%s", lista[idx].ip, lista[idx].status);
        encontrado = 1;
    }
    pthread_mutex_unlock(&mutex_lista);

    if (!encontrado) {
        enviar_error(sockfd, in_pkt->sender, "Usuario no conectado");
        return;
    }

    ChatPacket out;
    fill_packet(&out, CMD_USER_INFO, "SERVER", in_pkt->sender, info);
    send_packet(sockfd, &out);
}

static void manejar_cmd_status(const ChatPacket *in_pkt, int sockfd) {
    if (!status_valido(in_pkt->payload)) {
        enviar_error(sockfd, in_pkt->sender, "Status invalido. Use ACTIVE|BUSY|INACTIVE");
        return;
    }

    pthread_mutex_lock(&mutex_lista);
    int idx = buscar_cliente_index_por_sockfd(sockfd);
    if (idx >= 0) {
        strncpy(lista[idx].status, in_pkt->payload, sizeof(lista[idx].status) - 1);
        lista[idx].ultimo_mensaje = time(NULL);
    }
    pthread_mutex_unlock(&mutex_lista);

    enviar_ok(sockfd, in_pkt->sender, in_pkt->payload);
}

static void *monitor_inactividad(void *arg) {
    (void)arg;

    while (server_running) {
        sleep(1);

        time_t ahora = time(NULL);

        pthread_mutex_lock(&mutex_lista);
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (!lista[i].activo) {
                continue;
            }

            if (strcmp(lista[i].status, STATUS_INACTIVO) == 0) {
                continue;
            }

            if (ahora - lista[i].ultimo_mensaje >= INACTIVITY_TIMEOUT) {
                strncpy(lista[i].status, STATUS_INACTIVO, sizeof(lista[i].status) - 1);

                ChatPacket aviso;
                fill_packet(&aviso, CMD_MSG, "SERVER", lista[i].username, "Tu status cambio a INACTIVE");
                send_packet(lista[i].sockfd, &aviso);
            }
        }
        pthread_mutex_unlock(&mutex_lista);
    }

    return NULL;
}

typedef struct {
    int sockfd;
    struct sockaddr_in addr;
} ThreadArgs;

static void cerrar_cliente(ThreadArgs *args, int notificar) {
    remover_cliente_por_sockfd(args->sockfd, notificar);
    close(args->sockfd);
    free(args);
}

static void *cliente_thread(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;
    int sockfd = args->sockfd;

    char ip_cliente[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &args->addr.sin_addr, ip_cliente, sizeof(ip_cliente));

    ChatPacket pkt;
    ssize_t n = recv_all(sockfd, &pkt, sizeof(pkt));
    if (n <= 0 || pkt.command != CMD_REGISTER) {
        enviar_error(sockfd, "", "Registro invalido");
        cerrar_cliente(args, 0);
        return NULL;
    }

    if (pkt.sender[0] == '\0') {
        enviar_error(sockfd, "", "Nombre de usuario vacio");
        cerrar_cliente(args, 0);
        return NULL;
    }

    pthread_mutex_lock(&mutex_lista);
    if (num_clientes >= MAX_CLIENTES) {
        pthread_mutex_unlock(&mutex_lista);
        enviar_error(sockfd, pkt.sender, "Servidor lleno");
        cerrar_cliente(args, 0);
        return NULL;
    }

    if (username_o_ip_existe(pkt.sender, ip_cliente)) {
        pthread_mutex_unlock(&mutex_lista);
        enviar_error(sockfd, pkt.sender, "Usuario o IP ya registrados");
        cerrar_cliente(args, 0);
        return NULL;
    }

    int idx_libre = -1;
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (!lista[i].activo) {
            idx_libre = i;
            break;
        }
    }

    if (idx_libre < 0) {
        pthread_mutex_unlock(&mutex_lista);
        enviar_error(sockfd, pkt.sender, "Servidor lleno");
        cerrar_cliente(args, 0);
        return NULL;
    }

    memset(&lista[idx_libre], 0, sizeof(lista[idx_libre]));
    strncpy(lista[idx_libre].username, pkt.sender, sizeof(lista[idx_libre].username) - 1);
    strncpy(lista[idx_libre].ip, ip_cliente, sizeof(lista[idx_libre].ip) - 1);
    strncpy(lista[idx_libre].status, STATUS_ACTIVO, sizeof(lista[idx_libre].status) - 1);
    lista[idx_libre].sockfd = sockfd;
    lista[idx_libre].activo = 1;
    lista[idx_libre].ultimo_mensaje = time(NULL);
    num_clientes++;
    pthread_mutex_unlock(&mutex_lista);

    char bienvenida[128];
    snprintf(bienvenida, sizeof(bienvenida), "Bienvenido %s", pkt.sender);
    enviar_ok(sockfd, pkt.sender, bienvenida);

    while (server_running) {
        memset(&pkt, 0, sizeof(pkt));
        n = recv_all(sockfd, &pkt, sizeof(pkt));
        if (n <= 0) {
            cerrar_cliente(args, 1);
            return NULL;
        }

        switch (pkt.command) {
            case CMD_BROADCAST:
                manejar_cmd_broadcast(&pkt, sockfd);
                break;
            case CMD_DIRECT:
                manejar_cmd_direct(&pkt, sockfd);
                break;
            case CMD_LIST:
                manejar_cmd_list(&pkt, sockfd);
                break;
            case CMD_INFO:
                manejar_cmd_info(&pkt, sockfd);
                break;
            case CMD_STATUS:
                manejar_cmd_status(&pkt, sockfd);
                break;
            case CMD_LOGOUT:
                enviar_ok(sockfd, pkt.sender, "Logout exitoso");
                cerrar_cliente(args, 1);
                return NULL;
            default:
                enviar_error(sockfd, pkt.sender, "Comando no reconocido");
                break;
        }
    }

    cerrar_cliente(args, 1);
    return NULL;
}

static void handle_sigint(int signum) {
    (void)signum;
    server_running = 0;
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <puerto>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Puerto invalido\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_sigint);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Servidor escuchando en puerto %d...\n", port);

    pthread_t monitor_thread;
    if (pthread_create(&monitor_thread, NULL, monitor_inactividad, NULL) != 0) {
        perror("pthread_create monitor");
        close(server_fd);
        return EXIT_FAILURE;
    }

    while (server_running) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0) {
            if (!server_running) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        ThreadArgs *args = (ThreadArgs *)malloc(sizeof(ThreadArgs));
        if (args == NULL) {
            fprintf(stderr, "Error de memoria\n");
            close(client_fd);
            continue;
        }

        args->sockfd = client_fd;
        args->addr = cli_addr;

        pthread_t tid;
        if (pthread_create(&tid, NULL, cliente_thread, args) != 0) {
            perror("pthread_create cliente");
            close(client_fd);
            free(args);
            continue;
        }

        pthread_detach(tid);
    }

    server_running = 0;
    pthread_join(monitor_thread, NULL);

    pthread_mutex_lock(&mutex_lista);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo) {
            close(lista[i].sockfd);
            lista[i].activo = 0;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    printf("Servidor detenido.\n");
    return EXIT_SUCCESS;
}

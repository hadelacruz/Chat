#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../protocolo.h"

static volatile sig_atomic_t running = 1;
static int sockfd_global = -1;
static char username_global[32] = {0};

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

static int send_packet(int fd, const ChatPacket *pkt) {
    return (send_all(fd, pkt, sizeof(*pkt)) == (ssize_t)sizeof(*pkt)) ? 0 : -1;
}

static int recv_packet(int fd, ChatPacket *pkt) {
    memset(pkt, 0, sizeof(*pkt));
    ssize_t n = recv_all(fd, pkt, sizeof(*pkt));
    if (n == 0) {
        return 0;
    }
    if (n < 0) {
        return -1;
    }
    return 1;
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

static void print_help(void) {
    printf("\nComandos disponibles:\n");
    printf("  /broadcast <mensaje>\n");
    printf("  /msg <usuario> <mensaje>\n");
    printf("  /status <ACTIVE|BUSY|INACTIVE>\n");
    printf("  /list\n");
    printf("  /info <usuario>\n");
    printf("  /help\n");
    printf("  /exit\n\n");
}

static void handle_sigint(int signum) {
    (void)signum;
    running = 0;
    if (sockfd_global >= 0) {
        ChatPacket pkt;
        fill_packet(&pkt, CMD_LOGOUT, username_global, NULL, NULL);
        send_packet(sockfd_global, &pkt);
        shutdown(sockfd_global, SHUT_RDWR);
    }
}

static void *receiver_thread(void *arg) {
    int sockfd = *(int *)arg;

    while (running) {
        ChatPacket pkt;
        int rc = recv_packet(sockfd, &pkt);
        if (rc <= 0) {
            printf("\n[INFO] Conexión cerrada por el servidor.\n");
            running = 0;
            break;
        }

        switch (pkt.command) {
            case CMD_MSG:
                if (strcmp(pkt.target, "ALL") == 0) {
                    printf("\n[GENERAL][%s]: %s\n", pkt.sender, pkt.payload);
                } else {
                    printf("\n[PRIVADO][%s -> %s]: %s\n", pkt.sender, pkt.target, pkt.payload);
                }
                break;
            case CMD_OK:
                printf("\n[OK] %s\n", pkt.payload);
                break;
            case CMD_ERROR:
                printf("\n[ERROR] %s\n", pkt.payload);
                break;
            case CMD_USER_LIST:
                printf("\n[USUARIOS] %s\n", pkt.payload[0] ? pkt.payload : "(sin usuarios)");
                break;
            case CMD_USER_INFO:
                printf("\n[INFO USUARIO] %s\n", pkt.payload);
                break;
            case CMD_DISCONNECTED:
                printf("\n[INFO] Usuario desconectado: %s\n", pkt.payload);
                break;
            default:
                printf("\n[INFO] Comando recibido no reconocido: %u\n", pkt.command);
                break;
        }

        fflush(stdout);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <username> <IP_servidor> <puerto>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *username = argv[1];
    const char *ip_server = argv[2];
    int port = atoi(argv[3]);

    if (strlen(username) >= sizeof(username_global)) {
        fprintf(stderr, "Username demasiado largo (max 31)\n");
        return EXIT_FAILURE;
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Puerto invalido\n");
        return EXIT_FAILURE;
    }

    strncpy(username_global, username, sizeof(username_global) - 1);
    signal(SIGINT, handle_sigint);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    sockfd_global = sockfd;

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip_server, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "IP de servidor invalida\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    ChatPacket reg_pkt;
    fill_packet(&reg_pkt, CMD_REGISTER, username, NULL, username);
    if (send_packet(sockfd, &reg_pkt) != 0) {
        fprintf(stderr, "No se pudo enviar registro\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    ChatPacket resp;
    int reg_rc = recv_packet(sockfd, &resp);
    if (reg_rc <= 0) {
        fprintf(stderr, "No hubo respuesta de registro\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (resp.command != CMD_OK) {
        fprintf(stderr, "Registro rechazado: %s\n", resp.payload);
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("%s\n", resp.payload);
    print_help();

    pthread_t recv_tid;
    if (pthread_create(&recv_tid, NULL, receiver_thread, &sockfd) != 0) {
        perror("pthread_create");
        close(sockfd);
        return EXIT_FAILURE;
    }

    char line[1200];
    while (running) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        if (strcmp(line, "/help") == 0) {
            print_help();
            continue;
        }

        if (strcmp(line, "/exit") == 0) {
            ChatPacket pkt;
            fill_packet(&pkt, CMD_LOGOUT, username, NULL, NULL);
            send_packet(sockfd, &pkt);
            running = 0;
            break;
        }

        if (strncmp(line, "/broadcast ", 11) == 0) {
            const char *msg = line + 11;
            if (*msg == '\0') {
                printf("Uso: /broadcast <mensaje>\n");
                continue;
            }

            ChatPacket pkt;
            fill_packet(&pkt, CMD_BROADCAST, username, NULL, msg);
            if (send_packet(sockfd, &pkt) != 0) {
                printf("Error enviando broadcast\n");
                running = 0;
                break;
            }
            continue;
        }

        if (strncmp(line, "/msg ", 5) == 0) {
            char *cursor = line + 5;
            while (*cursor == ' ') {
                cursor++;
            }

            char *dest = strtok(cursor, " ");
            char *msg = strtok(NULL, "");
            if (dest == NULL || msg == NULL || *msg == '\0') {
                printf("Uso: /msg <usuario> <mensaje>\n");
                continue;
            }

            ChatPacket pkt;
            fill_packet(&pkt, CMD_DIRECT, username, dest, msg);
            if (send_packet(sockfd, &pkt) != 0) {
                printf("Error enviando mensaje directo\n");
                running = 0;
                break;
            }
            continue;
        }

        if (strncmp(line, "/status ", 8) == 0) {
            const char *st = line + 8;
            if (!(strcmp(st, STATUS_ACTIVO) == 0 || strcmp(st, STATUS_OCUPADO) == 0 || strcmp(st, STATUS_INACTIVO) == 0)) {
                printf("Status invalido. Use ACTIVE|BUSY|INACTIVE\n");
                continue;
            }

            ChatPacket pkt;
            fill_packet(&pkt, CMD_STATUS, username, NULL, st);
            if (send_packet(sockfd, &pkt) != 0) {
                printf("Error enviando status\n");
                running = 0;
                break;
            }
            continue;
        }

        if (strcmp(line, "/list") == 0) {
            ChatPacket pkt;
            fill_packet(&pkt, CMD_LIST, username, NULL, NULL);
            if (send_packet(sockfd, &pkt) != 0) {
                printf("Error enviando solicitud de lista\n");
                running = 0;
                break;
            }
            continue;
        }

        if (strncmp(line, "/info ", 6) == 0) {
            const char *user = line + 6;
            if (*user == '\0') {
                printf("Uso: /info <usuario>\n");
                continue;
            }

            ChatPacket pkt;
            fill_packet(&pkt, CMD_INFO, username, user, NULL);
            if (send_packet(sockfd, &pkt) != 0) {
                printf("Error enviando solicitud de info\n");
                running = 0;
                break;
            }
            continue;
        }

        printf("Comando no reconocido. Use /help\n");
    }

    running = 0;
    shutdown(sockfd, SHUT_RDWR);
    pthread_join(recv_tid, NULL);
    close(sockfd);

    return EXIT_SUCCESS;
}

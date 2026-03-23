# Protocolo de Comunicación — Chat

---

# 1. Ejecución

## Servidor

```bash
./servidor <puerto>
```

## Cliente

```bash
./cliente <username> <IP_servidor> <puerto>
```

---

# 2. Struct — `protocolo.h`

Copiar este archivo exactamente igual en todos los grupos.  
**No modificar orden ni tamaños.**

```c
#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <stdint.h>

/* ── Comandos Cliente → Servidor ── */

#define CMD_REGISTER    1
#define CMD_BROADCAST   2
#define CMD_DIRECT      3
#define CMD_LIST        4
#define CMD_INFO        5
#define CMD_STATUS      6
#define CMD_LOGOUT      7

/* ── Respuestas Servidor → Cliente ── */

#define CMD_OK          8
#define CMD_ERROR       9
#define CMD_MSG         10
#define CMD_USER_LIST   11
#define CMD_USER_INFO   12
#define CMD_DISCONNECTED 13

/* ── Valores de status ── */

#define STATUS_ACTIVO   "ACTIVE"
#define STATUS_OCUPADO  "BUSY"
#define STATUS_INACTIVO "INACTIVE"

/* ── Timeouts ── */

#define INACTIVITY_TIMEOUT 60

/* ── Struct principal: siempre 1024 bytes ── */

typedef struct {

    uint8_t  command;
    uint16_t payload_len;

    char sender[32];
    char target[32];

    char payload[957];

} __attribute__((packed)) ChatPacket;

/* TOTAL: 1 + 2 + 32 + 32 + 957 = 1024 bytes */

#endif
```

---

# ¿Por qué 1024 bytes?

- Potencia de 2 → alineación eficiente en memoria
- Cabe en un segmento TCP (MTU ≈ 1500 bytes)
- `recv()` siempre lee lo mismo → no hay que detectar fin de mensaje

---

# 3. Convención de campos por comando

| Comando | sender | target | payload |
|------|------|------|------|
| CMD_REGISTER | username | vacío | username |
| CMD_BROADCAST | username | vacío | mensaje |
| CMD_DIRECT | username | destinatario | mensaje |
| CMD_LIST | username | vacío | vacío |
| CMD_INFO | username | usuario consultado | vacío |
| CMD_STATUS | username | vacío | ACTIVE / BUSY / INACTIVE |
| CMD_LOGOUT | username | vacío | vacío |

### Respuestas del servidor

| Comando | sender | target | payload |
|------|------|------|------|
| CMD_OK | SERVER | destinatario | confirmación |
| CMD_ERROR | SERVER | destinatario | descripción error |
| CMD_MSG | remitente | destinatario o ALL | mensaje |
| CMD_USER_LIST | SERVER | solicitante | lista usuarios |
| CMD_USER_INFO | SERVER | solicitante | IP,STATUS |
| CMD_DISCONNECTED | SERVER | ALL | usuario que salió |

---

# 4. Enviar y recibir (patrón base)

## Enviar

```c
ChatPacket pkt;

memset(&pkt, 0, sizeof(pkt));

pkt.command = CMD_BROADCAST;

strncpy(pkt.sender, "alice", 31);
strncpy(pkt.payload, "Hola a todos!", 956);

pkt.payload_len = strlen(pkt.payload);

send(fd, &pkt, sizeof(pkt), 0);
```

## Recibir

```c
ChatPacket pkt;

recv(fd, &pkt, sizeof(pkt), MSG_WAITALL);

/* interpretar */

pkt.command
pkt.sender
pkt.target
pkt.payload
```

---

# 5. Flujos de sesión

## 5.1 Registro

```
Cliente                          Servidor
    |-- CMD_REGISTER ------------->|  sender="alice", payload="alice"
    |<-- CMD_OK -------------------|  payload="Bienvenido alice"
    |                              |  [crea sesion, status=ACTIVE]

    (si nombre o IP ya existe)
    |<-- CMD_ERROR ----------------|  payload="Usuario o IP ya registrados"
    [cliente cierra]
```

---

## 5.2 Broadcast

```
Cliente A                        Servidor                    Clientes B, C...
    |-- CMD_BROADCAST ----------->|  payload="Hola a todos"
    |                              |-- CMD_MSG --------------->|  sender="alice", target="ALL"
    |<-----------------------------|-- CMD_MSG -----------------|  (A tambien recibe su propio mensaje)
```

---

## 5.3 Mensaje directo

```
Cliente A (alice)                Servidor                    Cliente B (bob)
    |-- CMD_DIRECT -------------->|  sender="alice", target="bob", payload="Hola Bob"
    |                              |-- CMD_MSG --------------->|  sender="alice", target="bob"

    (si bob no esta conectado)
    |<-- CMD_ERROR ---------------|  payload="Destinatario no conectado"
```


---

## 5.4 Listado de usuarios

```
Cliente                          Servidor
    |-- CMD_LIST ----------------->|
    |<-- CMD_USER_LIST ------------|  payload="alice,ACTIVE;bob,BUSY;carlos,INACTIVE"
```

---

## 5.5 Información de usuario

```
Cliente                          Servidor
    |-- CMD_INFO ----------------->|  target="bob"
    |<-- CMD_USER_INFO ------------|  payload="192.168.1.10,BUSY"

    (si no existe)
    |<-- CMD_ERROR ---------------|  payload="Usuario no conectado"
```

---

## 5.6 Cambio de status

```
Cliente                          Servidor
    |-- CMD_STATUS --------------->|  payload="BUSY"
    |<-- CMD_OK -------------------|  payload="BUSY"
    [cliente actualiza UI]

    [sin actividad por INACTIVITY_TIMEOUT segundos]
    |<-- CMD_MSG ------------------|  sender="SERVER", payload="Tu status cambio a INACTIVE"
    [cliente actualiza UI]
```


---

## 5.7 Desconexión controlada

```
Cliente                          Servidor                    Otros clientes
    |-- CMD_LOGOUT --------------->|
    |<-- CMD_OK -------------------|
    |   [cierra socket]            |-- CMD_DISCONNECTED ------>|  payload="alice"
                                                                    |   [termina thread]
```


---

## 5.8 Desconexión abrupta


```
Cliente                          Servidor
    [crash / red caida]
                                recv() devuelve 0 o -1
                                [remueve cliente de lista]
                                [notifica CMD_DISCONNECTED a todos]
                                [termina thread]
```



---

# 6. Estructura del servidor en memoria

```c
typedef struct {

    char username[32];
    char ip[INET_ADDRSTRLEN];
    char status[16];

    int sockfd;
    int activo;

    time_t ultimo_mensaje;

} Cliente;

Cliente lista[100];
int num_clientes = 0;

pthread_mutex_t mutex_lista = PTHREAD_MUTEX_INITIALIZER;
```

No se requiere base de datos.

Los usuarios existen **solo mientras el servidor está activo**.

---

# 7. Comandos de la UI del cliente

| Comando | Acción |
|------|------|
| `/broadcast <mensaje>` | envía `CMD_BROADCAST` |
| `/msg <usuario> <mensaje>` | envía `CMD_DIRECT` |
| `/status <ACTIVE/BUSY/INACTIVE>` | envía `CMD_STATUS` |
| `/list` | envía `CMD_LIST` |
| `/info <usuario>` | envía `CMD_INFO` |
| `/help` | ayuda local |
| `/exit` | envía `CMD_LOGOUT` |

---

# 8. Configuración de red (EC2)

El **Security Group** debe permitir:

```
Tipo: Custom TCP
Puerto: <puertodelservidor>
Origen: 0.0.0.0/0
```

Si no se habilita esta regla:
Los clientes no podrán conectarse.
# Protocolo de Comunicación — Chat

## 1. Ejecución

Servidor: `./servidor <puerto>`

Cliente: `./cliente <username> <IP_servidor> <puerto>`

---

## 2. Struct Principal

```c
#define CMD_REGISTER    1
#define CMD_BROADCAST   2
#define CMD_DIRECT      3
#define CMD_LIST        4
#define CMD_INFO        5
#define CMD_STATUS      6
#define CMD_LOGOUT      7

#define CMD_OK          8
#define CMD_ERROR       9
#define CMD_MSG         10
#define CMD_USER_LIST   11
#define CMD_USER_INFO   12
#define CMD_DISCONNECTED 13

#define STATUS_ACTIVO   "ACTIVE"
#define STATUS_OCUPADO  "BUSY"
#define STATUS_INACTIVO "INACTIVE"

#define INACTIVITY_TIMEOUT 60

typedef struct {
    uint8_t  command;
    uint16_t payload_len;
    char     sender[32];
    char     target[32];
    char     payload[957];
} __attribute__((packed)) ChatPacket;  // 1024 bytes total
```

Tamaño fijo de 1024 bytes para alineación eficiente en memoria y TCP.

---

## 3. Comandos y Respuestas

| Comando | sender | target | payload |
|---------|--------|--------|---------|
| CMD_REGISTER | username | - | username |
| CMD_BROADCAST | username | - | mensaje |
| CMD_DIRECT | username | destinatario | mensaje |
| CMD_LIST | username | - | - |
| CMD_INFO | username | usuario | - |
| CMD_STATUS | username | - | ACTIVE/BUSY/INACTIVE |
| CMD_LOGOUT | username | - | - |
| CMD_OK | SERVER | cliente | confirmación |
| CMD_ERROR | SERVER | cliente | error |
| CMD_MSG | remitente | destinatario/ALL | mensaje |
| CMD_USER_LIST | SERVER | cliente | usuarios |
| CMD_USER_INFO | SERVER | cliente | IP,STATUS |
| CMD_DISCONNECTED | SERVER | ALL | usuario |

---

## 4. Flujos Principales

### Registro
```
Cliente                          Servidor
|-- CMD_REGISTER ------------->|  
|<-- CMD_OK -------------------|  Crea sesión, status=ACTIVE

Si existe usuario o IP:
|<-- CMD_ERROR -------------|  Usuario o IP ya registrados
```

### Broadcast
```
Cliente A      Servidor      Clientes B, C
|-- CMD_BROADCAST ------>|
|<-- CMD_MSG ---------->|  A recibe su propio mensaje
```

### Mensaje Directo
```
Cliente A      Servidor      Cliente B
|-- CMD_DIRECT ------>|
|-- CMD_MSG ------>|  
Si B no existe:
|-- CMD_ERROR ---|  Destinatario no conectado
```

### Status y Desconexión
```
|-- CMD_STATUS ------>|
|<-- CMD_OK ----------|  El servidor valida status

Sin actividad >= INACTIVITY_TIMEOUT:
|<-- CMD_MSG ---|  Tu status cambio a INACTIVE

|-- CMD_LOGOUT ------>|
|<-- CMD_OK ----------|
|-- CMD_DISCONNECTED ->|  A otros clientes
```

---

## 5. Comandos del Cliente

| Comando | Función |
|---------|---------|
| `/broadcast <mensaje>` | Enviar a todos |
| `/msg <usuario> <mensaje>` | Mensaje privado |
| `/status <ACTIVE/BUSY/INACTIVE>` | Cambiar estado |
| `/list` | Ver usuarios conectados |
| `/info <usuario>` | Información de usuario |
| `/help` | Mostrar ayuda |
| `/exit` | Desconectarse |

---

## 6. Configuración de Red (EC2)

Security Group:
- Tipo: Custom TCP
- Puerto: puerto_del_servidor
- Origen: 0.0.0.0/0

Sin esta regla, los clientes no podrán conectarse.
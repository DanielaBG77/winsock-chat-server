/*
 * servidor.c - Servidor de chat concurrente para Windows (WinSock2 + hilos Win32)
 * Taller de Sistemas Operativos - UNINTEP
 *
 * Qué hace este programa (dos servicios en un solo .exe):
 *   1) Chat TCP en el puerto 6000 -> clientes de terminal (cliente.exe, ncat).
 *   2) Servidor web (HTTP) en el puerto 9090 -> navegador con la página index.html.
 *
 * Rutas HTTP disponibles:
 *   GET  /          -> entrega la página index.html (debe estar junto a servidor.exe)
 *   GET  /mensajes  -> devuelve el historial en JSON (id, hora, origen, usuario, texto)
 *   GET  /ws        -> RETO: se convierte en WebSocket (RFC 6455); el servidor EMPUJA cada mensaje nuevo
 *   POST /enviar    -> recibe {"usuario":"...","texto":"..."} y lo reparte a los clientes TCP
 *
 * Compilar:  gcc servidor.c -o servidor.exe -lws2_32
 */

#include <stdio.h>      // printf, snprintf, fopen, fread, fclose, fgets (entrada y salida)
#include <stdlib.h>     // malloc, free, atoi (memoria dinámica y conversión de texto a número)
#include <string.h>     // strlen, strcmp, strstr, strncpy, memmove (manejo de cadenas)
#include <ctype.h>      // isspace, tolower (clasificación de caracteres)
#include <winsock2.h>   // API de sockets de Windows (socket, bind, listen, accept, recv, send)
#include <windows.h>    // API Win32: hilos, secciones críticas y hora local
#include "websocket.h"  // RETO: SHA-1, Base64, handshake y tramas WebSocket (RFC 6455)

#define PUERTO_CHAT 6000    // puerto TCP del chat (antes era 5000)
#define PUERTO_WEB  9090    // puerto HTTP de la página web (antes era 8080)
#define BUFFER_SIZE 1024    // tamaño máximo de lo que se lee de un cliente TCP de una vez
#define MAX_CLIENTS 10      // cantidad máxima de clientes TCP conectados al mismo tiempo
#define MAX_WS      20      // cantidad máxima de navegadores conectados por WebSocket

#define MAX_MSGS    200     // cuántos mensajes recuerda el historial
#define MAX_USER    32      // largo máximo del nombre de usuario (incluye el '\0')
#define MAX_TEXT    512     // largo máximo del texto de un mensaje (incluye el '\0')
#define HTTP_BUF    8192    // tamaño máximo de una petición HTTP completa

/* ============================================================
 *  RECURSOS COMPARTIDOS (los usan varios hilos a la vez)
 * ============================================================ */

SOCKET clients[MAX_CLIENTS];    // arreglo con los sockets de los clientes TCP conectados
int clientCount = 0;            // cuántos clientes hay conectados ahora mismo
CRITICAL_SECTION lockClients;   // "candado" que protege clients[] y clientCount

typedef struct {                // un mensaje guardado en el historial
    unsigned long id;           // número consecutivo (así la web no repite mensajes)
    char hora[9];               // hora en formato "HH:MM:SS" (8 caracteres + '\0')
    char origen[8];             // quién lo envió: "TCP", "WEB" o "ADMIN"
    char usuario[MAX_USER];     // nombre de quien escribió
    char texto[MAX_TEXT];       // contenido del mensaje
} Mensaje;                      // nombre del tipo de dato

Mensaje historial[MAX_MSGS];    // arreglo con los últimos mensajes
int msgCount = 0;               // cuántos mensajes hay guardados
unsigned long nextMsgId = 1;    // número que tendrá el próximo mensaje
CRITICAL_SECTION lockMsgs;      // "candado" que protege historial[], msgCount y nextMsgId

SOCKET wsClients[MAX_WS];       // sockets de los navegadores conectados por WebSocket
int wsCount = 0;                // cuántos navegadores WebSocket hay conectados
CRITICAL_SECTION lockWs;        // "candado" que protege wsClients[], wsCount y TODOS los send() a esos sockets
                                // ORDEN DE CANDADOS (evita deadlock): lockMsgs primero, lockWs después. Nunca al revés.

/* ============================================================
 *  LISTA DE CLIENTES TCP
 * ============================================================ */

/* Agrega un cliente a la lista. Devuelve 1 si había cupo y 0 si está llena. */
int AddClient(SOCKET s) {                       // recibe el socket del cliente nuevo
    int ok = 0;                                 // empieza asumiendo que no hay cupo
    EnterCriticalSection(&lockClients);         // cierra el candado: solo un hilo a la vez
    if (clientCount < MAX_CLIENTS) {            // ¿todavía queda espacio?
        clients[clientCount++] = s;             // guarda el socket y sube el contador
        ok = 1;                                 // avisa que sí se pudo agregar
    }                                           // fin del if
    LeaveCriticalSection(&lockClients);         // abre el candado para los demás hilos
    return ok;                                  // devuelve 1 (agregado) o 0 (lleno)
}                                               // fin de AddClient

/* Quita un cliente de la lista (cuando se desconecta). */
void RemoveClient(SOCKET s) {                   // recibe el socket que se va
    EnterCriticalSection(&lockClients);         // cierra el candado
    for (int i = 0; i < clientCount; i++) {     // recorre la lista de clientes
        if (clients[i] == s) {                  // ¿este es el que se va?
            clients[i] = clients[clientCount - 1];  // pone el último en su lugar (sin dejar huecos)
            clientCount--;                      // ahora hay uno menos
            break;                              // ya lo encontramos: dejamos de buscar
        }                                       // fin del if
    }                                           // fin del for
    LeaveCriticalSection(&lockClients);         // abre el candado
}                                               // fin de RemoveClient

/* Envía un mensaje a todos los clientes menos al que lo escribió.
 * Si sender es INVALID_SOCKET, no se excluye a nadie (le llega a TODOS). */
void Broadcast(const char* msg, int len, SOCKET sender) {   // texto, su largo y quién lo envió
    EnterCriticalSection(&lockClients);         // cierra el candado mientras recorremos la lista
    for (int i = 0; i < clientCount; i++) {     // recorre todos los clientes conectados
        if (clients[i] != sender) {             // salta al emisor para no devolverle su propio mensaje
            send(clients[i], msg, len, 0);      // manda el texto por el socket de ese cliente
        }                                       // fin del if
    }                                           // fin del for
    LeaveCriticalSection(&lockClients);         // abre el candado
}                                               // fin de Broadcast

/* ============================================================
 *  CLIENTES WEBSOCKET (RETO AVANZADO)
 * ============================================================ */

void JsonEscape(const char* in, char* out, size_t outSize);    // prototipo: se define más abajo

/* Quita un navegador de la lista WebSocket (cuando se desconecta). */
void RemoveWsClient(SOCKET s) {                 // recibe el socket que se va
    EnterCriticalSection(&lockWs);              // cierra el candado
    for (int i = 0; i < wsCount; i++) {         // recorre la lista
        if (wsClients[i] == s) {                // ¿este es?
            wsClients[i] = wsClients[wsCount - 1];  // pone el último en su lugar
            wsCount--;                          // hay uno menos
            break;                              // listo
        }                                       // fin del if
    }                                           // fin del for
    LeaveCriticalSection(&lockWs);              // abre el candado
}                                               // fin de RemoveWsClient

/* Envía UNA trama a un navegador. Quien la llame DEBE tener lockWs tomado:
 * así dos hilos nunca mezclan bytes de dos tramas en el mismo socket. */
void WsSendFrameLocked(SOCKET s, int opcode, const uint8_t* payload, size_t len) {
    uint8_t frame[WS_MAX_PAYLOAD + 8];          // espacio para encabezado + contenido
    int n = WsEncodeFrame(opcode, payload, len, frame, sizeof(frame));  // arma la trama (sin máscara)
    if (n > 0) send(s, (const char*)frame, n, 0);   // la manda por el socket
}                                               // fin de WsSendFrameLocked

/* Igual que la anterior, pero toma y suelta el candado por su cuenta. */
void WsSendFrame(SOCKET s, int opcode, const uint8_t* payload, size_t len) {
    EnterCriticalSection(&lockWs);              // cierra el candado
    WsSendFrameLocked(s, opcode, payload, len); // manda la trama
    LeaveCriticalSection(&lockWs);              // abre el candado
}                                               // fin de WsSendFrame

/* Empuja UN mensaje (como JSON) a todos los navegadores conectados por WebSocket. */
void BroadcastWsMensaje(const Mensaje* m) {     // el mensaje recién guardado
    char u[MAX_USER * 6 + 8];                   // nombre ya escapado
    char t[MAX_TEXT * 6 + 8];                   // texto ya escapado
    char json[WS_MAX_PAYLOAD];                  // JSON final del mensaje
    JsonEscape(m->usuario, u, sizeof(u));       // escapa el nombre
    JsonEscape(m->texto, t, sizeof(t));         // escapa el texto
    int len = snprintf(json, sizeof(json),      // arma el objeto JSON de un solo mensaje
                       "{\"id\":%lu,\"hora\":\"%s\",\"origen\":\"%s\",\"usuario\":\"%s\",\"texto\":\"%s\"}",
                       m->id, m->hora, m->origen, u, t);
    if (len <= 0 || len >= (int)sizeof(json)) return;   // si no cupo completo, no se manda (JSON roto)

    EnterCriticalSection(&lockWs);              // cierra el candado mientras recorremos la lista
    for (int i = 0; i < wsCount; i++)           // para cada navegador conectado
        WsSendFrameLocked(wsClients[i], WS_OP_TEXT, (const uint8_t*)json, (size_t)len);
    LeaveCriticalSection(&lockWs);              // abre el candado
}                                               // fin de BroadcastWsMensaje

/* ============================================================
 *  HISTORIAL DE MENSAJES
 * ============================================================ */

/* Guarda un mensaje nuevo en el historial (con id y hora). */
void AddMessage(const char* usuario, const char* texto, const char* origen) {   // datos del mensaje
    SYSTEMTIME st;                              // estructura de Windows para guardar la fecha y hora
    GetLocalTime(&st);                          // llena st con la hora local del computador

    EnterCriticalSection(&lockMsgs);            // cierra el candado del historial
    if (msgCount == MAX_MSGS) {                 // ¿el historial está lleno?
        // desplaza todo una posición hacia arriba: se descarta el mensaje más antiguo
        memmove(&historial[0], &historial[1], sizeof(Mensaje) * (MAX_MSGS - 1));
        msgCount--;                             // queda un espacio libre al final
    }                                           // fin del if

    Mensaje* m = &historial[msgCount];          // puntero a la casilla donde irá el mensaje nuevo
    m->id = nextMsgId++;                        // le asigna el número actual y prepara el siguiente
    // escribe la hora como HH:MM:SS (el %100 solo asegura que quepa en 2 dígitos)
    snprintf(m->hora, sizeof(m->hora), "%02u:%02u:%02u",
             (unsigned)(st.wHour % 100), (unsigned)(st.wMinute % 100), (unsigned)(st.wSecond % 100));
    snprintf(m->origen, sizeof(m->origen), "%s", origen);   // copia el origen sin pasarse del tamaño
    strncpy(m->usuario, usuario, MAX_USER - 1); // copia el nombre (como máximo MAX_USER-1 caracteres)
    m->usuario[MAX_USER - 1] = '\0';            // garantiza que el nombre termine en '\0'
    strncpy(m->texto, texto, MAX_TEXT - 1);     // copia el texto (como máximo MAX_TEXT-1 caracteres)
    m->texto[MAX_TEXT - 1] = '\0';              // garantiza que el texto termine en '\0'
    msgCount++;                                 // ahora hay un mensaje más guardado
    BroadcastWsMensaje(m);                      // RETO: lo empuja a los navegadores WebSocket (sin que pregunten)
    LeaveCriticalSection(&lockMsgs);            // abre el candado del historial
}                                               // fin de AddMessage

/* Cambia saltos de línea y otros caracteres de control por espacios. */
void Sanitize(char* s) {                        // recibe la cadena que se va a limpiar
    for (; *s != '\0'; s++) {                   // avanza carácter por carácter hasta el final
        if ((unsigned char)*s < 0x20) *s = ' '; // los códigos menores a 32 son de control: se cambian por ' '
    }                                           // fin del for
}                                               // fin de Sanitize

/* ============================================================
 *  UTILIDADES JSON MÍNIMAS
 * ============================================================ */

/* Copia 'in' en 'out' escapando comillas, barras invertidas y caracteres de control. */
void JsonEscape(const char* in, char* out, size_t outSize) {    // origen, destino y tamaño del destino
    size_t j = 0;                               // posición actual de escritura en 'out'
    // recorre 'in' mientras queden al menos 7 bytes libres (el escape más largo mide 6 + '\0')
    for (size_t i = 0; in[i] != '\0' && j + 7 < outSize; i++) {
        unsigned char c = (unsigned char)in[i]; // carácter actual como número sin signo
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = (char)c; }     // comilla o barra: se antepone '\'
        else if (c == '\n')        { out[j++] = '\\'; out[j++] = 'n'; }         // salto de línea -> \n
        else if (c == '\r')        { out[j++] = '\\'; out[j++] = 'r'; }         // retorno de carro -> \r
        else if (c == '\t')        { out[j++] = '\\'; out[j++] = 't'; }         // tabulación -> \t
        // otros caracteres de control se escriben como \u00XX
        else if (c < 0x20)         { j += (size_t)snprintf(out + j, outSize - j, "\\u%04x", c); }
        else                       { out[j++] = (char)c; }                      // carácter normal: se copia igual
    }                                           // fin del for
    out[j] = '\0';                              // cierra la cadena de salida
}                                               // fin de JsonEscape

/* Busca "clave":"valor" en un JSON sencillo y copia el valor en 'out'. Devuelve 1 si lo halló. */
int JsonGetString(const char* json, const char* key, char* out, size_t outSize) {   // json, clave, destino, tamaño
    char pattern[64];                           // aquí se arma el texto a buscar: "clave" (con comillas)
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);  // construye el patrón con las comillas incluidas

    const char* p = strstr(json, pattern);      // busca el patrón dentro del JSON
    if (p == NULL) return 0;                    // si no está, la clave no existe
    p += strlen(pattern);                       // salta el nombre de la clave

    while (isspace((unsigned char)*p)) p++;     // ignora espacios antes de los dos puntos
    if (*p != ':') return 0;                    // debe haber ':' o el JSON es inválido
    p++;                                        // salta los dos puntos
    while (isspace((unsigned char)*p)) p++;     // ignora espacios después de los dos puntos
    if (*p != '"') return 0;                    // el valor debe empezar con comillas
    p++;                                        // salta la comilla de apertura

    size_t j = 0;                               // posición de escritura en 'out'
    // copia hasta la comilla de cierre o hasta quedarse sin espacio (siempre deja lugar al '\0')
    while (*p != '\0' && *p != '"' && j + 1 < outSize) {
        if (*p == '\\' && p[1] != '\0') {       // ¿es una secuencia de escape (\n, \", \\ ...)?
            p++;                                // se mira el carácter que va después de la barra
            if (*p == 'n' || *p == 'r' || *p == 't') out[j++] = ' ';    // saltos y tabulaciones -> espacio
            else out[j++] = *p;                 // \" o \\ -> se guarda el carácter tal cual
            p++;                                // avanza al siguiente carácter
        } else {                                // carácter normal
            out[j++] = *p++;                    // se copia y se avanza
        }                                       // fin del if/else
    }                                           // fin del while
    out[j] = '\0';                              // cierra la cadena de salida
    return 1;                                   // avisa que sí encontró el valor
}                                               // fin de JsonGetString

/* ============================================================
 *  SERVIDOR DEL CHAT TCP (puerto 6000)
 * ============================================================ */

/* Hilo que atiende a UN cliente TCP: se crea uno por cada usuario conectado. */
DWORD WINAPI ClientHandler(LPVOID lpParam) {    // lpParam apunta al socket del cliente
    SOCKET clientSocket = *(SOCKET*)lpParam;    // copia el socket que main() dejó en memoria dinámica
    free(lpParam);                              // libera esa memoria: ya no se necesita

    char buffer[BUFFER_SIZE];                   // zona donde llegan los datos del cliente
    int bytesReceived;                          // cuántos bytes llegaron en cada lectura

    // recv espera datos; devuelve >0 si llegaron, 0 si el cliente cerró, y <0 si hubo un error
    while ((bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytesReceived] = '\0';           // cierra el texto recibido para tratarlo como cadena C
        printf("[CHAT %d] %s", PUERTO_CHAT, buffer);    // muestra en la consola del servidor lo recibido

        char linea[BUFFER_SIZE];                // copia de trabajo para poder modificar el texto
        snprintf(linea, sizeof(linea), "%s", buffer);   // copia el mensaje sin pasarse del tamaño
        linea[strcspn(linea, "\r\n")] = '\0';   // corta la línea donde empieza el salto de línea
        if (linea[0] != '\0') {                 // si quedó algo de texto
            char* sep = strstr(linea, ": ");    // busca el separador "nombre: texto" que usa cliente.exe
            if (sep != NULL) {                  // ¿tiene ese formato?
                *sep = '\0';                    // parte la cadena en dos: nombre y texto
                AddMessage(linea, sep + 2, "TCP");      // guarda: nombre = antes del ": ", texto = después
            } else {                            // llegó sin formato (por ejemplo, desde ncat)
                AddMessage("TCP", linea, "TCP");        // guarda todo como texto de un usuario genérico
            }                                   // fin del if/else interno
        }                                       // fin del if de la línea

        Broadcast(buffer, bytesReceived, clientSocket); // reparte el mensaje a los demás clientes
    }                                           // fin del while: el cliente se fue o falló la conexión

    RemoveClient(clientSocket);                 // lo saca de la lista de clientes
    closesocket(clientSocket);                  // cierra su socket y libera el recurso
    printf("[CHAT %d] Un usuario salio del chat.\n", PUERTO_CHAT);  // avisa en la consola del servidor
    return 0;                                   // termina el hilo
}                                               // fin de ClientHandler

/* Hilo que lee lo que escribes en la terminal del servidor y lo manda a todos. */
DWORD WINAPI ServerConsole(LPVOID lpParam) {    // no recibe datos útiles
    (void)lpParam;                              // evita la advertencia de "parámetro sin usar"
    char buffer[BUFFER_SIZE];                   // aquí se guarda cada línea que escribes
    char mensaje[BUFFER_SIZE + 20];             // mensaje final con prefijo (con holgura para el prefijo)

    // fgets espera a que escribas una línea; devuelve NULL si se cierra la entrada
    while (fgets(buffer, sizeof(buffer), stdin) != NULL) {
        buffer[strcspn(buffer, "\r\n")] = '\0'; // quita el salto de línea del final
        if (buffer[0] == '\0') continue;        // si la línea está vacía, no se envía nada

        AddMessage("Consola", buffer, "ADMIN"); // guarda el mensaje en el historial para la web
        int len = snprintf(mensaje, sizeof(mensaje), "Consola: %s\n", buffer);  // arma "Consola: texto"
        Broadcast(mensaje, len, INVALID_SOCKET);    // lo envía a TODOS los clientes TCP
    }                                           // fin del while
    return 0;                                   // termina el hilo
}                                               // fin de ServerConsole

/* ============================================================
 *  SERVIDOR HTTP EMBEBIDO (puerto 9090)
 * ============================================================ */

/* Envía todos los bytes pedidos (send puede enviar menos de lo que se le pide). */
int SendAll(SOCKET s, const char* data, int len) {  // socket, datos y cantidad
    int sent = 0;                               // cuántos bytes se han enviado hasta ahora
    while (sent < len) {                        // repite hasta enviar todo
        int n = send(s, data + sent, len - sent, 0);    // envía lo que falta, desde donde se quedó
        if (n == SOCKET_ERROR) return 0;        // si falla, avisa con 0
        sent += n;                              // suma lo que sí se envió
    }                                           // fin del while
    return 1;                                   // todo se envió bien
}                                               // fin de SendAll

/* Responde con las cabeceras HTTP (incluye CORS) y el cuerpo. */
void SendHttp(SOCKET s, const char* status, const char* contentType,
              const char* body, int bodyLen) {  // socket, estado, tipo de contenido, cuerpo y su largo
    char header[512];                           // espacio para armar las cabeceras
    int h = snprintf(header, sizeof(header),    // arma el texto de las cabeceras y guarda su largo en h
        "HTTP/1.1 %s\r\n"                       // línea de estado (por ejemplo "200 OK")
        "Content-Type: %s\r\n"                  // tipo de contenido que se devuelve
        "Access-Control-Allow-Origin: *\r\n"    // CORS: cualquier origen puede usar esta API
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"  // CORS: métodos permitidos
        "Access-Control-Allow-Headers: Content-Type\r\n"        // CORS: cabeceras permitidas
        "Content-Length: %d\r\n"                // tamaño del cuerpo en bytes
        "Connection: close\r\n"                 // se cierra la conexión después de responder
        "\r\n",                                 // línea vacía: aquí terminan las cabeceras
        status, contentType, bodyLen);          // valores que reemplazan los %s y %d de arriba

    if (!SendAll(s, header, h)) return;         // envía las cabeceras; si falla, no sigue
    if (bodyLen > 0) SendAll(s, body, bodyLen); // si hay cuerpo, lo envía después
}                                               // fin de SendHttp

/* Atajo: responde con un cuerpo de texto cuyo largo se calcula solo. */
void SendHttpText(SOCKET s, const char* status, const char* contentType, const char* body) {
    SendHttp(s, status, contentType, body, (int)strlen(body));  // usa strlen para el largo del cuerpo
}                                               // fin de SendHttpText

/* GET /mensajes -> arma el JSON con todo el historial y lo envía. */
void HandleGetMensajes(SOCKET s) {              // recibe el socket del navegador
    // tamaño máximo posible del JSON (el *6 cubre el peor caso de escapes)
    size_t cap = (size_t)MAX_MSGS * ((MAX_USER + MAX_TEXT) * 6 + 120) + 16;
    char* json = (char*)malloc(cap);            // reserva la memoria para el JSON
    if (json == NULL) {                         // ¿no hubo memoria disponible?
        SendHttpText(s, "500 Internal Server Error", "application/json",
                     "{\"error\":\"sin memoria disponible\"}");     // responde con un error 500
        return;                                 // termina la función
    }                                           // fin del if

    size_t pos = 0;                             // posición actual de escritura dentro de json
    json[pos++] = '[';                          // un arreglo JSON empieza con '['

    EnterCriticalSection(&lockMsgs);            // cierra el candado: nadie modifica el historial mientras se lee
    for (int i = 0; i < msgCount; i++) {        // recorre todos los mensajes guardados
        char u[MAX_USER * 6 + 8];               // nombre ya escapado
        char t[MAX_TEXT * 6 + 8];               // texto ya escapado
        JsonEscape(historial[i].usuario, u, sizeof(u)); // escapa el nombre de usuario
        JsonEscape(historial[i].texto, t, sizeof(t));   // escapa el texto del mensaje
        // agrega al JSON un objeto con los cinco campos (con una coma antes si no es el primero)
        pos += (size_t)snprintf(json + pos, cap - pos,
                                "%s{\"id\":%lu,\"hora\":\"%s\",\"origen\":\"%s\","
                                "\"usuario\":\"%s\",\"texto\":\"%s\"}",
                                (i > 0) ? "," : "",
                                historial[i].id, historial[i].hora, historial[i].origen, u, t);
    }                                           // fin del for
    LeaveCriticalSection(&lockMsgs);            // abre el candado

    json[pos++] = ']';                          // un arreglo JSON termina con ']'
    json[pos] = '\0';                           // cierra la cadena

    SendHttp(s, "200 OK", "application/json; charset=utf-8", json, (int)pos);   // envía el JSON
    free(json);                                 // libera la memoria reservada
}                                               // fin de HandleGetMensajes

/* GET / -> entrega index.html (el archivo debe estar en la carpeta desde donde se ejecuta servidor.exe). */
void HandleGetIndex(SOCKET s) {                 // recibe el socket del navegador
    FILE* f = fopen("index.html", "rb");        // abre el archivo en modo binario de lectura
    if (f == NULL) {                            // ¿no se encontró el archivo?
        SendHttpText(s, "404 Not Found", "text/plain; charset=utf-8",
                     "No encuentro index.html en la carpeta del servidor");     // responde 404
        return;                                 // termina la función
    }                                           // fin del if

    fseek(f, 0, SEEK_END);                      // se mueve al final del archivo
    long size = ftell(f);                       // la posición actual es el tamaño del archivo
    fseek(f, 0, SEEK_SET);                      // vuelve al inicio para poder leerlo

    if (size <= 0 || size > 1024 * 1024) {      // tamaño vacío o mayor a 1 MB: se rechaza
        fclose(f);                              // cierra el archivo
        SendHttpText(s, "500 Internal Server Error", "text/plain; charset=utf-8",
                     "index.html tiene un tamano que no sirve");        // responde 500
        return;                                 // termina la función
    }                                           // fin del if

    char* data = (char*)malloc((size_t)size);   // reserva memoria para guardar el contenido
    if (data == NULL) {                         // ¿no hubo memoria?
        fclose(f);                              // cierra el archivo
        SendHttpText(s, "500 Internal Server Error", "text/plain; charset=utf-8",
                     "No hay memoria suficiente");  // responde 500
        return;                                 // termina la función
    }                                           // fin del if

    size_t n = fread(data, 1, (size_t)size, f); // lee el archivo completo y guarda cuántos bytes leyó
    fclose(f);                                  // cierra el archivo

    SendHttp(s, "200 OK", "text/html; charset=utf-8", data, (int)n);    // envía la página al navegador
    free(data);                                 // libera la memoria reservada
}                                               // fin de HandleGetIndex

/* Valida y publica un mensaje que vino de la web (lo usan POST /enviar y el WebSocket).
 * Devuelve 1 si se publicó y 0 si el JSON no trae el campo "texto". */
int PublicarDesdeWeb(const char* json) {        // JSON {"usuario":"...","texto":"..."} terminado en '\0'
    char usuario[MAX_USER];                     // aquí queda el nombre extraído del JSON
    char texto[MAX_TEXT];                       // aquí queda el texto extraído del JSON

    // el campo "texto" es obligatorio y no puede venir vacío
    if (!JsonGetString(json, "texto", texto, sizeof(texto)) || texto[0] == '\0') return 0;
    // si no llega "usuario" (o viene vacío), se usa un nombre por defecto
    if (!JsonGetString(json, "usuario", usuario, sizeof(usuario)) || usuario[0] == '\0') {
        snprintf(usuario, sizeof(usuario), "Visitante");    // nombre por defecto
    }                                           // fin del if
    Sanitize(usuario);                          // quita caracteres de control del nombre
    Sanitize(texto);                            // quita caracteres de control del texto

    AddMessage(usuario, texto, "WEB");          // guarda el mensaje (y lo empuja a los WebSocket)

    char linea[BUFFER_SIZE];                    // texto que se enviará a los clientes TCP
    int len = snprintf(linea, sizeof(linea), "%s: %s\n", usuario, texto);   // arma "usuario: texto"
    if (len >= (int)sizeof(linea)) len = (int)sizeof(linea) - 1;            // si se truncó, se manda lo que cupo
    Broadcast(linea, len, INVALID_SOCKET);      // lo envía a TODOS los clientes TCP

    printf("[WEB %d] %s escribio desde el navegador: %s\n", PUERTO_WEB, usuario, texto);   // registro en consola
    return 1;                                   // se publicó bien
}                                               // fin de PublicarDesdeWeb

/* POST /enviar -> guarda el mensaje de la web y lo reparte a los clientes TCP. */
void HandlePostEnviar(SOCKET s, const char* body) {     // socket y cuerpo (JSON) de la petición
    if (!PublicarDesdeWeb(body)) {              // ¿faltó el campo texto?
        SendHttpText(s, "400 Bad Request", "application/json",
                     "{\"error\":\"el campo texto es obligatorio\"}");   // responde 400
        return;                                 // termina la función
    }                                           // fin del if
    SendHttpText(s, "200 OK", "application/json", "{\"ok\":true,\"detalle\":\"mensaje entregado\"}");  // confirma
}                                               // fin de HandlePostEnviar

/* ============================================================
 *  WEBSOCKET (RETO AVANZADO): GET /ws
 * ============================================================ */

/* Busca una cabecera HTTP (sin importar mayúsculas) y copia su valor en 'out'. Devuelve 1 si la halló. */
int GetHeaderValue(const char* req, const char* name, char* out, size_t outSize) {
    size_t nl = strlen(name);                   // largo del nombre buscado
    const char* p = strstr(req, "\r\n");        // salta la primera línea (GET /ws HTTP/1.1)
    while (p != NULL) {                         // recorre línea por línea
        p += 2;                                 // se coloca al inicio de la línea siguiente
        if (_strnicmp(p, name, nl) == 0 && p[nl] == ':') {  // ¿empieza con "Nombre:"?
            p += nl + 1;                        // salta el nombre y los dos puntos
            while (*p == ' ') p++;              // ignora espacios
            size_t j = 0;                       // posición de escritura en 'out'
            while (*p != '\0' && *p != '\r' && *p != '\n' && j + 1 < outSize) out[j++] = *p++;  // copia el valor
            out[j] = '\0';                      // cierra la cadena
            return 1;                           // encontrada
        }                                       // fin del if
        p = strstr(p, "\r\n");                  // pasa a la línea siguiente
    }                                           // fin del while
    return 0;                                   // no existe esa cabecera
}                                               // fin de GetHeaderValue

/* Convierte la conexión HTTP en WebSocket y atiende al navegador hasta que se desconecte.
 * Este hilo ya es "uno por conexión", así que simplemente se queda aquí leyendo tramas. */
void HandleWebSocket(SOCKET s, const char* req) {   // socket del navegador y su petición HTTP
    char upgrade[32], key[64], version[16], accept[32];     // cabeceras que necesitamos

    // 1. Validar que de verdad pide un WebSocket (RFC 6455, sección 4.2.1)
    if (!GetHeaderValue(req, "Upgrade", upgrade, sizeof(upgrade)) || _stricmp(upgrade, "websocket") != 0 ||
        !GetHeaderValue(req, "Sec-WebSocket-Key", key, sizeof(key)) ||
        !GetHeaderValue(req, "Sec-WebSocket-Version", version, sizeof(version)) || strcmp(version, "13") != 0 ||
        !WsAcceptKey(key, accept)) {
        SendHttpText(s, "400 Bad Request", "text/plain", "Handshake WebSocket invalido");  // responde 400
        return;                                 // termina
    }                                           // fin del if

    // 2. Configurar el socket: tiempo máximo de envío (cliente lento) y keep-alive (cliente caído)
    DWORD sndTimeout = 2000;                    // 2 segundos
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));
    BOOL keepAlive = TRUE;                      // detecta conexiones muertas sin FIN (cable desconectado)
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepAlive, sizeof(keepAlive));

    // 3. Responder el handshake y registrar al cliente EN LA MISMA sección crítica:
    //    así ningún broadcast puede colarse antes del "101 Switching Protocols".
    char resp[256];                             // respuesta HTTP 101
    int rl = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept);
    EnterCriticalSection(&lockWs);              // cierra el candado
    int hayCupo = (wsCount < MAX_WS);           // ¿queda espacio?
    if (hayCupo) {                              // sí
        SendAll(s, resp, rl);                   // envía el 101
        wsClients[wsCount++] = s;               // lo agrega a la lista
    }                                           // fin del if
    LeaveCriticalSection(&lockWs);              // abre el candado
    if (!hayCupo) {                             // lista llena
        SendHttpText(s, "503 Service Unavailable", "text/plain", "Demasiados navegadores conectados");
        return;                                 // termina
    }                                           // fin del if
    printf("[WS %d] Navegador conectado por WebSocket.\n", PUERTO_WEB);    // aviso en consola

    // 4. Bucle de lectura de tramas
    uint8_t buf[WS_MAX_PAYLOAD + 16];           // acumula los bytes recibidos (una trama puede llegar partida)
    size_t have = 0;                            // cuántos bytes hay acumulados
    int running = 1;                            // se pone en 0 para salir
    while (running) {                           // repite hasta que se cierre la conexión
        if (have >= sizeof(buf)) break;         // buffer lleno sin trama completa: abuso, se corta
        int n = recv(s, (char*)buf + have, (int)(sizeof(buf) - have), 0);  // lee lo que haya llegado
        if (n <= 0) break;                      // 0 = cierre limpio, <0 = desconexión abrupta/error
        have += (size_t)n;                      // suma lo leído

        while (running && have > 0) {           // procesa TODAS las tramas completas que haya
            WsFrame f;                          // aquí queda la trama decodificada
            int used = WsDecodeFrame(buf, have, &f);    // intenta leer una trama (y le quita la máscara)
            if (used == 0) break;               // incompleta: falta otro recv
            if (used < 0) {                     // trama inválida
                const uint8_t code[2] = { 0x03, 0xEA };     // código de cierre 1002 (error de protocolo)
                WsSendFrame(s, WS_OP_CLOSE, code, 2);       // avisa antes de cortar
                running = 0;                    // sale
                break;
            }                                   // fin del if

            if (f.opcode == WS_OP_TEXT) {       // mensaje de chat
                char texto[WS_MAX_PAYLOAD + 1]; // copia con '\0' (el payload no lo trae)
                memcpy(texto, f.payload, f.payloadLen);     // copia exacta
                texto[f.payloadLen] = '\0';     // cierra la cadena
                PublicarDesdeWeb(texto);        // valida y publica (el WS se entera por AddMessage)
            } else if (f.opcode == WS_OP_PING) {    // "¿sigues ahí?"
                WsSendFrame(s, WS_OP_PONG, f.payload, f.payloadLen);    // responde con el mismo contenido
            } else if (f.opcode == WS_OP_CLOSE) {   // el navegador cierra
                WsSendFrame(s, WS_OP_CLOSE, f.payload, f.payloadLen);   // confirma el cierre (eco)
                running = 0;                    // sale
            }                                   // (pong, binario y demás se ignoran)

            memmove(buf, buf + used, have - (size_t)used);  // descarta la trama ya procesada
            have -= (size_t)used;               // quedan menos bytes acumulados
        }                                       // fin del while interno
    }                                           // fin del while externo

    RemoveWsClient(s);                          // lo saca de la lista ANTES de que se cierre el socket
    printf("[WS %d] Navegador WebSocket desconectado.\n", PUERTO_WEB);     // aviso en consola
}                                               // fin de HandleWebSocket

/* Lee la petición completa, la interpreta y responde. */
void ProcessHttpRequest(SOCKET s) {             // recibe el socket del navegador
    char req[HTTP_BUF];                         // zona donde se guarda la petición completa
    int total = 0;                              // bytes acumulados en req
    char* headerEnd = NULL;                     // apuntará al final de las cabeceras ("\r\n\r\n")

    // 1. Leer hasta tener las cabeceras completas (terminan con una línea vacía)
    while (total < HTTP_BUF - 1) {              // mientras quepa más información
        int n = recv(s, req + total, HTTP_BUF - 1 - total, 0);  // lee lo que haya llegado
        if (n <= 0) break;                      // si el navegador cerró o hubo error, se sale
        total += n;                             // suma lo leído
        req[total] = '\0';                      // cierra la cadena para poder buscar texto
        headerEnd = strstr(req, "\r\n\r\n");    // busca la línea vacía que cierra las cabeceras
        if (headerEnd != NULL) break;           // ya llegaron todas las cabeceras
    }                                           // fin del while
    if (headerEnd == NULL) {                    // si nunca llegaron completas
        SendHttpText(s, "400 Bad Request", "text/plain", "Peticion mal formada");   // responde 400
        return;                                 // termina la función
    }                                           // fin del if

    int headerLen = (int)(headerEnd - req) + 4; // largo de las cabeceras incluyendo el "\r\n\r\n"

    // 2. Buscar Content-Length (sin importar mayúsculas o minúsculas)
    int contentLength = 0;                      // largo del cuerpo (0 si no hay)
    char low[HTTP_BUF];                         // copia de las cabeceras en minúsculas
    for (int i = 0; i < headerLen; i++) low[i] = (char)tolower((unsigned char)req[i]);  // pasa a minúsculas
    low[headerLen] = '\0';                      // cierra la cadena
    char* cl = strstr(low, "content-length:");  // busca la cabecera que indica el largo del cuerpo
    if (cl != NULL) contentLength = atoi(req + (cl - low) + 15);    // convierte el número que sigue (15 = largo del nombre)

    // si el cuerpo es negativo o no cabe en el buffer, se rechaza la petición
    if (contentLength < 0 || headerLen + contentLength > HTTP_BUF - 1) {
        SendHttpText(s, "413 Payload Too Large", "text/plain", "Peticion demasiado grande");   // responde 413
        return;                                 // termina la función
    }                                           // fin del if

    // 3. Leer el cuerpo completo si todavía no llegó entero
    while (total < headerLen + contentLength) { // mientras falten bytes del cuerpo
        int n = recv(s, req + total, HTTP_BUF - 1 - total, 0);  // lee más datos
        if (n <= 0) break;                      // si se cierra o falla, se sale
        total += n;                             // suma lo leído
    }                                           // fin del while
    req[headerLen + contentLength] = '\0';      // cierra la cadena justo al final del cuerpo
    const char* body = req + headerLen;         // el cuerpo empieza después de las cabeceras

    // 4. Interpretar la primera línea de la petición: MÉTODO RUTA
    char method[8];                             // aquí queda el método (GET, POST, OPTIONS)
    char path[128];                             // aquí queda la ruta (/, /mensajes, /enviar)
    if (sscanf(req, "%7s %127s", method, path) != 2) {  // lee las dos primeras palabras sin pasarse del tamaño
        SendHttpText(s, "400 Bad Request", "text/plain", "Peticion mal formada");   // responde 400
        return;                                 // termina la función
    }                                           // fin del if

    printf("[WEB %d] Peticion %s %s\n", PUERTO_WEB, method, path);  // registra la petición en la consola

    // 5. Decidir qué hacer según el método y la ruta
    if (strcmp(method, "OPTIONS") == 0) {       // consulta previa (preflight) que hace el navegador por CORS
        SendHttp(s, "200 OK", "text/plain", "", 0);     // responde 200 solo con las cabeceras CORS
    } else if (strcmp(method, "GET") == 0 &&
               (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {  // piden la página web
        HandleGetIndex(s);                      // entrega index.html
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/ws") == 0) {      // piden pasar a WebSocket
        HandleWebSocket(s, req);                // se queda atendiendo tramas hasta que se desconecte
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/mensajes") == 0) {  // piden el historial
        HandleGetMensajes(s);                   // entrega el JSON
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/enviar") == 0) {   // envían un mensaje
        HandlePostEnviar(s, body);              // guarda y reparte el mensaje
    } else {                                    // cualquier otra ruta
        SendHttpText(s, "404 Not Found", "application/json",
                     "{\"error\":\"esa ruta no existe en este servidor\"}");    // responde 404
    }                                           // fin del if/else
}                                               // fin de ProcessHttpRequest

/* Hilo que atiende UNA petición HTTP. */
DWORD WINAPI HttpClientHandler(LPVOID lpParam) {    // lpParam apunta al socket del navegador
    SOCKET s = *(SOCKET*)lpParam;               // copia el socket
    free(lpParam);                              // libera la memoria que dejó el hilo que lo creó

    ProcessHttpRequest(s);                      // lee la petición y responde

    shutdown(s, SD_SEND);                       // avisa que ya no se enviarán más datos
    closesocket(s);                             // cierra el socket
    return 0;                                   // termina el hilo
}                                               // fin de HttpClientHandler

/* Hilo que escucha en el puerto 9090 y crea un hilo por cada petición. */
DWORD WINAPI HttpServerThread(LPVOID lpParam) { // no recibe datos útiles
    (void)lpParam;                              // evita la advertencia de "parámetro sin usar"
    struct sockaddr_in addr;                    // dirección donde escuchará el servidor web

    SOCKET httpListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);  // crea un socket TCP (IPv4)
    if (httpListen == INVALID_SOCKET) {         // ¿falló la creación?
        printf("[FALLO] Web: no se pudo crear el socket (%d)\n", WSAGetLastError());    // muestra el código de error
        return 1;                               // termina el hilo con error
    }                                           // fin del if

    addr.sin_family = AF_INET;                  // familia de direcciones: IPv4
    addr.sin_addr.s_addr = INADDR_ANY;          // escucha en todas las interfaces de red del equipo
    addr.sin_port = htons(PUERTO_WEB);          // puerto 9090 (htons lo pasa al orden de bytes de red)

    // bind asocia el socket con la dirección y el puerto
    if (bind(httpListen, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[FALLO] Web: el puerto %d no se pudo reservar (%d)\n", PUERTO_WEB, WSAGetLastError());  // error de bind
        closesocket(httpListen);                // cierra el socket
        return 1;                               // termina el hilo con error
    }                                           // fin del if
    if (listen(httpListen, SOMAXCONN) == SOCKET_ERROR) {    // pone el socket a esperar conexiones
        printf("[FALLO] Web: no se pudo poner en escucha (%d)\n", WSAGetLastError());   // error de listen
        closesocket(httpListen);                // cierra el socket
        return 1;                               // termina el hilo con error
    }                                           // fin del if

    printf("[SERVIDOR] Pagina web lista: abre http://localhost:%d en el navegador\n", PUERTO_WEB);  // aviso de que ya funciona

    while (1) {                                 // bucle infinito: atiende peticiones para siempre
        SOCKET c = accept(httpListen, NULL, NULL);  // espera a que un navegador se conecte
        if (c == INVALID_SOCKET) continue;      // si falló, intenta con el siguiente

        SOCKET* pSocket = (SOCKET*)malloc(sizeof(SOCKET));  // memoria propia para pasar el socket al hilo
        if (pSocket == NULL) {                  // ¿no hubo memoria?
            closesocket(c);                     // se descarta esta conexión
            continue;                           // y se sigue con la próxima
        }                                       // fin del if
        *pSocket = c;                           // guarda el socket en esa memoria

        HANDLE h = CreateThread(NULL, 0, HttpClientHandler, pSocket, 0, NULL);  // crea el hilo de esta petición
        if (h == NULL) {                        // ¿no se pudo crear el hilo?
            free(pSocket);                      // libera la memoria
            closesocket(c);                     // cierra la conexión
        } else {                                // el hilo se creó bien
            CloseHandle(h);                     // soltamos el handle (el hilo sigue corriendo solo)
        }                                       // fin del if/else
    }                                           // fin del while
    return 0;                                   // (no se llega aquí por el while infinito)
}                                               // fin de HttpServerThread

/* ============================================================
 *  PROGRAMA PRINCIPAL
 * ============================================================ */
int main(void) {                                // punto de entrada del programa
    WSADATA wsaData;                            // información que devuelve WinSock al iniciarse
    SOCKET listenSocket;                        // socket que espera clientes del chat TCP
    struct sockaddr_in serverAddr, clientAddr;  // dirección del servidor y dirección del cliente que llega
    int clientAddrLen = sizeof(clientAddr);     // tamaño de la dirección del cliente (lo usa accept)

    // 1. Encender WinSock (pedimos la versión 2.2)
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[FALLO] No arranco WinSock (%d)\n", WSAGetLastError());     // muestra el código de error
        return 1;                               // sale del programa con error
    }                                           // fin del if

    InitializeCriticalSection(&lockClients);    // prepara el candado de la lista de clientes
    InitializeCriticalSection(&lockMsgs);       // prepara el candado del historial de mensajes
    InitializeCriticalSection(&lockWs);         // prepara el candado de los navegadores WebSocket

    // 2. Crear el socket del chat
    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);   // socket TCP sobre IPv4
    if (listenSocket == INVALID_SOCKET) {       // ¿falló la creación?
        printf("[FALLO] No se pudo crear el socket del chat (%d)\n", WSAGetLastError());    // error
        WSACleanup();                           // apaga WinSock
        return 1;                               // sale con error
    }                                           // fin del if

    // 3. Indicar en qué dirección y puerto escuchará
    serverAddr.sin_family = AF_INET;            // familia de direcciones: IPv4
    serverAddr.sin_addr.s_addr = INADDR_ANY;    // todas las interfaces de red del equipo
    serverAddr.sin_port = htons(PUERTO_CHAT);   // puerto 6000 en orden de bytes de red

    // 4. bind: reservar el puerto 6000 para este socket
    if (bind(listenSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printf("[FALLO] El puerto %d no se pudo reservar (%d)\n", PUERTO_CHAT, WSAGetLastError());  // error
        closesocket(listenSocket);              // cierra el socket
        WSACleanup();                           // apaga WinSock
        return 1;                               // sale con error
    }                                           // fin del if

    // 5. listen: empezar a aceptar conexiones entrantes
    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        printf("[FALLO] No se pudo poner el chat en escucha (%d)\n", WSAGetLastError());    // error
        closesocket(listenSocket);              // cierra el socket
        WSACleanup();                           // apaga WinSock
        return 1;                               // sale con error
    }                                           // fin del if

    printf("[SERVIDOR] Chat TCP escuchando en el puerto %d. Esperando usuarios...\n", PUERTO_CHAT);     // aviso

    // arranca el servidor web en su propio hilo para que funcione a la vez que el chat
    HANDLE hHttp = CreateThread(NULL, 0, HttpServerThread, NULL, 0, NULL);
    if (hHttp != NULL) CloseHandle(hHttp);      // soltamos el handle: el hilo sigue corriendo

    printf("[SERVIDOR] Escribe aqui y presiona Enter para hablarle a todos.\n");    // instrucción para quien use la consola

    // arranca el hilo que lee lo que se escriba en esta consola
    HANDLE hConsole = CreateThread(NULL, 0, ServerConsole, NULL, 0, NULL);
    if (hConsole != NULL) CloseHandle(hConsole);    // soltamos el handle

    // 6. Bucle principal: aceptar usuarios y crear un hilo para cada uno
    while (1) {                                 // bucle infinito
        // accept se queda esperando hasta que alguien se conecte y devuelve su socket
        SOCKET clientSocket = accept(listenSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
        if (clientSocket == INVALID_SOCKET) {   // ¿falló accept?
            printf("[FALLO] accept no pudo aceptar la conexion (%d)\n", WSAGetLastError()); // error
            continue;                           // intenta con la siguiente conexión
        }                                       // fin del if

        printf("[RED] Nuevo usuario conectado desde %s\n", inet_ntoa(clientAddr.sin_addr));     // muestra su IP

        // si ya hay MAX_CLIENTS conectados, AddClient devuelve 0
        if (!AddClient(clientSocket)) {
            const char* lleno = "El chat esta lleno por ahora. Vuelve a intentarlo en un rato.\n";  // aviso al cliente
            send(clientSocket, lleno, (int)strlen(lleno), 0);   // se lo enviamos
            closesocket(clientSocket);          // cerramos esa conexión
            printf("[RED] Cupo completo: conexion rechazada.\n");   // aviso en la consola del servidor
            continue;                           // seguimos esperando a otros
        }                                       // fin del if

        // reservamos memoria propia para pasar el socket al hilo (así no se pisa con el siguiente cliente)
        SOCKET* pSocket = (SOCKET*)malloc(sizeof(SOCKET));
        if (pSocket == NULL) {                  // ¿no hubo memoria?
            RemoveClient(clientSocket);         // lo sacamos de la lista
            closesocket(clientSocket);          // cerramos la conexión
            continue;                           // seguimos con el siguiente
        }                                       // fin del if
        *pSocket = clientSocket;                // guardamos el socket en esa memoria

        // creamos el hilo que atenderá a este usuario con la función ClientHandler
        HANDLE hThread = CreateThread(NULL, 0, ClientHandler, pSocket, 0, NULL);
        if (hThread == NULL) {                  // ¿no se pudo crear el hilo?
            printf("[FALLO] No se pudo crear el hilo del usuario (%lu)\n", GetLastError());     // error
            free(pSocket);                      // liberamos la memoria
            RemoveClient(clientSocket);         // lo sacamos de la lista
            closesocket(clientSocket);          // cerramos la conexión
        } else {                                // el hilo se creó bien
            CloseHandle(hThread);               // soltamos el handle: el hilo sigue corriendo solo
        }                                       // fin del if/else
    }                                           // fin del while

    // (con el while infinito no se llega aquí, pero se deja el cierre ordenado)
    closesocket(listenSocket);                  // cierra el socket del chat
    DeleteCriticalSection(&lockClients);        // destruye el candado de clientes
    DeleteCriticalSection(&lockMsgs);           // destruye el candado del historial
    DeleteCriticalSection(&lockWs);             // destruye el candado de WebSocket
    WSACleanup();                               // apaga WinSock
    return 0;                                   // fin del programa
}                                               // fin de main
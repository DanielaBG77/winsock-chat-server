/*
 * cliente.c - Cliente de chat TCP para Windows (WinSock2 + hilos Win32)
 * Taller de Sistemas Operativos - UNINTEP
 *
 * Se conecta al puerto 6000 del servidor. Usa dos hilos:
 *   - el principal lee lo que escribes y lo envía,
 *   - el de recepción muestra lo que llega de los demás.
 *
 * Compilar:  gcc cliente.c -o cliente.exe -lws2_32
 * Usar:      .\cliente.exe               (se conecta a 127.0.0.1, el mismo equipo)
 *            .\cliente.exe 192.168.1.10  (se conecta a otro equipo de la red)
 */

#include <stdio.h>      // printf, snprintf, fgets, fflush (entrada y salida)
#include <stdlib.h>     // exit (terminar el programa)
#include <string.h>     // strcspn, strcmp (manejo de cadenas)
#include <winsock2.h>   // API de sockets de Windows
#include <windows.h>    // API Win32: hilos (CreateThread, CloseHandle)

#define PUERTO_CHAT 6000    // puerto TCP del servidor (antes era 5000)
#define BUFFER_SIZE 1024    // tamaño del buffer para leer y recibir texto

/* Hilo que SOLO se dedica a recibir lo que manda el servidor. */
DWORD WINAPI ReceiveThread(LPVOID lpParam) {    // lpParam apunta al socket del cliente
    SOCKET s = *(SOCKET*)lpParam;               // copia el socket para usarlo en este hilo
    char buffer[BUFFER_SIZE];                   // zona donde llegan los datos
    int n;                                      // cantidad de bytes recibidos en cada lectura

    // recv espera datos; devuelve >0 si llegaron, 0 si el servidor cerró y <0 si hubo error
    while ((n = recv(s, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[n] = '\0';                       // cierra el texto para tratarlo como cadena C
        printf("\r%s", buffer);                 // \r vuelve al inicio de la línea para no mezclarse con lo que escribes
        fflush(stdout);                         // fuerza que el texto se vea de inmediato
    }                                           // fin del while: el servidor cerró o falló la conexión

    printf("\n[CLIENTE] El servidor cerro la conexion.\n");     // avisa que ya no hay conexión
    exit(0);                                    // termina todo el programa (el hilo principal estaba esperando teclado)
    return 0;                                   // (no se alcanza por el exit, pero el hilo debe devolver un valor)
}                                               // fin de ReceiveThread

int main(int argc, char* argv[]) {              // argc/argv permiten recibir la IP al ejecutar
    WSADATA wsaData;                            // información que devuelve WinSock al iniciarse
    SOCKET s;                                   // socket de este cliente
    struct sockaddr_in server;                  // dirección del servidor al que nos conectamos
    char buffer[BUFFER_SIZE];                   // aquí se guarda lo que escribes
    char nombre[50];                            // nombre que usarás en el chat
    char mensaje[BUFFER_SIZE + 60];             // mensaje final "nombre: texto" (con holgura para el nombre)
    const char* ip = (argc > 1) ? argv[1] : "127.0.0.1";    // IP dada al ejecutar o, si no hay, este mismo equipo

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {    // enciende WinSock (versión 2.2)
        printf("[FALLO] No arranco WinSock (%d)\n", WSAGetLastError());     // muestra el código de error
        return 1;                               // sale con error
    }                                           // fin del if

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);      // crea un socket TCP sobre IPv4
    if (s == INVALID_SOCKET) {                  // ¿falló la creación?
        printf("[FALLO] No se pudo crear el socket (%d)\n", WSAGetLastError());     // error
        WSACleanup();                           // apaga WinSock
        return 1;                               // sale con error
    }                                           // fin del if

    server.sin_family = AF_INET;                // familia de direcciones: IPv4
    server.sin_addr.s_addr = inet_addr(ip);     // convierte el texto de la IP al formato numérico de red
    server.sin_port = htons(PUERTO_CHAT);       // puerto 6000 en orden de bytes de red

    // connect intenta establecer la conexión con el servidor
    if (connect(s, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) {
        printf("[FALLO] No pude conectarme a %s:%d (codigo %d). Esta encendido el servidor?\n",
               ip, PUERTO_CHAT, WSAGetLastError());     // muestra IP, puerto y código de error
        closesocket(s);                         // cierra el socket
        WSACleanup();                           // apaga WinSock
        return 1;                               // sale con error
    }                                           // fin del if

    printf("[CLIENTE] Conexion establecida con %s:%d\n", ip, PUERTO_CHAT);     // confirma la conexión

    printf("Como quieres llamarte en el chat? ");   // pide el nombre
    if (fgets(nombre, sizeof(nombre), stdin) == NULL) {     // lee el nombre; NULL significa que se cerró la entrada
        closesocket(s);                         // cierra el socket
        WSACleanup();                           // apaga WinSock
        return 1;                               // sale
    }                                           // fin del if
    nombre[strcspn(nombre, "\r\n")] = '\0';     // quita el salto de línea que deja fgets al final

    // crea el hilo que recibirá los mensajes mientras tú escribes
    HANDLE hRecv = CreateThread(NULL, 0, ReceiveThread, &s, 0, NULL);
    if (hRecv == NULL) {                        // ¿no se pudo crear el hilo?
        printf("[FALLO] No se pudo crear el hilo que recibe mensajes.\n");  // error
        closesocket(s);                         // cierra el socket
        WSACleanup();                           // apaga WinSock
        return 1;                               // sale con error
    }                                           // fin del if

    printf("Ya puedes chatear. Escribe 'salir' para cerrar tu sesion.\n");  // instrucciones de uso

    // lee línea por línea lo que escribes hasta que se cierre la entrada
    while (fgets(buffer, sizeof(buffer), stdin) != NULL) {
        buffer[strcspn(buffer, "\r\n")] = '\0'; // quita el salto de línea del final
        if (strcmp(buffer, "salir") == 0) break;    // si escribes "salir", se sale del bucle
        if (buffer[0] == '\0') continue;        // si la línea está vacía, no se envía nada

        // arma el mensaje con el formato "nombre: texto" (snprintf devuelve el largo escrito)
        int len = snprintf(mensaje, sizeof(mensaje), "%s: %s\n", nombre, buffer);
        send(s, mensaje, len, 0);               // envía el mensaje al servidor
    }                                           // fin del while

    closesocket(s);                             // cierra la conexión con el servidor
    CloseHandle(hRecv);                         // libera el handle del hilo de recepción
    WSACleanup();                               // apaga WinSock
    printf("[CLIENTE] Sesion terminada.\n");    // mensaje de despedida
    return 0;                                   // fin del programa
}                                               // fin de main
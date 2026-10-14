/*
 * websocket.h - WebSocket (RFC 6455) en C puro, sin librerias externas.
 * Taller de Sistemas Operativos - UNINTEP  (RETO AVANZADO)
 *
 * Contiene SOLO funciones puras (no usan sockets), asi se pueden probar aparte:
 *   1) Sha1()            -> hash SHA-1 (RFC 3174)
 *   2) Base64Encode()    -> codificacion Base64
 *   3) WsAcceptKey()     -> Sec-WebSocket-Key  ->  Sec-WebSocket-Accept (handshake)
 *   4) WsEncodeFrame()   -> arma una trama del servidor (sin mascara)
 *   5) WsDecodeFrame()   -> lee una trama del cliente y le quita la mascara (unmasking)
 */
#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <stdint.h>   // uint8_t, uint32_t, uint64_t (enteros de tamano exacto)
#include <string.h>   // memcpy, strlen

#define WS_GUID        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"  // constante fija que define el RFC 6455
#define WS_MAX_PAYLOAD 4096     // tramas con mas de 4 KB se rechazan (proteccion de memoria)

#define WS_OP_TEXT  0x1         // trama de texto
#define WS_OP_CLOSE 0x8         // el cliente cierra la conexion
#define WS_OP_PING  0x9         // "sigues ahi?"
#define WS_OP_PONG  0xA         // respuesta a un ping

/* ============================================================
 *  1) SHA-1
 * ============================================================ */

static uint32_t Rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }   // rotacion a la izquierda

/* Calcula el SHA-1 de 'data' (len bytes) y deja 20 bytes en 'out'. */
static void Sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

    // El mensaje se completa con: un bit 1, ceros, y el largo en bits (8 bytes). Total multiplo de 64.
    size_t total = ((len + 8) / 64 + 1) * 64;           // tamano final del mensaje rellenado
    uint8_t block[128];                                 // para claves cortas bastan 2 bloques
    if (total > sizeof(block)) {                        // este modulo solo hashea textos cortos (<= 119 bytes)
        memset(out, 0, 20);
        return;
    }
    memset(block, 0, sizeof(block));                    // todo en ceros
    memcpy(block, data, len);                           // copia el mensaje original
    block[len] = 0x80;                                  // bit 1 despues del mensaje
    uint64_t bits = (uint64_t)len * 8;                  // largo en bits
    for (int i = 0; i < 8; i++)                         // largo en big-endian al final
        block[total - 1 - i] = (uint8_t)(bits >> (8 * i));

    for (size_t off = 0; off < total; off += 64) {      // procesa cada bloque de 64 bytes
        uint32_t w[80];
        for (int i = 0; i < 16; i++)                    // 16 palabras de 32 bits (big-endian)
            w[i] = ((uint32_t)block[off + i * 4] << 24) | ((uint32_t)block[off + i * 4 + 1] << 16) |
                   ((uint32_t)block[off + i * 4 + 2] << 8) | (uint32_t)block[off + i * 4 + 3];
        for (int i = 16; i < 80; i++)                   // expande a 80 palabras
            w[i] = Rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {                  // 80 rondas
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t t = Rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol32(b, 30); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;   // suma al estado
    }

    uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++) {                       // escribe el resultado en big-endian
        out[i * 4]     = (uint8_t)(hs[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(hs[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(hs[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(hs[i]);
    }
}

/* ============================================================
 *  2) BASE64
 * ============================================================ */

/* Codifica 'len' bytes en Base64. 'out' necesita 4*((len+2)/3)+1 bytes. */
static void Base64Encode(const uint8_t* in, size_t len, char* out) {
    static const char tabla[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {               // de 3 en 3 bytes -> 4 caracteres
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t)in[i + 2];
        out[j++] = tabla[(v >> 18) & 63];
        out[j++] = tabla[(v >> 12) & 63];
        out[j++] = (i + 1 < len) ? tabla[(v >> 6) & 63] : '=';   // relleno '=' si faltan bytes
        out[j++] = (i + 2 < len) ? tabla[v & 63] : '=';
    }
    out[j] = '\0';
}

/* ============================================================
 *  3) HANDSHAKE
 * ============================================================ */

/* Recibe la Sec-WebSocket-Key del navegador y escribe en 'out' (min. 32 bytes)
 * el valor de Sec-WebSocket-Accept = Base64( SHA1( clave + GUID ) ).
 * Devuelve 1 si todo salio bien y 0 si la clave es invalida. */
static int WsAcceptKey(const char* clientKey, char* out) {
    char concat[128];
    size_t kl = strlen(clientKey);
    if (kl == 0 || kl > 40) return 0;                   // la clave real mide 24 caracteres
    memcpy(concat, clientKey, kl);                      // clave del navegador
    memcpy(concat + kl, WS_GUID, strlen(WS_GUID));      // + GUID fijo del RFC
    uint8_t digest[20];
    Sha1((const uint8_t*)concat, kl + strlen(WS_GUID), digest);
    Base64Encode(digest, 20, out);                      // 20 bytes -> 28 caracteres
    return 1;
}

/* ============================================================
 *  4) CODIFICAR TRAMA (servidor -> navegador, SIN mascara)
 * ============================================================ */

/* Arma una trama completa en 'out'. Devuelve su largo, o -1 si no cabe. */
static int WsEncodeFrame(int opcode, const uint8_t* payload, size_t len, uint8_t* out, size_t outCap) {
    size_t head;
    if (len > WS_MAX_PAYLOAD) return -1;
    if (len < 126) {                                    // largo cabe en 7 bits
        if (outCap < len + 2) return -1;
        out[0] = (uint8_t)(0x80 | (opcode & 0x0F));     // FIN=1 + opcode
        out[1] = (uint8_t)len;                          // MASK=0 + largo
        head = 2;
    } else {                                            // largo en 16 bits (hasta 65535)
        if (outCap < len + 4) return -1;
        out[0] = (uint8_t)(0x80 | (opcode & 0x0F));
        out[1] = 126;
        out[2] = (uint8_t)(len >> 8);
        out[3] = (uint8_t)(len & 0xFF);
        head = 4;
    }
    if (len > 0) memcpy(out + head, payload, len);      // el servidor NO enmascara el contenido
    return (int)(head + len);
}

/* ============================================================
 *  5) DECODIFICAR TRAMA (navegador -> servidor, CON mascara)
 * ============================================================ */

typedef struct {
    int      opcode;        // tipo de trama (texto, ping, cierre...)
    uint8_t* payload;       // contenido ya desenmascarado (apunta DENTRO del buffer de entrada)
    size_t   payloadLen;    // largo del contenido
} WsFrame;

/* Intenta leer UNA trama del buffer 'in' (inLen bytes). Desenmascara en el mismo lugar.
 * Devuelve: >0 = bytes consumidos (trama completa)
 *            0 = todavia faltan bytes (hay que hacer otro recv)
 *           -1 = trama invalida (cliente sin mascara, fragmentada o demasiado grande) */
static int WsDecodeFrame(uint8_t* in, size_t inLen, WsFrame* f) {
    if (inLen < 2) return 0;                            // ni siquiera esta el encabezado minimo
    int fin    = (in[0] & 0x80) != 0;                   // bit FIN
    int rsv    = in[0] & 0x70;                          // bits reservados: deben ser 0
    int opcode = in[0] & 0x0F;                          // tipo de trama
    int masked = (in[1] & 0x80) != 0;                   // bit MASK
    uint64_t len = in[1] & 0x7F;                        // largo (o indicador 126/127)
    size_t pos = 2;

    if (!masked || rsv != 0) return -1;                 // el RFC obliga al cliente a enmascarar
    if (!fin || opcode == 0) return -1;                 // este servidor no soporta fragmentacion

    if (len == 126) {                                   // largo en los 2 bytes siguientes
        if (inLen < 4) return 0;
        len = ((uint64_t)in[2] << 8) | in[3];
        pos = 4;
    } else if (len == 127) {                            // largo de 8 bytes: nunca lo aceptamos (> WS_MAX_PAYLOAD)
        return -1;
    }
    if (len > WS_MAX_PAYLOAD) return -1;                // proteccion contra desbordamiento de buffer
    if (opcode >= 8 && len > 125) return -1;            // las tramas de control (ping/close) miden maximo 125 bytes

    if (inLen < pos + 4 + (size_t)len) return 0;        // faltan la mascara (4 bytes) y/o el contenido

    uint8_t* mask = in + pos;                           // 4 bytes de mascara
    uint8_t* data = in + pos + 4;                       // contenido enmascarado
    for (size_t i = 0; i < (size_t)len; i++)            // UNMASKING: byte ^ mascara[i % 4]
        data[i] ^= mask[i % 4];

    f->opcode = opcode;
    f->payload = data;
    f->payloadLen = (size_t)len;
    return (int)(pos + 4 + (size_t)len);
}

#endif // WEBSOCKET_H

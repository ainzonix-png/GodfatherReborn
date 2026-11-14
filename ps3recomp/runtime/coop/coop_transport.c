#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ps3emu/coop_net.h"
#include "coop_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET coop_socket;
#define COOP_BAD_SOCKET INVALID_SOCKET
#define coop_close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int coop_socket;
#define COOP_BAD_SOCKET (-1)
#define coop_close close
#endif

#define COOP_DEFAULT_PORT       29715u
#define COOP_DEFAULT_TIMEOUT_MS 3000u
#define COOP_HEARTBEAT_MS       500u

typedef struct coop_context {
    ps3emu_coop_mode mode;
    coop_socket socket;
    struct sockaddr_in peer;
    int has_peer;
    int connected;
    int has_rx_sequence;
    int has_remote_state;
    uint32_t local_session;
    uint32_t peer_session;
    uint32_t tx_sequence;
    uint32_t rx_sequence;
    uint32_t rejected_packets;
    uint32_t timeout_ms;
    uint64_t now_ms;
    uint64_t last_receive_ms;
    uint64_t last_send_ms;
    ps3emu_coop_remote_state remote;
    uint64_t blob_sent_bytes;
    uint64_t blob_received_bytes;
    char error[160];
#ifdef _WIN32
    int winsock_started;
#endif
} coop_context;

static coop_context g_coop;
static int g_coop_initialized;
static ps3emu_coop_blob_handler g_blob_handler;
static void *g_blob_user;

static void set_error(const char *message)
{
    snprintf(g_coop.error, sizeof(g_coop.error), "%s", message ? message : "unknown error");
}

static unsigned parse_number(const char *name, unsigned fallback,
                             unsigned minimum, unsigned maximum)
{
    const char *text = getenv(name);
    char *end;
    unsigned long value;
    if (!text || !*text)
        return fallback;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *end || value < minimum || value > maximum)
        return fallback;
    return (unsigned)value;
}

static int set_nonblocking(coop_socket socket_value)
{
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket_value, FIONBIO, &enabled) == 0;
#else
    int flags = fcntl(socket_value, F_GETFL, 0);
    return flags >= 0 && fcntl(socket_value, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static int address_equal(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static int would_block(void)
{
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static int send_packet(uint16_t type, const ps3emu_coop_player_state *state)
{
    uint8_t bytes[INFAMOUS_COOP_PACKET_MAX_SIZE];
    size_t size;
    int sent;

    if (g_coop.socket == COOP_BAD_SOCKET || !g_coop.has_peer)
        return 0;
    size = infamous_coop_protocol_encode(bytes, sizeof(bytes), type,
                                         ++g_coop.tx_sequence,
                                         g_coop.local_session, state);
    if (!size)
        return 0;
    sent = sendto(g_coop.socket, (const char *)bytes, (int)size, 0,
                  (const struct sockaddr *)&g_coop.peer, sizeof(g_coop.peer));
    if (sent != (int)size) {
#ifdef _WIN32
        snprintf(g_coop.error, sizeof(g_coop.error), "UDP send failed (%d)", WSAGetLastError());
#else
        snprintf(g_coop.error, sizeof(g_coop.error), "UDP send failed (%d)", errno);
#endif
        return 0;
    }
    g_coop.last_send_ms = g_coop.now_ms;
    return 1;
}

static int send_bytes(const uint8_t *bytes, size_t size)
{
    int sent = sendto(g_coop.socket, (const char *)bytes, (int)size, 0,
                      (const struct sockaddr *)&g_coop.peer, sizeof(g_coop.peer));
    if (sent != (int)size) {
#ifdef _WIN32
        /* A full send buffer is a dropped datagram, not a broken link. */
        if (WSAGetLastError() == WSAEWOULDBLOCK) return 0;
        snprintf(g_coop.error, sizeof(g_coop.error), "UDP send failed (%d)", WSAGetLastError());
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        snprintf(g_coop.error, sizeof(g_coop.error), "UDP send failed (%d)", errno);
#endif
        return 0;
    }
    return 1;
}

static void reset_peer(void)
{
    g_coop.connected = 0;
    g_coop.peer_session = 0;
    g_coop.has_rx_sequence = 0;
    g_coop.has_remote_state = 0;
    if (g_coop.mode == PS3EMU_COOP_HOST)
        g_coop.has_peer = 0;
}

int ps3emu_coop_init(void)
{
    const char *mode_text;
    const char *address_text;
    unsigned port;
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct sockaddr_in bind_address;
    char service[16];

    if (g_coop_initialized)
        ps3emu_coop_shutdown();
    memset(&g_coop, 0, sizeof(g_coop));
    g_coop.socket = COOP_BAD_SOCKET;
    g_coop_initialized = 1;
    mode_text = getenv("INFAMOUS_COOP_MODE");
    if (!mode_text || !*mode_text || strcmp(mode_text, "off") == 0)
        return 1;
    if (strcmp(mode_text, "host") == 0)
        g_coop.mode = PS3EMU_COOP_HOST;
    else if (strcmp(mode_text, "client") == 0)
        g_coop.mode = PS3EMU_COOP_CLIENT;
    else {
        set_error("INFAMOUS_COOP_MODE must be host, client, or off");
        return 0;
    }

#ifdef _WIN32
    {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            set_error("WSAStartup failed");
            g_coop.mode = PS3EMU_COOP_OFF;
            return 0;
        }
        g_coop.winsock_started = 1;
        g_coop.local_session = (uint32_t)GetTickCount() ^ (uint32_t)GetCurrentProcessId();
    }
#else
    g_coop.local_session = (uint32_t)getpid() ^ (uint32_t)(uintptr_t)&g_coop;
#endif
    if (!g_coop.local_session)
        g_coop.local_session = 1;
    g_coop.timeout_ms = parse_number("INFAMOUS_COOP_TIMEOUT_MS",
                                     COOP_DEFAULT_TIMEOUT_MS, 250, 60000);
    port = parse_number("INFAMOUS_COOP_PORT", COOP_DEFAULT_PORT, 1, 65535);
    g_coop.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_coop.socket == COOP_BAD_SOCKET || !set_nonblocking(g_coop.socket)) {
        set_error("could not create a nonblocking UDP socket");
        ps3emu_coop_shutdown();
        return 0;
    }

    {
        /* Live pose frames arrive as bursts of ~40-80 datagrams. */
        int buffer_bytes = 4 * 1024 * 1024;
        setsockopt(g_coop.socket, SOL_SOCKET, SO_RCVBUF, (const char *)&buffer_bytes, sizeof(buffer_bytes));
        setsockopt(g_coop.socket, SOL_SOCKET, SO_SNDBUF, (const char *)&buffer_bytes, sizeof(buffer_bytes));
    }
#ifdef _WIN32
    {
        /* Without this, an ICMP port-unreachable (peer restarting) makes every
         * later recvfrom fail with WSAECONNRESET on Windows. */
        BOOL report = FALSE;
        DWORD returned = 0;
        WSAIoctl(g_coop.socket, _WSAIOW(IOC_VENDOR, 12), &report, sizeof(report),
                 NULL, 0, &returned, NULL, NULL);
    }
#endif
    memset(&bind_address, 0, sizeof(bind_address));
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_address.sin_port = htons(g_coop.mode == PS3EMU_COOP_HOST ? (uint16_t)port : 0);
    if (bind(g_coop.socket, (const struct sockaddr *)&bind_address,
             sizeof(bind_address)) != 0) {
        set_error("could not bind the co-op UDP socket");
        ps3emu_coop_shutdown();
        return 0;
    }

    if (g_coop.mode == PS3EMU_COOP_CLIENT) {
        address_text = getenv("INFAMOUS_COOP_ADDRESS");
        if (!address_text || !*address_text)
            address_text = "127.0.0.1";
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        snprintf(service, sizeof(service), "%u", port);
        if (getaddrinfo(address_text, service, &hints, &addresses) != 0 || !addresses) {
            set_error("could not resolve INFAMOUS_COOP_ADDRESS");
            ps3emu_coop_shutdown();
            return 0;
        }
        memcpy(&g_coop.peer, addresses->ai_addr, sizeof(g_coop.peer));
        freeaddrinfo(addresses);
        g_coop.has_peer = 1;
    }
    return 1;
}

void ps3emu_coop_shutdown(void)
{
    char saved_error[sizeof(g_coop.error)];
    if (!g_coop_initialized)
        return;
    memcpy(saved_error, g_coop.error, sizeof(saved_error));
    if (g_coop.socket != COOP_BAD_SOCKET)
        coop_close(g_coop.socket);
#ifdef _WIN32
    if (g_coop.winsock_started)
        WSACleanup();
#endif
    memset(&g_coop, 0, sizeof(g_coop));
    g_coop.mode = PS3EMU_COOP_OFF;
    g_coop.socket = COOP_BAD_SOCKET;
    memcpy(g_coop.error, saved_error, sizeof(g_coop.error));
}

void ps3emu_coop_tick(uint64_t now_ms)
{
    uint8_t bytes[INFAMOUS_COOP_PACKET_MAX_SIZE + 1];
    struct sockaddr_in sender;
    infamous_coop_decoded_packet packet;
    int received;

    if (g_coop.mode == PS3EMU_COOP_OFF)
        return;
    g_coop.now_ms = now_ms;
    for (;;) {
#ifdef _WIN32
        int sender_size = (int)sizeof(sender);
#else
        socklen_t sender_size = (socklen_t)sizeof(sender);
#endif
        received = recvfrom(g_coop.socket, (char *)bytes, sizeof(bytes), 0,
                            (struct sockaddr *)&sender, &sender_size);
        if (received < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEMSGSIZE)
                continue;
#endif
            if (!would_block())
                set_error("UDP receive failed");
            break;
        }
        if (!infamous_coop_protocol_decode(bytes, (size_t)received, &packet)) {
            ++g_coop.rejected_packets;
            continue;
        }
        if (g_coop.mode == PS3EMU_COOP_CLIENT &&
            !address_equal(&sender, &g_coop.peer)) {
            ++g_coop.rejected_packets;
            continue;
        }
        if (g_coop.mode == PS3EMU_COOP_HOST && g_coop.has_peer &&
            !address_equal(&sender, &g_coop.peer)) {
            ++g_coop.rejected_packets;
            continue;
        }
        if (packet.type == INFAMOUS_COOP_MESSAGE_HELLO) {
            if (!g_coop.has_peer) {
                g_coop.peer = sender;
                g_coop.has_peer = 1;
            }
            g_coop.peer_session = packet.session;
            g_coop.has_rx_sequence = 0;
            g_coop.has_remote_state = 0;
            g_coop.connected = 1;
            g_coop.last_receive_ms = now_ms;
            if (g_coop.mode == PS3EMU_COOP_HOST)
                send_packet(INFAMOUS_COOP_MESSAGE_HELLO, NULL);
            continue;
        }
        if (!g_coop.connected || packet.session != g_coop.peer_session ||
            (g_coop.has_rx_sequence && (int32_t)(packet.sequence - g_coop.rx_sequence) <= 0)) {
            ++g_coop.rejected_packets;
            continue;
        }
        g_coop.has_rx_sequence = 1;
        g_coop.rx_sequence = packet.sequence;
        g_coop.last_receive_ms = now_ms;
        if (packet.type == INFAMOUS_COOP_MESSAGE_BLOB) {
            g_coop.blob_received_bytes += packet.blob_size;
            if (g_blob_handler)
                g_blob_handler(packet.blob, packet.blob_size, now_ms, g_blob_user);
            continue;
        }
        if (packet.type == INFAMOUS_COOP_MESSAGE_STATE) {
            g_coop.remote.player = packet.state;
            g_coop.remote.sequence = packet.sequence;
            g_coop.remote.received_at_ms = now_ms;
            g_coop.has_remote_state = 1;
        }
    }

    if (g_coop.connected && now_ms - g_coop.last_receive_ms > g_coop.timeout_ms)
        reset_peer();
    if (now_ms - g_coop.last_send_ms >= COOP_HEARTBEAT_MS) {
        if (g_coop.mode == PS3EMU_COOP_CLIENT && !g_coop.connected)
            send_packet(INFAMOUS_COOP_MESSAGE_HELLO, NULL);
        else if (g_coop.connected)
            send_packet(INFAMOUS_COOP_MESSAGE_HEARTBEAT, NULL);
    }
}

int ps3emu_coop_send_local_state(const ps3emu_coop_player_state *state)
{
    return state && g_coop.connected &&
           send_packet(INFAMOUS_COOP_MESSAGE_STATE, state);
}

int ps3emu_coop_get_remote_state(ps3emu_coop_remote_state *state)
{
    if (!state || !g_coop.connected || !g_coop.has_remote_state)
        return 0;
    *state = g_coop.remote;
    return 1;
}

void ps3emu_coop_get_status(ps3emu_coop_status *status)
{
    if (!status)
        return;
    memset(status, 0, sizeof(*status));
    status->enabled = g_coop.mode != PS3EMU_COOP_OFF;
    status->mode = g_coop.mode;
    status->connected = g_coop.connected;
    status->peer_age_ms = g_coop.connected ? g_coop.now_ms - g_coop.last_receive_ms : 0;
    status->received_sequence = g_coop.rx_sequence;
    status->rejected_packets = g_coop.rejected_packets;
}

const char *ps3emu_coop_last_error(void)
{
    return g_coop.error;
}

void ps3emu_coop_set_blob_handler(ps3emu_coop_blob_handler handler, void *user)
{
    g_blob_handler = handler;
    g_blob_user = user;
}

int ps3emu_coop_send_blob(const void *data, uint32_t size)
{
    uint8_t bytes[INFAMOUS_COOP_PACKET_MAX_SIZE];
    size_t encoded;
    if (!g_coop.connected || g_coop.socket == COOP_BAD_SOCKET || !g_coop.has_peer)
        return 0;
    encoded = infamous_coop_protocol_encode_blob(bytes, sizeof(bytes), ++g_coop.tx_sequence,
                                                 g_coop.local_session, data, size);
    if (!encoded || !send_bytes(bytes, encoded))
        return 0;
    g_coop.blob_sent_bytes += size;
    return 1;
}

void ps3emu_coop_blob_counters(uint64_t *sent_bytes, uint64_t *received_bytes)
{
    if (sent_bytes) *sent_bytes = g_coop.blob_sent_bytes;
    if (received_bytes) *received_bytes = g_coop.blob_received_bytes;
}

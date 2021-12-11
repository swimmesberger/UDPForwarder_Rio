#include <stdio.h>
#include <conio.h>

#include "deps/Argtable3/argtable3.h"
#include "deps/RioSockets/Source/riosockets.h"

#define UDP_FORWARDER_VERSION "1.0.0"
#define MAX_QUEUE_LENGTH 256
// must be lower than MAX_QUEUE_LENGTH
// lower values reduce latency, higher values increase performance
#define SENDER_PACKET_BUFFER 256
// must be lower than MAX_QUEUE_LENGTH
// lower values reduce latency, higher values increase performance
#define RECEIVE_SENDER_PACKET_BUFFER 256
// size of a single packet
#define MAX_BUFFER_LENGTH 1024
// memory reserved for send buffer in multicast mode
#define SEND_BUFFER_SIZE (MAX_QUEUE_LENGTH * MAX_BUFFER_LENGTH)
// memory reserved for receive buffer in multicast mode
#define RECEIVE_BUFFER_SIZE (MAX_QUEUE_LENGTH * MAX_BUFFER_LENGTH)

// clients to broadcast to
RioSocket clients[10] = { 0 };
unsigned int client_count = 0;
unsigned int receive_sender_queue_size = 0;

/* global arg_xxx structs */
struct arg_lit *version;
struct arg_lit *help;
struct arg_lit *sender;
struct arg_str *input_address;
struct arg_str *output_address;
struct arg_end *end;

int riosockets_address_set_ip_port(RioAddress* dstAddress, const char* hostPort) {
    char hostAddr[INET6_ADDRSTRLEN];
    if(hostPort != NULL) {
        char* ptr = strrchr(hostPort, ':');
        if(!ptr) {
            strncpy_s(hostAddr, INET6_ADDRSTRLEN, hostPort, strlen(hostPort));
            dstAddress->port = 0;
        } else {
            rsize_t pos = ptr - hostPort;
            strncpy_s(hostAddr, INET6_ADDRSTRLEN, hostPort, pos);
            hostAddr[pos] = '\0';
            ptr += 1; // remove ':'
            long port = strtol(ptr, &ptr, 10);
            dstAddress->port = port;
        }
    } else {
        strcpy_s(hostAddr, INET6_ADDRSTRLEN, "0.0.0.0");
        dstAddress->port = 0;
    }

    if (riosockets_address_set_ip(dstAddress, hostAddr) != RIOSOCKETS_STATUS_OK){
        return RIOSOCKETS_STATUS_ERROR;
    }
    return RIOSOCKETS_STATUS_OK;
}

RioSocket riosockets_bind_ip_port(const char* hostPort, RioCallback callback, RioAddress* address, RioError* error) {
    if (riosockets_address_set_ip_port(address, hostPort) != RIOSOCKETS_STATUS_OK) {
        *error = RIOSOCKETS_ERROR_SOCKET_CREATION;
        return -1;
    }
    // only a single packet is used for the sending buffer because the server should not send a message
    // the server is used for receiving only
    RioSocket socket = riosockets_create(MAX_BUFFER_LENGTH, MAX_BUFFER_LENGTH, RECEIVE_BUFFER_SIZE, callback, error);
    if (riosockets_bind(socket, address) != 0) {
        *error = RIOSOCKETS_ERROR_SOCKET_CREATION;
        return -1;
    }
    if(riosockets_address_get(socket, address) != 0) {
        *error = RIOSOCKETS_ERROR_SOCKET_CREATION;
        return -1;
    }
    return socket;
}

RioSocket riosockets_connect_ip_port(const char* hostPort, RioCallback callback, RioAddress* address, RioError* error) {
    if (riosockets_address_set_ip_port(address, hostPort) != RIOSOCKETS_STATUS_OK) {
        *error = RIOSOCKETS_ERROR_SOCKET_CREATION;
        return -1;
    }
    // only a single packet is used for the receiving buffer because the clients should normally not receive a message
    // they are used for sending only
    RioSocket socket = riosockets_create(MAX_BUFFER_LENGTH, SEND_BUFFER_SIZE, MAX_BUFFER_LENGTH, callback, error);
    if (riosockets_connect(socket, address) != 0) {
        *error = RIOSOCKETS_ERROR_SOCKET_CREATION;
        return -1;
    }
    return socket;
}

#pragma clang diagnostic push
#pragma ide diagnostic ignored "UnusedParameter"
void server_callback(RioSocket server, const RioAddress* address, const uint8_t* data, int dataLength, RioType type) {
    if (type == RIOSOCKETS_TYPE_RECEIVE) {
        char ip[RIOSOCKETS_HOSTNAME_SIZE] = { 0 };

        riosockets_address_get_ip(address, ip, sizeof(ip));

#if defined(_DEBUG)
        printf("Message received from - IP: %s, Data length: %i\n", ip, dataLength);
#endif
        for(int i = 0; i<client_count; i++) {
            RioSocket client = clients[i];
            uint8_t* buffer = riosockets_buffer(client, NULL, dataLength);
            memcpy(buffer, data, dataLength);
            receive_sender_queue_size += 1;
            if(receive_sender_queue_size >= RECEIVE_SENDER_PACKET_BUFFER) {
#if defined(_DEBUG)
                printf("Flushing send queue (size=%d)\n", receive_sender_queue_size);
#endif
                riosockets_send(client);
                receive_sender_queue_size = 0;
            }
        }
        return;
    }
#if defined(_DEBUG)
    printf("Message sending was failed!\n");
#endif
}
#pragma clang diagnostic pop

#pragma clang diagnostic push
#pragma ide diagnostic ignored "UnusedParameter"
void client_callback(RioSocket client, const RioAddress* address, const uint8_t* data, int dataLength, RioType type) {
    // ignore message when message is received, client is write only - no reads
}
#pragma clang diagnostic pop

int multicast_mode(void) {
    printf("Starting in Multicast mode\n");

    const char* serverHost = input_address->count > 0 ? input_address->sval[0] : "127.0.0.1:0";
    RioError server_error = RIOSOCKETS_ERROR_NONE;
    RioAddress server_address = { 0 };
    RioSocket server = riosockets_bind_ip_port(serverHost, server_callback, &server_address, &server_error);
    if (server_error != RIOSOCKETS_ERROR_NONE) {
        printf("Server Socket creation failed! Error code: %i\n", server_error);
        return -1;
    }
    char server_ip_addr_str[INET6_ADDRSTRLEN];
    riosockets_address_get_ip(&server_address, server_ip_addr_str, INET6_ADDRSTRLEN);
    printf("Server Socket bound %s:%d\n", server_ip_addr_str, server_address.port);

    for(int i = 0; i<output_address->count; i++) {
        const char* client_host = output_address->sval[i];
        RioError client_error = RIOSOCKETS_ERROR_NONE;
        RioAddress client_address = { 0 };
        RioSocket new_client = riosockets_connect_ip_port(client_host, client_callback, &client_address, &client_error);
        if (client_error != RIOSOCKETS_ERROR_NONE) {
            printf("Client Socket creation failed! Error code: %i\n", client_error);
            riosockets_destroy(&server);
            return -1;
        }
        char client_ip_addr_str[INET6_ADDRSTRLEN];
        riosockets_address_get_ip(&client_address, client_ip_addr_str, INET6_ADDRSTRLEN);
        printf("Client connected to %s:%d\n", client_ip_addr_str, client_address.port);
        clients[i] = new_client;
        client_count += 1;
    }

    while (!_kbhit()) {
        riosockets_receive(server, RIOSOCKETS_MAX_COMPLETION_RESULTS);
    }

    riosockets_destroy(&server);
    for(int i = 0; i<client_count; i++) {
        riosockets_destroy(&clients[i]);
    }

    return 0;
}

int sender_mode(void) {
    printf("Starting in Sender mode\n");

    const char* client_host = output_address->sval[0];
    RioError client_error = RIOSOCKETS_ERROR_NONE;
    RioAddress client_address = { 0 };
    RioSocket client = riosockets_connect_ip_port(client_host, client_callback, &client_address, &client_error);
    if (client_error != RIOSOCKETS_ERROR_NONE) {
        printf("Client Socket creation failed! Error code: %i\n", client_error);
        return -1;
    }
    char client_ip_addr_str[INET6_ADDRSTRLEN];
    riosockets_address_get_ip(&client_address, client_ip_addr_str, INET6_ADDRSTRLEN);
    printf("Client connected to %s:%d\n", client_ip_addr_str, client_address.port);

    unsigned int queueSize = 0;
    while (!_kbhit()) {
        uint8_t *buffer = riosockets_buffer(client, NULL, MAX_BUFFER_LENGTH);
        memset(buffer, 0, MAX_BUFFER_LENGTH);
        queueSize += 1;
        if(queueSize >= SENDER_PACKET_BUFFER) {
#if defined(_DEBUG)
            printf("Flushing send queue (size=%d)\n", queueSize);
#endif
            riosockets_send(client);
            queueSize = 0;
        }
    }

    return 0;
}

int main(int argc, char* argv[]) {
    /* the global arg_xxx structs are initialised within the argtable */
    void *argtable[] = {
            help    = arg_litn(NULL, "help", 0, 1, "display this help and exit"),
            version = arg_litn(NULL, "version", 0, 1, "display version info and exit"),
            sender = arg_litn("s", "sending", 0, 1, "Switches to sending mode"),
            input_address = arg_str0("i", "input-address", NULL, "The address the server should listen on"),
            output_address = arg_strn("o", "output-address", NULL, 0, 10, "The address the server should send data to (allowed multiple times)"),
            end     = arg_end(20),
    };

    char* prog_name = argv[0];
    int nerrors = arg_parse(argc,argv,argtable);
    /* special case: '--help' takes precedence over error reporting */
    if (help->count > 0) {
        printf("Usage: %s", prog_name);
        arg_print_syntax(stdout, argtable, "\n");
        printf("Usage of UDPForwarder.\n\n");
        arg_print_glossary(stdout, argtable, "  %-25s %s\n");
        /* deallocate each non-null entry in argtable[] */
        arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
        return 0;
    }

    /* If the parser returned any errors then display them and exit */
    if (nerrors > 0) {
        /* Display the error details contained in the arg_end struct.*/
        arg_print_errors(stdout, end, prog_name);
        printf("Try '%s --help' for more information.\n", prog_name);
        /* deallocate each non-null entry in argtable[] */
        arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
        return 1;
    }

    if (version->count > 0) {
        printf("UDPForwarder version: %s \n", UDP_FORWARDER_VERSION);
        /* deallocate each non-null entry in argtable[] */
        arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
        return 0;
    }

    riosockets_initialize();

    int exit_code;
    if(output_address->count <= 0) {
        printf("No output address passed see --help");
        exit_code = -1;
    } else if(sender->count > 0) {
        exit_code = sender_mode();
    } else {
        exit_code = multicast_mode();
    }

    riosockets_deinitialize();

    /* deallocate each non-null entry in argtable[] */
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));

    return exit_code;
}

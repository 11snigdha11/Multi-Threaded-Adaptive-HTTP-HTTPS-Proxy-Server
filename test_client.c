// test_client.c
// A more capable test client that can handle both HTTP GET and HTTPS CONNECT requests.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>

#define BUFFER_SIZE 8192

// A simple helper function to parse a URL into its components
void parse_url(const char *url, char *host, int host_len, int *port, char *path, int path_len) {
    char temp_url[1024];
    strncpy(temp_url, url, sizeof(temp_url) - 1);
    temp_url[sizeof(temp_url) - 1] = '\0';

    char *scheme_end = strstr(temp_url, "://");
    char *host_start;

    if (scheme_end) {
        host_start = scheme_end + 3;
    } else {
        host_start = temp_url;
    }

    char *path_start = strchr(host_start, '/');
    if (path_start) {
        strncpy(path, path_start, path_len - 1);
        path[path_len - 1] = '\0';
        *path_start = '\0'; // Terminate the host part
    } else {
        strncpy(path, "/", path_len - 1);
        path[path_len - 1] = '\0';
    }
    
    char* port_start = strrchr(host_start, ':');
    if (port_start) {
        *port_start = '\0'; // Terminate the host part
        *port = atoi(port_start + 1);
    }

    strncpy(host, host_start, host_len - 1);
    host[host_len - 1] = '\0';
}


int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <proxy_host> <proxy_port> <URL_to_fetch>\n", argv[0]);
        fprintf(stderr, "Example (HTTP): %s localhost 8080 http://example.com/index.html\n", argv[0]);
        fprintf(stderr, "Example (HTTPS): %s localhost 8080 https://www.google.com\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *proxy_host = argv[1];
    int proxy_port = atoi(argv[2]);
    char *url = argv[3];

    // --- Connect to the proxy server ---
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct hostent *server = gethostbyname(proxy_host);
    if (server == NULL) {
        fprintf(stderr, "ERROR, no such host: %s\n", proxy_host);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(proxy_port);

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        exit(EXIT_FAILURE);
    }
    printf("--- Connected to proxy at %s:%d ---\n", proxy_host, proxy_port);

    // --- Determine request type (HTTP GET or HTTPS CONNECT) ---
    char request[BUFFER_SIZE];
    
    if (strncasecmp(url, "https://", 8) == 0) {
        // HTTPS CONNECT Request
        char host[256];
        int port = 443; // Default HTTPS port
        char path[1]; // Not needed for CONNECT
        
        parse_url(url, host, sizeof(host), &port, path, sizeof(path));
        
        snprintf(request, BUFFER_SIZE, "CONNECT %s:%d HTTP/1.0\r\n\r\n", host, port);
        
        printf("--- Sending CONNECT Request ---\n%s", request);
        
        if (send(sock_fd, request, strlen(request), 0) < 0) {
            perror("send");
            exit(EXIT_FAILURE);
        }

        // Wait for the "200 Connection established" response
        char response_buffer[BUFFER_SIZE];
        ssize_t bytes_received = recv(sock_fd, response_buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received > 0) {
            response_buffer[bytes_received] = '\0';
            printf("--- Received Proxy Response ---\n%s", response_buffer);
            if(strstr(response_buffer, "200") != NULL) {
                printf("--- Tunnel established. In a real client, TLS handshake would start now. ---\n");
            } else {
                printf("--- Proxy failed to establish tunnel. ---\n");
            }
        }

    } else {
        // HTTP GET Request
        char host[256];
        int port = 80;
        char path[1024];

        parse_url(url, host, sizeof(host), &port, path, sizeof(path));

        snprintf(request, BUFFER_SIZE,
                 "GET %s HTTP/1.0\r\n"
                 "Host: %s\r\n"
                 "Connection: close\r\n\r\n",
                 url, host);
        
        printf("--- Sending GET Request ---\n%s", request);

        if (send(sock_fd, request, strlen(request), 0) < 0) {
            perror("send");
            exit(EXIT_FAILURE);
        }

        // --- Receive and print the full response ---
        char response_buffer[BUFFER_SIZE];
        ssize_t bytes_received;

        printf("--- Receiving Response ---\n");
        while ((bytes_received = recv(sock_fd, response_buffer, BUFFER_SIZE - 1, 0)) > 0) {
            response_buffer[bytes_received] = '\0';
            printf("%s", response_buffer);
        }
        if (bytes_received < 0) {
            perror("recv");
        }
    }

    printf("\n--- Connection closed ---\n");
    close(sock_fd);

    return 0;
}
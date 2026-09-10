#include "proxy_parse.h"
#include "cache.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* --- Global Variables --- */
volatile sig_atomic_t server_running = 1;

// --- MONITORING COUNTERS (extern from utils.h) ---
extern long long mon_requests;
extern long long mon_hits;
extern pthread_mutex_t mon_mutex;

/* --- Forward Declarations --- */
void handle_request(int client_socket, char* client_ip);
void handle_http_request(int client_socket, struct ParsedRequest *req, char* client_ip);
void handle_connect_request(int client_socket, struct ParsedRequest *req, char* client_ip);

/* --- Signal Handler --- */
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        server_running = 0;
    }
}

/* --- Main Server Logic --- */
int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    load_configuration("proxy.conf");
    load_blacklist("blacklist.txt");
    init_logging_and_monitoring(); // Combined init
    init_disk_cache();
    init_adaptive_cache(g_real_policy);
    init_task_queue(100);

    // --- Create Worker Threads ---
    pthread_t threads[g_thread_pool_size];
    for (int i = 0; i < g_thread_pool_size; i++) {
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    }
    
    // --- Create and Start the Monitoring Thread ---
    pthread_t monitor_thread_id;
    pthread_create(&monitor_thread_id, NULL, monitoring_thread, NULL);

    int server_fd;
    struct sockaddr_in address;
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(g_port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        log_message("FATAL", "bind failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 100) < 0) {
        log_message("FATAL", "listen failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    printf("Proxy server listening on port %d...\n", g_port);
    log_message("INFO", "Server starting: Port=%d, Threads=%d, CacheSize=%zuMB", g_port, g_thread_pool_size, g_max_cache_size / (1024*1024));

    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (errno == EINTR && !server_running) break;
            log_message("ERROR", "accept failed: %s", strerror(errno));
            continue;
        }
        Task task = { .type = CLIENT_REQUEST };
        task.data.client.socket = client_socket;
        inet_ntop(AF_INET, &client_addr.sin_addr, task.data.client.ip, INET_ADDRSTRLEN);
        enqueue_task(task);
    }

    log_message("INFO", "Shutting down...");
    broadcast_for_shutdown();
    for (int i = 0; i < g_thread_pool_size; i++) {
        pthread_join(threads[i], NULL);
    }
    pthread_join(monitor_thread_id, NULL); // Wait for monitor thread to finish
    
    close(server_fd);
    log_message("INFO", "Server shut down cleanly.");
    
    close_logging();
    free_blacklist();
    // Add cache destruction if necessary
    return 0;
}

/* --- Worker & Request Handlers --- */
void* worker_thread(void *arg) {
    while (1) {
        Task task = dequeue_task();
        if (task.type == -1) break;

        if (task.type == CLIENT_REQUEST) {
            handle_request(task.data.client.socket, task.data.client.ip);
            close(task.data.client.socket);
        } else if (task.type == PREFETCH_REQUEST) {
            handle_prefetch_request(task.data.url);
        }
    }
    return NULL;
}

void handle_request(int client_socket, char* client_ip) {
    char *buffer = (char*)malloc(MAX_REQUEST_LEN);
    if (!buffer) return;
    bzero(buffer, MAX_REQUEST_LEN);

    ssize_t bytes_read = recv(client_socket, buffer, MAX_REQUEST_LEN - 1, 0);
    if (bytes_read <= 0) {
        free(buffer);
        return;
    }
    
    struct ParsedRequest *req = ParsedRequest_create();
    if (ParsedRequest_parse(req, buffer, bytes_read) < 0) {
        log_message("ERROR", "Failed to parse request from %s.", client_ip);
    } else {
        log_message("INFO", "Request from %s: %s %s%s", client_ip, req->method, req->host, req->path);
        if (is_blacklisted(req->host)) {
            log_message("WARN", "Blocked blacklisted host: %s", req->host);
            const char *res = "HTTP/1.1 403 Forbidden\r\n\r\n";
            send(client_socket, res, strlen(res), 0);
        } else if (req->method && strcmp(req->method, "CONNECT") == 0) {
            handle_connect_request(client_socket, req, client_ip);
        } else {
            handle_http_request(client_socket, req, client_ip);
        }
    }
    ParsedRequest_destroy(req);
    free(buffer);
}

void handle_http_request(int client_socket, struct ParsedRequest *req, char* client_ip) {
    if (!req->host || !req->path) {
        log_message("ERROR", "Incomplete request from %s.", client_ip);
        return;
    }
    char cache_key[2048];
    snprintf(cache_key, sizeof(cache_key), "%s%s", req->host, req->path);
    
    // --- UPDATE MONITORING & ADAPTIVE COUNTERS ---
    pthread_mutex_lock(&mon_mutex);
    mon_requests++;
    pthread_mutex_unlock(&mon_mutex);
    
    pthread_mutex_lock(&adaptive_mutex);
    g_request_count++;
    ghost_cache_access(cache_key);
    size_t data_size;
    void* data = get_from_cache(cache_key, &data_size);
    pthread_mutex_unlock(&adaptive_mutex);

    if (data) {
        // --- CACHE HIT ---
        pthread_mutex_lock(&mon_mutex);
        mon_hits++;
        pthread_mutex_unlock(&mon_mutex);

        send(client_socket, data, data_size, 0);
        free(data);
        return;
    }
    
    log_message("INFO", "In-Memory MISS for: %s", cache_key);
    
    char* filepath = key_to_filepath(cache_key);
    if (filepath) {
        FILE* file = fopen(filepath, "rb");
        if (file) {
            log_message("INFO", "Disk HIT for: %s", cache_key);
            fseek(file, 0, SEEK_END);
            long size = ftell(file);
            fseek(file, 0, SEEK_SET);
            char* file_buf = malloc(size);
            if(file_buf) {
                fread(file_buf, 1, size, file);
                send(client_socket, file_buf, size, 0);
                put_in_cache(cache_key, file_buf, size);
                free(file_buf);
            }
            fclose(file);
            free(filepath);
            return;
        }
        free(filepath);
    }
    log_message("INFO", "Disk MISS for: %s", cache_key);
    
    // Fetch from origin server
    int remote_port = req->port ? atoi(req->port) : 80;
    struct hostent *host = gethostbyname(req->host);
    if (!host) {
        log_message("ERROR", "Cannot resolve hostname: %s", req->host);
        return;
    }

    int remote_socket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in remote_addr;
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(remote_port);
    bcopy((char*)host->h_addr, (char*)&remote_addr.sin_addr.s_addr, host->h_length);

    if (connect(remote_socket, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) == 0) {
        char new_req[MAX_REQUEST_LEN];
        snprintf(new_req, sizeof(new_req), "GET %s %s\r\nHost: %s\r\nConnection: close\r\n\r\n", req->path, req->version, req->host);
        send(remote_socket, new_req, strlen(new_req), 0);

        char *res_buf = (char*)malloc(g_max_element_size);
        if (res_buf) {
            ssize_t total_size = 0, bytes;
            while (total_size < g_max_element_size && (bytes = recv(remote_socket, res_buf + total_size, g_max_element_size - total_size, 0)) > 0) {
                send(client_socket, res_buf + total_size, bytes, 0);
                total_size += bytes;
            }
            if (total_size > 0) {
                put_in_cache(cache_key, res_buf, total_size);
                ghost_cache_put(cache_key);
                save_to_disk_cache(cache_key, res_buf, total_size);

                char* content_type = find_header(res_buf, total_size, "Content-Type");
                if (content_type && strcasestr(content_type, "text/html")) {
                    char* body = get_body(res_buf, total_size);
                    if (body) start_prefetching(body, req->host, req->path);
                }
                free(content_type);
            }
            free(res_buf);
        }
    } else {
        log_message("ERROR", "Failed to connect to: %s", req->host);
    }
    close(remote_socket);

    pthread_mutex_lock(&adaptive_mutex);
    if (g_request_count >= ADAPTIVE_WINDOW) {
        run_adaptation_check();
    }
    pthread_mutex_unlock(&adaptive_mutex);
}

void handle_connect_request(int client_socket, struct ParsedRequest *req, char* client_ip) {
    int remote_port = req->port ? atoi(req->port) : 443;
    struct hostent *host = gethostbyname(req->host);
    if (!host) {
        log_message("ERROR", "CONNECT: Cannot resolve %s", req->host);
        return;
    }
    int remote_socket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in remote_addr;
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(remote_port);
    bcopy((char*)host->h_addr, (char*)&remote_addr.sin_addr.s_addr, host->h_length);
    if (connect(remote_socket, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) < 0) {
        log_message("ERROR", "CONNECT: Failed to connect to %s", req->host);
        close(remote_socket);
        return;
    }
    const char *ok_res = "HTTP/1.1 200 Connection established\r\n\r\n";
    if (send(client_socket, ok_res, strlen(ok_res), 0) < 0) {
        close(remote_socket);
        return;
    }
    log_message("INFO", "Tunnel established for %s:%d", req->host, remote_port);
    fd_set read_fds;
    int max_fd = (client_socket > remote_socket) ? client_socket : remote_socket;
    while (server_running) {
        FD_ZERO(&read_fds);
        FD_SET(client_socket, &read_fds);
        FD_SET(remote_socket, &read_fds);
        struct timeval tv = { .tv_sec = 60 };
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (activity <= 0) break;
        char buffer[MAX_REQUEST_LEN];
        ssize_t bytes;
        if (FD_ISSET(client_socket, &read_fds)) {
            if ((bytes = recv(client_socket, buffer, sizeof(buffer), 0)) <= 0) break;
            if (send(remote_socket, buffer, bytes, 0) <= 0) break;
        }
        if (FD_ISSET(remote_socket, &read_fds)) {
            if ((bytes = recv(remote_socket, buffer, sizeof(buffer), 0)) <= 0) break;
            if (send(client_socket, buffer, bytes, 0) <= 0) break;
        }
    }
    log_message("INFO", "Tunnel closed for %s:%d", req->host, remote_port);
    close(remote_socket);
}
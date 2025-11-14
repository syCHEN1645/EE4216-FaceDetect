#include "lwip/sockets.h"
#include "esp_log.h"
#include <string>

static const char *TAG = "TCP_CLIENT";
static const int ip_protocol = 0;  
static int sock = -1; 
static int max_retry = 5; 
static int curr_try = 0; 

bool tcp_connect(const char *server_ip, uint16_t port)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(server_ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);

    // create an IPv4, TCP socket file descriptor 
    sock = socket(AF_INET, SOCK_STREAM, ip_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return false;
    }

    int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
        while (curr_try < max_retry && err != 0) { 
            err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            curr_try ++; 
        }
        close(sock);
        sock = -1; 
        return false;
    }
    ESP_LOGI(TAG, "Successfully connected to server");
    return true;
}

bool tcp_send(const std::string &message)
{
    if (sock < 0){ 
        ESP_LOGE(TAG, "UNABLE TO SEND INFO");
        return false;
    } 
    send(sock, message.c_str(), message.length(), 0);
    ESP_LOGI(TAG, "Successfully sent to server");
    return true; 
}

void tcp_recv(void *pvParameters){ 
    while (true){ 
        if (sock < 0){ 
            ESP_LOGE(TAG, "UNABLE TO RECV INFO");
            return; 
        } 
        char buffer[1024]; 
        int bytes_recv = recv(sock, buffer, 1024-1, 0); 
        if (bytes_recv > 0){ 
            buffer[bytes_recv] = '\0'; 
            ESP_LOGI(TAG, "RECEIVED RESPONSE: %s", buffer);
            tcp_send("HMM TEST\r"); 
        } else { 
            ESP_LOGE(TAG, "RECV FAILED"); 
        }
    } 
}

void tcp_close()
{
    if (sock >= 0) {
        close(sock);
        sock = -1; 
    }
}



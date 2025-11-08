#include "lwip/sockets.h"
#include "esp_log.h"
#include <string>

static const char *TAG = "TCP_CLIENT";
static const int ip_protocol = 0;  
static int sock = -1; 

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
        close(sock);
        sock = -1; 
        return false;
    }
    ESP_LOGI(TAG, "Successfully connected to server");
    return true;
}

void tcp_send(const std::string &message)
{
    if (sock < 0) return;
    send(sock, message.c_str(), message.length(), 0);
}

void tcp_close()
{
    if (sock >= 0) {
        close(sock);
        sock = -1; 
    }
}



/*******************************************************************************
 * TCP Client for ESP32-S3-EYE
 * 
 * PURPOSE:
 * - Connect to Arduino gateway via TCP
 * - Send face recognition results to gateway
 * - Receive PIR motion trigger commands from gateway
 * 
 * PROTOCOL:
 * - Send: JSON format with detection results
 * - Receive: "1\r" = PIR motion detected, trigger recognition
 ******************************************************************************/

#include "lwip/sockets.h"  // Sockets Library 
#include "esp_log.h"       // Library Library 
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string>        

#include "shared_mem.hpp"

static const char *TAG = "TCP_CLIENT";  // For Logging Library Use 
static const int ip_protocol = 0;  
static int sock = -1;     // if unable to connect, sock will take on -1 value 
static const int max_retry = 5;
static bool connection_active = false;

// const char* server_ip = "172.20.10.14";   // Arduino gateway IP
// const int port = 5500;
// #define GATEWAY_IP   "192.168.68.103"    // MUST match the gateway printout
// #define GATEWAY_PORT 5500

bool tcp_connect(const char *server_ip, uint16_t port)
{
    // const char *server_ip : server_ip is a pointer to a char
    //     --> points to beginning of ipv4 string
    //     --> const means read-only; cannot be changed
    // uint16_t : unsigned integer type that uses 16 bits of memory

    // check if any existing connections. if yes, close them. 
    if (sock >= 0) {
        ESP_LOGW(TAG, "Already connected, closing old connection");
        close(sock);
        sock = -1;
    }

    // define structure dest_addr
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(server_ip); // server (other esp32)'s IP 
    dest_addr.sin_family = AF_INET;   // ipv4 
    dest_addr.sin_port = htons(port); // server port, big endian 

    ESP_LOGI(TAG, "╔════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   TCP CLIENT INITIALIZATION            ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════╝");
    ESP_LOGI(TAG, "Target: %s:%d", server_ip, port);

    // create an IPv4, TCP socket file descriptor 
    sock = socket(AF_INET, SOCK_STREAM, ip_protocol);
        // AF_INET : Specifies socket will use IPv4 Address Family
        // SOCK_STREAM : Stream Socket with TCP Protocol  
        // ip_protocol : 0 (unspecified) selects TCP Protocol as default
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return false;
    }
    ESP_LOGI(TAG, "Socket created");

    // Set socket timeouts
    struct timeval timeout;
    timeout.tv_sec = 10;   // 10 second timeout for send/receive
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Keep-alive settings (helps maintain connection)
    int keepalive = 1;
    int keepidle = 5;      // Start probes after 5 seconds
    int keepinterval = 5;  // Probe every 5 seconds
    int keepcount = 3;     // Drop after 3 failed probes
    
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepinterval, sizeof(keepinterval));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcount, sizeof(keepcount));

    // Client (esp-eye) initiates connection to server (other esp32) device
    ESP_LOGI(TAG, "Attempting to connect...");
    int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    
    int retry_count = 0;
    // if unable to connect, retry up to 5 times
    while (err != 0 && retry_count < max_retry) {
        retry_count++;
        ESP_LOGW(TAG, "Connection attempt %d/%d failed (errno %d)", 
                 retry_count, max_retry, errno);
        ESP_LOGI(TAG, "Retrying in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Close and Recreate socket for retry
        close(sock);
        sock = socket(AF_INET, SOCK_STREAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Failed to recreate socket");
            return false;
        }
        // attempt to connect with newly recreated socket 
        err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    }
    
    // tcp connection failed 
    if (err != 0) {
        ESP_LOGE(TAG, "╔════════════════════════════════════════╗");
        ESP_LOGE(TAG, "║   CONNECTION FAILED                    ║");
        ESP_LOGE(TAG, "╚════════════════════════════════════════╝");
        ESP_LOGE(TAG, "Failed after %d attempts", max_retry);
        ESP_LOGE(TAG, "Please check:");
        ESP_LOGE(TAG, "  1. Gateway is powered on");
        ESP_LOGE(TAG, "  2. Gateway IP is correct: %s", server_ip);
        ESP_LOGE(TAG, "  3. Both devices on same WiFi network");
        ESP_LOGE(TAG, "  4. Port %d is not blocked", port);
        
        close(sock);
        sock = -1;
        connection_active = false;
        return false;
    }
    
    // tcp connection successful 
    connection_active = true;
    ESP_LOGI(TAG, "╔════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║     CONNECTED TO GATEWAY               ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════╝");
    ESP_LOGI(TAG, "Remote: %s:%d", server_ip, port);
    
    return true;
}

/**
 * @brief Send data to gateway
 * @param message String to send (should be JSON format)
 * @return true if sent successfully, false otherwise
 */
bool tcp_send(const std::string &message)
{
    // const std::string &message
    //   --> const means message is read-only
    //   &message is a reference to message, this prevents code from making a copy
    //   of the message every time the code is executed
    //   doing so improves performance as there is less overhead of copying large strings  

    if (sock < 0 || !connection_active) {
        ESP_LOGE(TAG, "✗ Cannot send: not connected to gateway");
        return false;
    }
    
    ESP_LOGD(TAG, "Sending %d bytes...", message.length());
    
    int total_sent = 0;
    int len = message.length();
    const char* data = message.c_str();   // c_str() returns a pointer to message

    
    // check that all data has been sent 
    while (total_sent < len) {
        // send message using tcp connection previously established if sock file descriptor exists
        int sent = send(sock, data + total_sent, len - total_sent, 0);
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGW(TAG, "Send would block, retrying...");
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            ESP_LOGE(TAG, "✗ Send failed: errno %d", errno);
            connection_active = false;
            return false;
        }
        
        total_sent += sent;
    }
    
    ESP_LOGI(TAG, "Sent %d bytes to gateway", total_sent);
    ESP_LOGD(TAG, "Data: %s", message.c_str());
    
    return true;
}

/**
 * @brief Task to receive data from gateway (runs continuously)
 * Listens for PIR trigger commands from gateway
 */
void tcp_recv(void *pvParameters)
{
    ESP_LOGI(TAG, "╔════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   TCP RECEIVE TASK STARTED             ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════╝");
    ESP_LOGI(TAG, "Listening for PIR trigger commands...");
    
    char buffer[1024];
    
    while (true) {

        // connection non-existent as socket file descriptor invalid 
        if (sock < 0 || !connection_active) {
            ESP_LOGE(TAG, "Connection lost, stopping receive task");
            break;
        }
        
        // Blocking receive command 
        // code will only continue after recv if: 
        //    (1) data is received 
        //    (2) connection is closed 
        //    (3) error occurs 
        //    (4) timeout - 10 secs 
        int bytes_recv = recv(sock, buffer, sizeof(buffer) - 1, 0);

        if (bytes_recv > 0) {   // if data is received 
            buffer[bytes_recv] = '\0';   // add null terminator 
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔════════════════════════════════════════╗");
            ESP_LOGI(TAG, "║   COMMAND RECEIVED FROM GATEWAY        ║");
            ESP_LOGI(TAG, "╚════════════════════════════════════════╝");
            ESP_LOGI(TAG, "Received: %s (%d bytes)", buffer, bytes_recv);
            
            // Check for PIR motion trigger
            if (buffer[0] == '1') {
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "╔════════════════════════════════════════╗");
                ESP_LOGI(TAG, "║     PIR MOTION TRIGGER DETECTED        ║");
                ESP_LOGI(TAG, "╚════════════════════════════════════════╝");
                ESP_LOGI(TAG, "Motion detected by gateway PIR sensor");
                ESP_LOGI(TAG, "Face recognition will be triggered...");
                ESP_LOGI(TAG, "");

                // set flag to 3 to start streaming 
                set_flag(&shared_mem.stream_flag, 3);
                
            } else {
                ESP_LOGI(TAG, "Unknown command: %s", buffer);
            }
        } else {
            // Error or timeout
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout - this is normal, just continue
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }
    }
}

/**
 * @brief Check if connection is active
 * @return true if connected, false otherwise
 */
bool tcp_is_connected()
{
    return (sock >= 0 && connection_active);
}

/**
 * @brief Close TCP connection
 */
void tcp_close()
{
    // if sock file descriptor is valid, close the TCP connection
    if (sock >= 0) {
        ESP_LOGI(TAG, "Closing TCP connection...");
        shutdown(sock, SHUT_RDWR);
        close(sock);
        sock = -1;
        connection_active = false;
        ESP_LOGI(TAG, "Connection closed");
    }
}


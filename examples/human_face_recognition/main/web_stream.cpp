#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"

// Macro headers to tell the browser what protocol to expect
/*
--frame
Content-Type: image/jpeg
Content-Length: xxxx

<raw jpeg bytes>
*/
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
// Separate payload and image
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PAYLOAD = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
const char* TAG = "Stream";

static esp_err_t stream_handler(httpd_req_t *req) {
    size_t jpg_buf_len = 0;
    uint8_t *jpg_buf = NULL;
    // Check if  http response is ok
    auto res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");      
    httpd_resp_set_hdr(req, "X-Framerate", "60");

    while (true) {
        camera_fb_t *cam_fb = esp_camera_fb_get();
        // Handle when no frame captured
        if (!cam_fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "FRAME INFO: len=%u format=%d", (unsigned)cam_fb->len, cam_fb->format);

        // Buffer for headers (not frames)
        char buffer[64];
        // snprintf: put formatted string into buffer
        // params: ptr to buffer, size of buffer, formatted string
        int l = snprintf(buffer, sizeof(buffer), STREAM_PAYLOAD, cam_fb->len);

        // Send a boundary string
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        
        if (res != ESP_OK) {
            break;
        }
        ESP_LOGI(TAG, "Boundary sent, res=%d", res);
        
        // Send payboad string
        ESP_LOGI(TAG, "Payload sent, res=%d", res);
        res = httpd_resp_send_chunk(req, buffer, l);
        if (res != ESP_OK) {
            break;
        }

        // Send actual frame
        // Make sure frame is of JPG before sending
        if (!frame2jpg(cam_fb, 80, &jpg_buf, &jpg_buf_len)) {
            break;
        }

        // Return frame buffer
        esp_camera_fb_return(cam_fb);
        cam_fb = NULL;

        res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_buf_len);
        if (res != ESP_OK) {
            break;
        }
        ESP_LOGI(TAG, "JPEG frame size=%u bytes", jpg_buf_len);

        // Signal end of this frame
        res = httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGI(TAG, "FRAME END (NULL) -> res=%d (%s)", res, esp_err_to_name(res));
        
        if (res != ESP_OK) {
            break;
        }
        ESP_LOGI(TAG, "A frame was just sent to server");
        
        //vTaskDelay(pdMS_TO_TICKS(100));
    }
    // Program comes here only if errors happen
    ESP_LOGE(TAG, "An error occured when streaming video");
    return res;
}


httpd_handle_t init_http(httpd_handle_t server) {
    // Set http config
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    // Increase stack size to prevent overflow
    http_config.stack_size = 24 * 1024;

    // Set handler
    if (httpd_start(&server, &http_config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting http server");
        return NULL;
    }
    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &stream_uri);
    ESP_LOGI(TAG, "Http server started successfully");
    return server;
}
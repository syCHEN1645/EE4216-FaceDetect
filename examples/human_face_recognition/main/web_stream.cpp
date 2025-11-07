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

typedef struct {
    httpd_req_t *req;
    size_t len;
} jpg_chunking_t;

static size_t encode_to_jpg(void *arg, size_t index, const void *frame, size_t len) {
    // A standard workflow to encode
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    // Index: frame is split into small chunks, index of chunk, not frame
    if (index != 0) {
        j->len = 0;
    }
    // Send encoded frame
    if (httpd_resp_send_chunk(j->req, (const char *)frame, len) != ESP_OK) {
        return 0;
    }
    // Track total size of data sent
    j->len += len;
    return len;
}

static esp_err_t stream_handler(httpd_req_t *req) {
    // Check if  http response is ok
    auto res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }

    // Set these to stream video instead of pictures
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");  
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");      
    httpd_resp_set_hdr(req, "X-Framerate", "60");

    while (true) {
        camera_fb_t *cam_fb = esp_camera_fb_get();
        // Handle when no frame captured
        if (!cam_fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            return ESP_FAIL;
        }

        // Buffer for headers (not frames)
        char buffer[64];
        // snprintf: put formatted string into buffer
        // params: ptr to buffer, size of buffer, formatted string
        int l = snprintf(buffer, sizeof(buffer), STREAM_PAYLOAD, cam_fb->len);

        // Send a boundary string
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        ESP_LOGI(TAG, "Boundary sent, res=%d", res);
        if (res != ESP_OK) {
            break;
        }
        // Send payboad string
        ESP_LOGI(TAG, "Payload sent, res=%d", res);
        res = httpd_resp_send_chunk(req, buffer, l);
        if (res != ESP_OK) {
            break;
        }
        // Send actual frame
        // Make sure frame is of JPG before sending
        if (cam_fb->format != PIXFORMAT_JPEG) {
            jpg_chunking_t j = {req, 0};
            // Send encoded frame in-flight
            res = frame2jpg_cb(cam_fb, 60, encode_to_jpg, &j);
            if (res != ESP_OK) {
                break;
            }
            // Signal end of this frame
            res = httpd_resp_send_chunk(req, NULL, 0);
        } else {
            res = httpd_resp_send_chunk(req, (char*)cam_fb->buf, cam_fb->len);
        }
        ESP_LOGI(TAG, "A frame was just sent to server");
        if (res != ESP_OK) {
            break;
        }
        
        // Important to return the frame back to camera because its number is limited
        esp_camera_fb_return(cam_fb);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Program comes here only if errors happen
    ESP_LOGE(TAG, "An error occured when streaming video");
    return res;
}


httpd_handle_t init_http(httpd_handle_t server) {
    // Set http config
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();

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
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "shared_mem.hpp"

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

// http client id (of the webpage)
static int client_fd = -1;

// webpage code:
const char index_html[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>ESP Stream + Alerts</title>
  <style>
    body { font-family: sans-serif; text-align: center; background: #f5f5f5; }
    img { width: 480px; border: 3px solid #333; border-radius: 10px; }
    #alert { color: red; font-size: 20px; margin-top: 10px; font-weight: bold; }
  </style>
</head>
<body>
  <h1>ESP32 Camera Stream</h1>
  <img id="video" src="/stream" />
  <div id="alert"></div>
  <script>
    const alertBox = document.getElementById("alert");

    // Listen for server-sent events from /events
    const evtSource = new EventSource("/events");
    evtSource.onmessage = function(event) {
      alertBox.textContent = event.data;
      setTimeout(() => alertBox.textContent = "", 4000); // clear after 4s
    };
  </script>
</body>
</html>
)rawliteral";

static esp_err_t info_handler(httpd_req_t *req) {
    // push info onto webpage at fixed intervals
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true) {
        int flag = get_flag(&shared_mem.stream_flag);

        if (flag == 2) {
            const char* msg = "Known visitor detected!";
            char buffer[128];
            int len = snprintf(buffer, sizeof(buffer), "info: %s\n\n", msg);
            esp_err_t res = httpd_resp_send_chunk(req, buffer, len);
            if (res != ESP_OK) {
                ESP_LOGW(TAG, "Sending text info failed.");
                break;
            }
        } else if (flag == 0) {
            // 
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    return ESP_OK;
}

static esp_err_t stream_handler(httpd_req_t *req) {
    size_t jpg_buf_len = 0;
    uint8_t *jpg_buf = NULL;
    camera_fb_t *cam_fb = NULL;
    // Buffer for headers (not frames)
    char buffer[128];

    // Check if  http response is ok
    auto res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");      
    httpd_resp_set_hdr(req, "X-Framerate", "60");

    while (true) {
        int flag = get_flag(&shared_mem.stream_flag);

        if (flag == 1) {
            cam_fb = esp_camera_fb_get();
            // Handle when no frame captured
            if (!cam_fb) {
                ESP_LOGE(TAG, "Camera capture failed");
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "FRAME INFO: len=%u format=%d", (unsigned)cam_fb->len, cam_fb->format);
            
            // Make sure frame is of JPG before sending
            bool converted = frame2jpg(cam_fb, 80, &jpg_buf, &jpg_buf_len);
            // Return frame buffer
            esp_camera_fb_return(cam_fb);
            if (!converted) {
                ESP_LOGI(TAG, "JPEG conversion failed");
                break;
            }

            // Send a boundary string
            res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));        
            if (res != ESP_OK) {
                break;
            }
            ESP_LOGI(TAG, "Boundary sent, res=%d", res);
            
            // Send payboad string
            // snprintf: put formatted string into buffer
            // params: ptr to buffer, size of buffer, formatted string
            int l = snprintf(buffer, sizeof(buffer), STREAM_PAYLOAD, jpg_buf_len);
            res = httpd_resp_send_chunk(req, buffer, l);
            if (res != ESP_OK) {
                break;
            }
            ESP_LOGI(TAG, "Payload sent, res=%d, jpg_len=%d", res, jpg_buf_len);

            // Send actual frame
            res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_buf_len);
            free(jpg_buf);
            jpg_buf = NULL;
            if (res != ESP_OK) {
                break;
            }
            ESP_LOGI(TAG, "JPEG frame size=%u bytes", jpg_buf_len);

            // Signal end of this frame
            // res = httpd_resp_send_chunk(req, NULL, 0);
            // ESP_LOGI(TAG, "FRAME END (NULL) -> res=%d (%s)", res, esp_err_to_name(res));
            // if (res != ESP_OK) {
            //     break;
            // }
            ESP_LOGI(TAG, "A frame was just sent to server");
            
            vTaskDelay(pdMS_TO_TICKS(120));
        } else if (flag == 2) {
            // send a picture and standby
            // before coming to this step, the stream was already on
            // hence no need to send a separate picture, just keep showing the last frame
            vTaskDelay(pdMS_TO_TICKS(200));
            set_flag(&shared_mem.stream_flag, 0);
        } else if (flag == 0) {
            // do nothing
            ESP_LOGI(TAG, "Standby...");
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            //
        }
    }

    // if loop is broken, so error has occured
    if (jpg_buf) {
        free(jpg_buf);
    }
    cam_fb = NULL;
    jpg_buf = NULL;
    // Program comes here only if errors happen
    ESP_LOGE(TAG, "An error occured when streaming video");
    return res;        
}


httpd_handle_t init_http() {
    httpd_handle_t server;
    // Set http config
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    // Increase stack size to prevent overflow
    http_config.stack_size = 24 * 1024;
    http_config.recv_wait_timeout = 5;
    http_config.send_wait_timeout = 5;

    // Set handler
    if (httpd_start(&server, &http_config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting http server");
        return NULL;
    }

    httpd_uri_t info_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = info_handler,
    .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &info_uri);

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    #ifdef CONFIG_HTTPD_WS_SUPPORT
        ,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    #endif
    };
    httpd_register_uri_handler(server, &stream_uri);
    ESP_LOGI(TAG, "Http server started successfully");
    return server;
}
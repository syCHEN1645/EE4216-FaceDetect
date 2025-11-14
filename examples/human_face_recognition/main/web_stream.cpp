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
    body { font-family: sans-serif; text-align: center; background: #8ff4a3; }
    img { border: 3px solid #333; border-radius: 10px; margin-bottom: 10px; }
    #video { width: 480px; }
    #snapshot { width: 480px; }
    #log {
      width: 480px; height: 200px; margin: 15px auto;
      background: #d1df84; border: 2px solid #333;
      border-radius: 10px; overflow-y: scroll;
      text-align: left; padding: 10px; font-size: 16px;
    }
  </style>
</head>
<body>
  <h1>ESP32 Camera Stream</h1>
  <img id="video" src="/stream" />
  
  <h2>Latest Captured Frame</h2>
  <img id="snapshot" src="/capture" alt="No snapshot yet" />

  <h2>Event Log</h2>
  <div id="log"></div>

  <script>
  const logBox = document.getElementById("log");
  const snapshot = document.getElementById("snapshot");

  function fetchInfo() {
    fetch("/info")
      .then(response => response.json())
      .then(data => {
        const now = new Date().toLocaleTimeString();
        const line = document.createElement("div");
        if (data.msg) {
            line.textContent = `[${now}] ${data.msg}`;
            logBox.appendChild(line);
            logBox.scrollTop = logBox.scrollHeight;
            // refresh snapshot
            if (data.flag === 2 || data.flag === 1) {
                snapshot.src = `/capture?nocache=${Date.now()}`;
            }
        }
      })
      .catch(err => {
        const line = document.createElement("div");
        line.style.color = "red";
        line.textContent = `[Error] ${err}`;
        logBox.appendChild(line);
      });
    }

    // Poll every 1 second
    setInterval(fetchInfo, 1000);
    </script>

</body>
</html>
)rawliteral";


static esp_err_t info_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    static int old_flag = -1;
    int flag = get_flag(&shared_mem.stream_flag);
    const char *msg = "";

    if (old_flag != flag) {
        if (flag == 2) { 
            msg = "Known visitor detected!";
        } else if (flag == 1) {
            msg = "Unknown visitor detected!";
        } else if (flag == 3) {
            msg = "Motion detected!";
        }
    }
    old_flag = flag;
    char buffer[64];
    int len = snprintf(buffer, sizeof(buffer), "{\"flag\": %d, \"msg\": \"%s\"}", flag, msg);
    httpd_resp_send(req, buffer, len);

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

        if (flag == 1 || flag == 3) {
            cam_fb = esp_camera_fb_get();
            // Handle when no frame captured
            if (!cam_fb) {
                ESP_LOGE(TAG, "Camera capture failed");
                return ESP_FAIL;
            }
            // ESP_LOGI(TAG, "FRAME INFO: len=%u format=%d", (unsigned)cam_fb->len, cam_fb->format);
            
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
            // ESP_LOGI(TAG, "Boundary sent, res=%d", res);
            
            // Send payboad string
            // snprintf: put formatted string into buffer
            // params: ptr to buffer, size of buffer, formatted string
            int l = snprintf(buffer, sizeof(buffer), STREAM_PAYLOAD, jpg_buf_len);
            res = httpd_resp_send_chunk(req, buffer, l);
            if (res != ESP_OK) {
                break;
            }
            // ESP_LOGI(TAG, "Payload sent, res=%d, jpg_len=%d", res, jpg_buf_len);

            // Send actual frame
            res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_buf_len);
            free(jpg_buf);
            jpg_buf = NULL;
            if (res != ESP_OK) {
                break;
            }
            // ESP_LOGI(TAG, "JPEG frame size=%u bytes", jpg_buf_len);
            vTaskDelay(pdMS_TO_TICKS(120));
        } else if (flag == 2) {
            // another handler (capture handler) will send a frame
            // pause streaming until a motion triggers
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else if (flag == 0) {
            // do nothing
            // pause a second and start streaming
            vTaskDelay(pdMS_TO_TICKS(1000));
            set_flag(&shared_mem.stream_flag, 1);
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

static esp_err_t capture_handler(httpd_req_t *req)
{
    esp_err_t res;
    size_t jpg_buf_len = 0;
    uint8_t *jpg_buf = NULL;
    camera_fb_t *cam_fb = esp_camera_fb_get();
    if (!cam_fb) {
        ESP_LOGE("Frame", "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Make sure frame is of JPG before sending
    bool converted = frame2jpg(cam_fb, 80, &jpg_buf, &jpg_buf_len);
    // return frame buffer
    esp_camera_fb_return(cam_fb);
    if (!converted) {
        ESP_LOGI(TAG, "JPEG conversion failed");
        res = ESP_FAIL;
        return res;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    res = httpd_resp_send(req, (const char *)jpg_buf, jpg_buf_len);
    free(jpg_buf);
    jpg_buf = NULL;
    if (res != ESP_OK) {
        return res;
    }
    return ESP_OK;
}

static esp_err_t html_code_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, strlen(index_html));
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

    // for set up webpage
    httpd_uri_t html_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = html_code_handler,
    .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &html_uri);    

    // for display text info
    httpd_uri_t info_uri = {
        .uri       = "/info",
        .method    = HTTP_GET,
        .handler   = info_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &info_uri);

    // for capture single frame
    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &capture_uri);

    // for stream video
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
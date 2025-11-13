#include <string>

#include "who_recognition.hpp"
#include "shared_mem.hpp"
#include "tcp_client.cpp"

std::string event; 
std::string status; 
std::string id; 
std::string similarity; 
std::string json_payload; 

namespace who {
namespace recognition {
WhoRecognitionCore::WhoRecognitionCore(const std::string &name, detect::WhoDetect *detect) :
    task::WhoTask(name), m_detect(detect)
{
}

WhoRecognitionCore::~WhoRecognitionCore()
{
    delete m_recognizer;
}

void WhoRecognitionCore::set_recognizer(HumanFaceRecognizer *recognizer)
{
    m_recognizer = recognizer;
}

void WhoRecognitionCore::set_recognition_result_cb(const std::function<void(const std::string &)> &result_cb)
{
    m_recognition_result_cb = result_cb;
}

void WhoRecognitionCore::set_detect_result_cb(const std::function<void(const detect::WhoDetect::result_t &)> &result_cb)
{
    m_detect_result_cb = result_cb;
}

void WhoRecognitionCore::set_cleanup_func(const std::function<void()> &cleanup_func)
{
    m_cleanup = cleanup_func;
}

bool WhoRecognitionCore::run(const configSTACK_DEPTH_TYPE uxStackDepth,
                             UBaseType_t uxPriority,
                             const BaseType_t xCoreID)
{
    if (!m_recognizer) {
        ESP_LOGE("WhoRecognitionCore", "recognizer is nullptr, please call set_recognizer() first.");
        return false;
    }
    return task::WhoTask::run(uxStackDepth, uxPriority, xCoreID);
}

void WhoRecognitionCore::message_handler(int flag)
{
    /*
    0: stop streaming, dont do anything
    1: make a recognition while streaming
    2: send a picture and stop streaming
    3: keep streaming
    */
    switch (flag)
    {
    case 3:
        set_flag(&shared_mem.stream_flag, 3);
        ESP_LOGI("Shared mem", "An unknown face detected, streaming");
        break;
    case 2:
        set_flag(&shared_mem.stream_flag, 2);
        ESP_LOGI("Shared mem", "A known face detected, stop streaming");
        break;
    case 1:
        // motion is detected, do a scan
        xEventGroupSetBits(m_event_group, RECOGNIZE);
        // stream video
        set_flag(&shared_mem.stream_flag, 1);
        ESP_LOGI("Shared mem", "Motion detected, attempt to recognize");
        break;
    case 0:
        set_flag(&shared_mem.stream_flag, 0);
        // stop streaming and standby
        ESP_LOGI("Shared mem", "Streaming stops, standby");
        break;
    default:
        ESP_LOGI("Shared mem", "Unknown flag %d received", flag);
        break;
    }
}

void WhoRecognitionCore::task()
{
    tcp_connect("172.20.10.11", 5500);
    ESP_LOGI("TCP", "TCP connect function ends");
    // handle detection
    /*
    if motion detected: detect face
    if motion disappeared: stop detect and stand by
    if no face is detected: repeat after x seconds
    if a known face detected: send picture and stand by
    if an unknown face detected: send picture and video stream
    */

    while (true) {
        // receive message from motion sensor
        char buf[8];
        int r = recv(sock, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (r > 0) {
            // received a message
            buf[r] = '\0';
            int flag = atoi((char*)buf);
            message_handler(flag);
        } else if (r < 0) {
            ESP_LOGI("WhoRecognitionCore", "TCP reception failed");
        }

        EventBits_t event_bits = xEventGroupWaitBits(
            m_event_group, RECOGNIZE | ENROLL | DELETE | TASK_PAUSE | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
        // event = ""; 
        // status = ""; 
        // id = ""; 
        // similarity = ""; 
        if (event_bits & TASK_STOP) {
            // event += "TASK_STOP";
            break;
        } else if (event_bits & TASK_PAUSE) {
            // event += "TASK_PAUSE"; 
            xEventGroupSetBits(m_event_group, TASK_PAUSED);
            EventBits_t pause_event_bits =
                xEventGroupWaitBits(m_event_group, TASK_RESUME | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
            if (pause_event_bits & TASK_STOP) {
                break;
            } else {
                continue;
            }
        }
        // json_payload = "{"; 
        // json_payload += "\"event\":\"" + event + "\",";
        // json_payload += "\"status\":" + status + ",";
        // json_payload += "\"id\":" + id + ",";
        // json_payload += "\"similarity\":" + similarity;
        // json_payload += "}";
        if (event_bits & RECOGNIZE) {
            event = ""; 
            status = ""; 
            id = ""; 
            similarity = ""; 
            event += "RECOGNIZE"; 
            auto new_detect_result_cb = [this](const detect::WhoDetect::result_t &result) {
                auto ret = m_recognizer->recognize(result.img, result.det_res);
                if (m_detect_result_cb) {
                    m_detect_result_cb(result);
                }
                if (m_recognition_result_cb) {
                    if (ret.empty()) {
                        m_recognition_result_cb("who?");
                        status += "0"; 

                        // tell webpage to keep streaming
                        message_handler(3);
                    } else {
                        m_recognition_result_cb(std::format("id: {}, sim: {:.2f}", ret[0].id, ret[0].similarity));
                        status += "1"; 
                        id += std::to_string(ret[0].id); 
                        similarity += std::to_string(ret[0].similarity); 

                        // tell web page to send a picture and stop streaming
                        message_handler(2);
                    }
                    m_detect->set_detect_result_cb(m_detect_result_cb);
                    json_payload = "{"; 
                    json_payload += "\"event\":\"" + event + "\",";
                    json_payload += "\"status\":" + status + ",";
                    json_payload += "\"id\":" + id + ",";
                    json_payload += "\"similarity\":" + similarity;
                    json_payload += "}\r";
                    tcp_send(json_payload); 
                }    
            };
            m_detect->set_detect_result_cb(new_detect_result_cb);
            continue;
        }
        // event = ""; 
        // status = ""; 
        // id = ""; 
        // similarity = ""; 
        if (event_bits & ENROLL) {
            // event += "ENROLL"; 
            auto new_detect_result_cb = [this](const detect::WhoDetect::result_t &result) {
                esp_err_t ret = m_recognizer->enroll(result.img, result.det_res);
                if (m_detect_result_cb) {
                    m_detect_result_cb(result);
                }
                if (m_recognition_result_cb) {
                    if (ret == ESP_FAIL) {
                        m_recognition_result_cb("Failed to enroll.");
                        // status += "0"; 
                    } else {
                        int num_feats = m_recognizer->get_num_feats(); 
                        m_recognition_result_cb(std::format("id: {} enrolled.", num_feats));
                        // status += "1"; 
                        // id += std::to_string(num_feats); 
                    }
                }
                m_detect->set_detect_result_cb(m_detect_result_cb);
            };
            m_detect->set_detect_result_cb(new_detect_result_cb);
            continue;
        }
        // json_payload = "{"; 
        // json_payload += "\"event\":\"" + event + "\",";
        // json_payload += "\"status\":" + status + ",";
        // json_payload += "\"id\":" + id + ",";
        // json_payload += "\"similarity\":" + similarity;
        // json_payload += "}";
        // event = ""; 
        // status = ""; 
        // id = ""; 
        // similarity = ""; 
        if (event_bits & DELETE) {
            // event += "DELETE"; 
            esp_err_t ret = m_recognizer->delete_last_feat();
            if (m_recognition_result_cb) {
                if (ret == ESP_FAIL) {
                    m_recognition_result_cb("Failed to delete.");
                    // status += "0"; 
                } else {
                    int num_feats = m_recognizer->get_num_feats() + 1; 
                    m_recognition_result_cb(std::format("id: {} deleted.", num_feats));
                    // status += "1"; 
                    // id += std::to_string(num_feats); 
                }
            }
        }
    }
    xEventGroupSetBits(m_event_group, TASK_STOPPED);
    vTaskDelete(NULL);
    // json_payload = "{"; 
    // json_payload += "\"event\":\"" + event + "\",";
    // json_payload += "\"status\":" + status + ",";
    // json_payload += "\"id\":" + id + ",";
    // json_payload += "\"similarity\":" + similarity;
    // json_payload += "}";
    tcp_close(); 
}

void WhoRecognitionCore::cleanup()
{
    if (m_cleanup) {
        m_cleanup();
    }
}

WhoRecognition::WhoRecognition(frame_cap::WhoFrameCapNode *frame_cap_node) :
    m_detect(new detect::WhoDetect("Detect", frame_cap_node)),
    m_recognition(new WhoRecognitionCore("Recognition", m_detect))
{
    WhoTaskGroup::register_task(m_detect);
    WhoTaskGroup::register_task(m_recognition);
}

WhoRecognition::~WhoRecognition()
{
    WhoTaskGroup::destroy();
}

void WhoRecognition::set_detect_model(dl::detect::Detect *model)
{
    m_detect->set_model(model);
}

void WhoRecognition::set_recognizer(HumanFaceRecognizer *recognizer)
{
    m_recognition->set_recognizer(recognizer);
}

detect::WhoDetect *WhoRecognition::get_detect_task()
{
    return m_detect;
}

WhoRecognitionCore *WhoRecognition::get_recognition_task()
{
    return m_recognition;
}
} // namespace recognition
} // namespace who

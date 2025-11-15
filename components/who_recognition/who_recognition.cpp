/*******************************************************************************
 * Face Recognition Core with Gateway Integration
 * 
 * FEATURES:
 * - Connects to Arduino gateway via TCP
 * - Sends recognition results to gateway
 * - Receives PIR motion triggers from gateway
 * - Supports RECOGNIZE, ENROLL, and DELETE operations
 ******************************************************************************/
#include <string>

#include "who_recognition.hpp"
#include "shared_mem.hpp"
#include "tcp_client.cpp"    // include tcp_client library 

// Global variables for building JSON payload
std::string event; 
std::string status; 
std::string id; 
std::string similarity; 
std::string json_payload; 

namespace who {
namespace recognition {

// Initialises a object (recognition core) with a detection module for face recognition
WhoRecognitionCore::WhoRecognitionCore(const std::string &name, detect::WhoDetect *detect) :
    task::WhoTask(name), m_detect(detect)
{
}
// Handles a delete action that frees the memory allocated for m_recognizer. 
WhoRecognitionCore::~WhoRecognitionCore()
{
    delete m_recognizer;
}

// Assigns a recognizer instance (the actual engine that performs face recognition) to core
void WhoRecognitionCore::set_recognizer(HumanFaceRecognizer *recognizer)
{
    m_recognizer = recognizer;
}
// Stores a callback function (triggered when recognition results are available)
void WhoRecognitionCore::set_recognition_result_cb(const std::function<void(const std::string &)> &result_cb)
{
    m_recognition_result_cb = result_cb;
}
// Stores a callnack function for detection results (lets external code react to detection events)
void WhoRecognitionCore::set_detect_result_cb(const std::function<void(const detect::WhoDetect::result_t &)> &result_cb)
{
    m_detect_result_cb = result_cb;
}
// Stores a cleanup function that is called when the recognition core is shut down
void WhoRecognitionCore::set_cleanup_func(const std::function<void()> &cleanup_func)
{
    m_cleanup = cleanup_func;
}

// Entry point for executing WhoRecognitionCore task. 
bool WhoRecognitionCore::run(const configSTACK_DEPTH_TYPE uxStackDepth,
                             UBaseType_t uxPriority,
                             const BaseType_t xCoreID)
{    // Checks if m_recognizer is set 
    if (!m_recognizer) {
        ESP_LOGE("WhoRecognitionCore", "recognizer is nullptr, please call set_recognizer() first.");
        return false;
    } // Delegates execution to base class (WhoTask)
    return task::WhoTask::run(uxStackDepth, uxPriority, xCoreID);
}

void WhoRecognitionCore::task()
{
    // SETUP TCP CONNECTION 
    const char* GATEWAY_IP = "172.20.10.14";  // Arduino Code for ESP32 Uses Static IP Address 
    const uint16_t GATEWAY_PORT = 5500;

    // Print connection display data for readability 
    ESP_LOGI("WhoRecognitionCore", "");
    ESP_LOGI("WhoRecognitionCore", "╔════════════════════════════════════════════╗");
    ESP_LOGI("WhoRecognitionCore", "║   FACE RECOGNITION SYSTEM STARTING         ║");
    ESP_LOGI("WhoRecognitionCore", "╚════════════════════════════════════════════╝");
    ESP_LOGI("WhoRecognitionCore", "");
    ESP_LOGI("WhoRecognitionCore", "Connecting to Gateway...");
    ESP_LOGI("WhoRecognitionCore", "  Target IP:   %s", GATEWAY_IP);
    ESP_LOGI("WhoRecognitionCore", "  Target Port: %d", GATEWAY_PORT);
    ESP_LOGI("WhoRecognitionCore", "");
    
    // initiate TCP Connection with ESP32 Server 
    bool connected = tcp_connect(GATEWAY_IP, GATEWAY_PORT);

    // Displays connection status and list common failure points & troubleshooting measures if connection fails
    if (!connected) {
        ESP_LOGE("WhoRecognitionCore", "");
        ESP_LOGE("WhoRecognitionCore", "╔════════════════════════════════════════════╗");
        ESP_LOGE("WhoRecognitionCore", "║    GATEWAY CONNECTION FAILED               ║");
        ESP_LOGE("WhoRecognitionCore", "╚════════════════════════════════════════════╝");
        ESP_LOGE("WhoRecognitionCore", "");
        ESP_LOGE("WhoRecognitionCore", "Troubleshooting steps:");
        ESP_LOGE("WhoRecognitionCore", "  1. Check gateway is powered on");
        ESP_LOGE("WhoRecognitionCore", "  2. Verify gateway IP: %s", GATEWAY_IP);
        ESP_LOGE("WhoRecognitionCore", "  3. Confirm both on same WiFi network");
        ESP_LOGE("WhoRecognitionCore", "  4. Check gateway Serial Monitor for IP");
        ESP_LOGE("WhoRecognitionCore", "");
        ESP_LOGW("WhoRecognitionCore", "System will continue without gateway...");
        ESP_LOGW("WhoRecognitionCore", "Face recognition will work, but data won't upload");
        ESP_LOGW("WhoRecognitionCore", "");
    } else {
        ESP_LOGI("WhoRecognitionCore", "");
        ESP_LOGI("WhoRecognitionCore", "╔════════════════════════════════════════════╗");
        ESP_LOGI("WhoRecognitionCore", "║    CONNECTED TO GATEWAY                    ║");
        ESP_LOGI("WhoRecognitionCore", "╚════════════════════════════════════════════╝");
        ESP_LOGI("WhoRecognitionCore", "");
        ESP_LOGI("WhoRecognitionCore", "Starting PIR trigger listener...");
        
        // Start background task to receive PIR trigger commands -- Motion Detection Task 
        // =====================================================
        xTaskCreatePinnedToCore(tcp_recv, "tcp_poll_recv", 4096, NULL, 5, NULL, 0);
        
        ESP_LOGI("WhoRecognitionCore", "System ready");
        ESP_LOGI("WhoRecognitionCore", "  - PIR motion will trigger face recognition");
        ESP_LOGI("WhoRecognitionCore", "  - Results will upload to ThingSpeak");
        ESP_LOGI("WhoRecognitionCore", "");
    }
    
    // MAIN LOOP 
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        // Task is blocked; waits until 1 of the listed bits is set in m_event_group
        // Means that 1 of the below events has occurred (e.g. from ESP-EYE button presses) 
        // There are 5 total possible events, each event handler will be defined later. 
        EventBits_t event_bits = xEventGroupWaitBits(
            m_event_group, 
            RECOGNIZE | ENROLL | DELETE | TASK_PAUSE | TASK_STOP, 
            pdTRUE,    // Clear bits on exit
            pdFALSE,   // Wait for any bit
            portMAX_DELAY); // Blocks task indefinitely until an event occurs
        
        // (1) Handle STOP event
        // If task-stop bit is set, the task logs a shutdown message & terminates.
        if (event_bits & TASK_STOP) {
            ESP_LOGI("WhoRecognitionCore", "Stop signal received, shutting down...");
            break;
        } else if (event_bits & TASK_PAUSE) {
            ESP_LOGI("WhoRecognitionCore", "Task paused");
            xEventGroupSetBits(m_event_group, TASK_PAUSED);
            
            // Logs that task is paused 
            EventBits_t pause_event_bits = xEventGroupWaitBits(
                m_event_group, 
                TASK_RESUME | TASK_STOP, 
                pdTRUE, 
                pdFALSE, 
                portMAX_DELAY);
            
            if (pause_event_bits & TASK_STOP) {
                break;
            } else {
                ESP_LOGI("WhoRecognitionCore", "Task resumed");
                continue;
            }
        }
        
        // (2) Handle RECOGNIZE event when PLAY Button selected 
        if (event_bits & RECOGNIZE) {
            // Clear all previous values stored in event, status, id and similarity 
            event = ""; 
            status = ""; 
            id = ""; 
            similarity = ""; 
            event = "RECOGNIZE";  // store event as RECOGNIZE 
            
            ESP_LOGI("WhoRecognitionCore", "");
            ESP_LOGI("WhoRecognitionCore", "╔════════════════════════════════════════════╗");
            ESP_LOGI("WhoRecognitionCore", "║     FACE RECOGNITION TRIGGERED             ║");
            ESP_LOGI("WhoRecognitionCore", "╚════════════════════════════════════════════╝");
            ESP_LOGI("WhoRecognitionCore", "Processing camera frame...");
            
            // Define callback for when face is detected
            auto new_detect_result_cb = [this](const detect::WhoDetect::result_t &result) {
                
                ESP_LOGI("WhoRecognitionCore", "Face detected in frame");
                ESP_LOGI("WhoRecognitionCore", "Running recognition model...");
                
                // Run face recognition
                auto ret = m_recognizer->recognize(result.img, result.det_res);
                
                // Call detect result callback if registered
                if (m_detect_result_cb) {
                    m_detect_result_cb(result);
                }
                
                // Process recognition results
                if (m_recognition_result_cb) {
                    if (ret.empty()) { 
                        // Face detected but not recognised (i.e. no match -> logs "UNKNOWN")
                        m_recognition_result_cb("who?");
                        status = "0";   // store status as 0 
                        id = "0";
                        similarity = "0.0";
                        
                        ESP_LOGW("WhoRecognitionCore", "");
                        ESP_LOGW("WhoRecognitionCore", "┌────────────────────────────────────────┐");
                        ESP_LOGW("WhoRecognitionCore", "│ RECOGNITION RESULT: UNKNOWN            │");
                        ESP_LOGW("WhoRecognitionCore", "│ Face detected but not in database      │");
                        ESP_LOGW("WhoRecognitionCore", "└────────────────────────────────────────┘");
                        ESP_LOGW("WhoRecognitionCore", "");
                        

                        // keeps video stream running on webpage 
                        set_flag(&shared_mem.stream_flag, 1);
                    } else {   
                        // Face recognised -> logs "RECOGNISED"
                        std::string result_str = std::format("id: {}, sim: {:.2f}", 
                                                            ret[0].id, ret[0].similarity);
                        m_recognition_result_cb(result_str);
                        
                        status = "1";  // store status as 1 
                        id = std::to_string(ret[0].id);  // store id as the detected person's id 
                        similarity = std::to_string(ret[0].similarity);  // store similarity value 
                        
                        ESP_LOGI("WhoRecognitionCore", "");
                        ESP_LOGI("WhoRecognitionCore", "╔════════════════════════════════════════════╗");
                        ESP_LOGI("WhoRecognitionCore", "║     FACE RECOGNIZED                        ║");
                        ESP_LOGI("WhoRecognitionCore", "╚════════════════════════════════════════════╝");
                        ESP_LOGI("WhoRecognitionCore", "  Person ID:   %d", ret[0].id);
                        ESP_LOGI("WhoRecognitionCore", "  Similarity:  %.2f (%.1f%%)", 
                                ret[0].similarity, ret[0].similarity * 100);
                        ESP_LOGI("WhoRecognitionCore", "");
                        // tell web page to send a picture
                        // pause streaming
                        set_flag(&shared_mem.stream_flag, 2);
                    }
                    m_detect->set_detect_result_cb(m_detect_result_cb);
                }

                // Build JSON payload
                json_payload = "{"; 
                json_payload += "\"event\":\"" + event + "\",";
                json_payload += "\"status\":" + status + ",";
                json_payload += "\"id\":" + id + ",";
                json_payload += "\"similarity\":" + similarity;
                json_payload += "}\r";
                
                ESP_LOGI("WhoRecognitionCore", "Sending to gateway...");
                ESP_LOGD("WhoRecognitionCore", "JSON: %s", json_payload.c_str());
                    
                // Send to ESP32 gateway (via tcp client)
                if (tcp_is_connected()) {
                    bool sent = tcp_send(json_payload); // send via tcp_send function declared in tcp_client.cpp
                    if (sent) {
                        ESP_LOGI("WhoRecognitionCore", "Detection data sent to gateway");
                        ESP_LOGI("WhoRecognitionCore", "Gateway will upload to ThingSpeak");
                    } else {
                        ESP_LOGE("WhoRecognitionCore", "Failed to send to gateway");
                    }
                } else {
                    ESP_LOGW("WhoRecognitionCore", "Gateway not connected, data not sent");
                }
                    
                ESP_LOGI("WhoRecognitionCore", "");
            };
            
            
            // Set the callback and continue
            m_detect->set_detect_result_cb(new_detect_result_cb);
            continue;
        }
        
        // (3) HANDLE ENROLL EVENT when UP Button Selected 
        // i.e. user presses enroll button on esp-eye to add a new face to database 
        if (event_bits & ENROLL) {
            ESP_LOGI("WhoRecognitionCore", "");
            ESP_LOGI("WhoRecognitionCore", "╔════════════════════════════════════════════╗");
            ESP_LOGI("WhoRecognitionCore", "║      ENROLLMENT MODE ACTIVATED             ║");
            ESP_LOGI("WhoRecognitionCore", "╚════════════════════════════════════════════╝");
            ESP_LOGI("WhoRecognitionCore", "Look at camera to enroll your face...");
            
            // Creates callback function to run whenever a face is detected in camera frame. 
            auto new_detect_result_cb = [this](const detect::WhoDetect::result_t &result) {
                // calls m_recognizer to send detected face to recognition database
                esp_err_t ret = m_recognizer->enroll(result.img, result.det_res); 
                
                if (m_detect_result_cb) {
                    m_detect_result_cb(result); // notifies external systems
                }
                
                if (m_recognition_result_cb) {
                    if (ret == ESP_FAIL) {
                        m_recognition_result_cb("Failed to enroll.");
                        ESP_LOGE("WhoRecognitionCore", "Enrollment failed");
                        ESP_LOGE("WhoRecognitionCore", " Please try again with better lighting");
                    } else {
                        int num_feats = m_recognizer->get_num_feats(); 
                        std::string msg = std::format("id: {} enrolled.", num_feats);
                        m_recognition_result_cb(msg);
                        
                        ESP_LOGI("WhoRecognitionCore", "");
                        ESP_LOGI("WhoRecognitionCore", "╔════════════════════════════════════════════╗");
                        ESP_LOGI("WhoRecognitionCore", "║     ENROLLMENT SUCCESSFUL                  ║");
                        ESP_LOGI("WhoRecognitionCore", "╚════════════════════════════════════════════╝");
                        ESP_LOGI("WhoRecognitionCore", "  Assigned ID: %d", num_feats);
                        ESP_LOGI("WhoRecognitionCore", "  Total faces: %d", num_feats);
                        ESP_LOGI("WhoRecognitionCore", "");
                    }
                }
                // Restores original callback after completing enrolment task (resume normal detection flow)
                m_detect->set_detect_result_cb(m_detect_result_cb);
            };
            
            m_detect->set_detect_result_cb(new_detect_result_cb);
            continue;
        }
        
        // (4) HANDLE DELETE EVENT when DOWN button selected 
        if (event_bits & DELETE) {
            ESP_LOGI("WhoRecognitionCore", "");
            ESP_LOGI("WhoRecognitionCore", "╔════════════════════════════════════════════╗");
            ESP_LOGI("WhoRecognitionCore", "║       DELETE LAST FACE                     ║");
            ESP_LOGI("WhoRecognitionCore", "╚════════════════════════════════════════════╝");
            
            esp_err_t ret = m_recognizer->delete_last_feat();
            
            if (m_recognition_result_cb) {
                if (ret == ESP_FAIL) {
                    m_recognition_result_cb("Failed to delete.");
                    ESP_LOGE("WhoRecognitionCore", "Delete failed (no faces to delete?)");
                } else {
                    int num_feats = m_recognizer->get_num_feats() + 1; 
                    std::string msg = std::format("id: {} deleted.", num_feats);
                    m_recognition_result_cb(msg);
                    
                    ESP_LOGI("WhoRecognitionCore", "Deleted ID: %d", num_feats);
                    ESP_LOGI("WhoRecognitionCore", "  Remaining faces: %d", 
                            m_recognizer->get_num_feats());
                }
            }
            ESP_LOGI("WhoRecognitionCore", "");
        }
    }
    
    // CLEANUP AND SHUTDOWN
    ESP_LOGI("WhoRecognitionCore", "Task stopping...");
    xEventGroupSetBits(m_event_group, TASK_STOPPED);
    
    // Close TCP connection when event is TASK_STOP 
    tcp_close();

    // Display stop message for readability 
    ESP_LOGI("WhoRecognitionCore", "✓ Task stopped");
    vTaskDelete(NULL);
}

void WhoRecognitionCore::cleanup()
{
    if (m_cleanup) {
        m_cleanup();
    }
}

WhoRecognition::WhoRecognition(frame_cap::WhoFrameCapNode *frame_cap_node) :
    // Creates new detection & recognition tasks (objects)
    m_detect(new detect::WhoDetect("Detect", frame_cap_node)),
    m_recognition(new WhoRecognitionCore("Recognition", m_detect))
{    // registers tasks with WhoTaskGroup
    WhoTaskGroup::register_task(m_detect);
    WhoTaskGroup::register_task(m_recognition);
}

WhoRecognition::~WhoRecognition()
{    // destructor 
    WhoTaskGroup::destroy();
}

void WhoRecognition::set_detect_model(dl::detect::Detect *model)
{    // configures detection task (with specific detection model)
    m_detect->set_model(model);
}

void WhoRecognition::set_recognizer(HumanFaceRecognizer *recognizer)
{    // configures recognition task (with specific face recogniser)
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

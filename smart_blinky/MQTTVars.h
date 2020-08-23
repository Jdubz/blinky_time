// MQTT: ID, server IP, port, username and password
const char* MQTT_CLIENT_ID = "office_rgb_light";
const char* MQTT_SERVER_IP = "10.0.0.124";
const uint16_t MQTT_SERVER_PORT = 1883;
const char* MQTT_USER = "mqtt";
const char* MQTT_PASSWORD = "broker";

// MQTT: topics
// state
const char* MQTT_LIGHT_STATE_TOPIC = "office/rgb1/light/status";
const char* MQTT_LIGHT_COMMAND_TOPIC = "office/rgb1/light/switch";

// brightness
const char* MQTT_LIGHT_BRIGHTNESS_STATE_TOPIC = "office/rgb1/brightness/status";
const char* MQTT_LIGHT_BRIGHTNESS_COMMAND_TOPIC = "office/rgb1/brightness/set";

// colors (rgb)
const char* MQTT_LIGHT_RGB_STATE_TOPIC = "office/rgb1/rgb/status";
const char* MQTT_LIGHT_RGB_COMMAND_TOPIC = "office/rgb1/rgb/set";

// payloads by default (on/off)
const char* LIGHT_ON = "ON";
const char* LIGHT_OFF = "OFF";
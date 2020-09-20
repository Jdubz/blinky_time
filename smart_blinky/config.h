
#define MQTT_ID "Blinky-time"

#define MQTT_USERNAME     "mqtt"
#define MQTT_PASSWORD     "broker"
#define MQTT_SERVER       "192.168.86.23"
#define MQTT_SERVER_PORT  1883

#define MQTT_CONFIG_TOPIC_TEMPLATE  "%s/light/%s/config"

// turn light on/off
#define MQTT_STATE_TOPIC_TEMPLATE   "%s/rgb/light/state"
#define MQTT_COMMAND_TOPIC_TEMPLATE "%s/rgb/light/set"
#define MQTT_STATUS_TOPIC_TEMPLATE  "%s/rgb/status"

#define MQTT_STATE_ON_PAYLOAD   "ON"
#define MQTT_STATE_OFF_PAYLOAD  "OFF"

#define MQTT_HOME_ASSISTANT_DISCOVERY_PREFIX  "homeassistant"

#define MQTT_CONNECTION_TIMEOUT 5000


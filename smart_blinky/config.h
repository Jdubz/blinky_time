
#define MQTT_ID "MQTT RGB Light"

#define MQTT_USERNAME     "mqtt"
#define MQTT_PASSWORD     "broker"
#define MQTT_SERVER       "10.0.0.124"
#define MQTT_SERVER_PORT  1883

#define MQTT_HOME_ASSISTANT_SUPPORT

#if defined(MQTT_HOME_ASSISTANT_SUPPORT)
// template: <discovery prefix>/light/<chip ID>/config, status, state or set
#define MQTT_CONFIG_TOPIC_TEMPLATE  "%s/light/%s/config"
#else

#endif

// turn light on/off
#define MQTT_STATE_TOPIC_TEMPLATE   "%s/rgb/light/state"
#define MQTT_COMMAND_TOPIC_TEMPLATE "%s/rgb/light/set"
#define MQTT_STATE_ON_PAYLOAD   "ON"
#define MQTT_STATE_OFF_PAYLOAD  "OFF"

// is connected
#define MQTT_STATUS_TOPIC_TEMPLATE  "%s/rgb/status" // MQTT connection: alive/dead

#define MQTT_HOME_ASSISTANT_DISCOVERY_PREFIX  "homeassistant"

#define MQTT_CONNECTION_TIMEOUT 5000


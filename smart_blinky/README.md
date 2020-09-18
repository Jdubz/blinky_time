## Wifi enabled 12v RGB led controller with RESTful API.

## ToDo
- implement UDP discovery
- implement RESTful light standard
- GET "/" returns info/control webpage
- GET "/status" returns light status
- create system controller for abstracting state persistance and display
  - multiple interfaces should call the same single class, not Light and ROM.
  - any interface's changes should update other interfaces
- create serial interface for MQTT config

## resources

default mqtt schema: https://www.home-assistant.io/integrations/light.mqtt/
example https://github.com/mertenats/open-home-automation/tree/master/ha_mqtt_rgb_light
with discovery https://github.com/mertenats/Open-Home-Automation/blob/master/ha_mqtt_rgbw_light_with_discovery/ha_mqtt_rgbw_light_with_discovery.ino
Tasmota esp8266 https://github.com/arendst/Tasmota

### Home Assistant Configuration:
  light:
    platform: mqtt
    name: 'MQTT RGB light'
    state_topic: 'office/rgb1/light/status'
    command_topic: 'office/rgb1/light/switch'
    brightness_state_topic: 'office/rgb1/brightness/status'
    brightness_command_topic: 'office/rgb1/brightness/set'
    rgb_state_topic: 'office/rgb1/rgb/status'
    rgb_command_topic: 'office/rgb1/rgb/set'
    optimistic: false
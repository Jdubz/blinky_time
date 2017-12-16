Install the RF24Mesh_Example_Master on one arduino, and RF24Mesh_Example on the rest

The master node will blink blue as it reads messages available on the network.

The other nodes will blink between white and green (when they are connected) or
white and red (when they are disconnected).

Connection status has a delay of around 5s before it is reflected in the lights.

If a node is disconnected, bringing it closer to the master or upping the PA level
will help.


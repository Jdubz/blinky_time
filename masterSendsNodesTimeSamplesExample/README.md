Install the RF24Mesh_Example_Master on one arduino, and RF24Mesh_Example on the rest

The master node will blink blue as it sends messages to the network.

Nodes on the network send a ping every second.

Nodes on the network blink green while they read timestamps from master

Connection status has a delay of around 5s before it is reflected in the lights.

If a node is disconnected, bringing it closer to the master or upping the PA level
will help.


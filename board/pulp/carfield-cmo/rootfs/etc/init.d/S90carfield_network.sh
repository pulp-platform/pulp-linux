#!/bin/sh

# Start the network interface, and let it fail
udhcpc -n

# Insmod carfield.ko (to route Ethernet IRQs via GPIO)
insmod /lib/modules/$(uname -r)/updates/carfield.ko

# Now get your IP address
udhcpc

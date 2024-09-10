#!/bin/bash

cmd="/usr/lib/frr/zebra -A 127.0.0.1 -s 90000000 -M dplane_fpm_nl -M snmp"

hwsku=$(redis-cli -n 4  hget "DEVICE_METADATA|localhost" "hwsku")

if [ $hwsku = "cisco-8101-p4-32x100-vs" ]; then
    cmd="/usr/lib/frr/zebra -A 127.0.0.1 -s 90000000 -M dplane_fpm_sonic -M snmp"
fi

$cmd

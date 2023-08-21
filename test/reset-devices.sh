#!/usr/bin/env bash
# Reset running example container devices and initiate with config x=11, y=22
set -u

echo "reset-devices"

# Data 
# Initial config: Define two interfaces x and y
REQ='<interfaces xmlns="http://openconfig.net/yang/interfaces"/>'
CONFIG='<interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>x</name><config><name>x</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:ethernetCsmacd</type></config></interface><interface><name>y</name><config><name>y</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:atm</type></config></interface></interfaces>'
CHECK='<interfaces xmlns="http://openconfig.net/yang/interfaces"><interface><name>y</name><config><name>y</name><type xmlns:ianaift="urn:ietf:params:xml:ns:yang:iana-if-type">ianaift:atm</type></config></interface></interfaces>'

# Add hostname
i=1
for ip in $CONTAINERS; do
    NAME="$IMG$i"
    echo "set hostname $NAME $ip"
    ret=$(ssh -l $USER $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" 
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" 
     message-id="42">
  <edit-config>
    <target>
      <candidate/>
    </target>
    <default-operation>replace</default-operation>
    <config>
       <system xmlns="http://openconfig.net/yang/system">
          <config>
             <hostname>${NAME}</hostname>
          </config>
       </system>
    </config>
  </edit-config>
</rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="42">
  <commit/>
</rpc>]]>]]>    
EOF
)
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf system rpc-error detected"
        echo "$ret"
        exit 1
    fi
    i=$((i+1))
done
          
i=1
for ip in $CONTAINERS; do
    NAME=$IMG$i
    echo "init config: $NAME"
    ret=$(ssh -l $USER $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" 
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" 
     message-id="42">
  <edit-config>
    <target>
      <candidate/>
    </target>
    <default-operation>merge</default-operation>
    <config>
      $CONFIG
    </config>
  </edit-config>
</rpc>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="42">
  <commit/>
</rpc>]]>]]>
EOF
       )
    match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        echo "netconf interfaces rpc-error detected"
        exit 1
    fi
    i=$((i+1))
done

# Check config
i=1
for ip in $CONTAINERS; do
    NAME=$IMG$i
    echo "Check config $NAME"
    ret=$(ssh -l $USER $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
  xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
  message-id="42">
  <get-config>
    <source>
      <running/>
    </source>
    <filter type='subtree'>
      ${REQ}
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#    echo "ret:$ret"
    match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
	echo "netconf rpc-error detected"
        echo "$ret"
	exit 1
    fi
    match=$(echo "$ret" | grep --null -Eo "$CONFIG") || true
    if [ -z "$match" ]; then
	echo "Config of device $i not matching:"
	echo "$ret"
        echo "Expected:"
        echo "$CONFIG"
	exit 1
    fi
    i=$((i+1))
done

echo "reset-devices OK"

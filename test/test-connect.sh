# Controller initial connect and push when no devices are still present,
# I.e., boostrapping from empty config
# See also https://github.com/clicon/clixon-controller/issues/5
# The test is essentially a precursor to test-cli-edit-commit-push but with no pre-configuration
# I.e., run reset-controller.sh from cli
# Start w no config
# 1. Open first device
# 2. Open second device
# Reset and do same with device-profile

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

if [ $nr -lt 2 ]; then
    echo "Test requires nr=$nr to be greater than 1"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

if [[ ! -v CONTAINERS ]]; then
    err1 "CONTAINERS variable set" "not set"
fi

# Default container name, postfixed with 1,2,..,<nr>
: ${IMG:=clixon-example}

dir=/var/tmp/$0
CFG=$dir/controller.xml
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD
fyang=$dir/clixon-test@2023-03-22.yang
# openconfig devices have noc user
: ${USER:=noc}

cat<<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$CFG</CLICON_CONFIGFILE>
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_CONFIG_EXTEND>clixon-controller-config</CLICON_CONFIG_EXTEND>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_YANG_DIR>${DATADIR}/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/common</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>${DATADIR}/controller/main</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_YANG_DOMAIN_DIR>$dir</CLICON_YANG_DOMAIN_DIR>
  <CLICON_CLI_MODE>operation</CLICON_CLI_MODE>
  <CLICON_CLI_DIR>${LIBDIR}/controller/cli</CLICON_CLI_DIR>
  <CLICON_CLISPEC_DIR>${LIBDIR}/controller/clispec</CLICON_CLISPEC_DIR>
  <CLICON_BACKEND_DIR>${LIBDIR}/controller/backend</CLICON_BACKEND_DIR>
  <CLICON_SOCK>${LOCALSTATEDIR}/run/controller.sock</CLICON_SOCK>
  <CLICON_SOCK_GROUP>${CLICON_GROUP}</CLICON_SOCK_GROUP>
  <CLICON_SOCK_PRIO>true</CLICON_SOCK_PRIO>
  <CLICON_BACKEND_PIDFILE>${LOCALSTATEDIR}/run/controller.pid</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_XMLDB_MULTI>true</CLICON_XMLDB_MULTI>
  <CLICON_STARTUP_MODE>init</CLICON_STARTUP_MODE>
  <CLICON_STREAM_DISCOVERY_RFC5277>true</CLICON_STREAM_DISCOVERY_RFC5277>
  <CLICON_RESTCONF_USER>${CLICON_USER}</CLICON_RESTCONF_USER>
  <CLICON_RESTCONF_PRIVILEGES>drop_perm</CLICON_RESTCONF_PRIVILEGES>
  <CLICON_RESTCONF_INSTALLDIR>${SBINDIR}</CLICON_RESTCONF_INSTALLDIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_HELPSTRING_TRUNCATE>true</CLICON_CLI_HELPSTRING_TRUNCATE>
  <CLICON_CLI_HELPSTRING_LINES>1</CLICON_CLI_HELPSTRING_LINES>
  <CLICON_CLI_OUTPUT_FORMAT>text</CLICON_CLI_OUTPUT_FORMAT>
  <CLICON_YANG_SCHEMA_MOUNT>true</CLICON_YANG_SCHEMA_MOUNT>
  <CLICON_YANG_SCHEMA_MOUNT_SHARE>true</CLICON_YANG_SCHEMA_MOUNT_SHARE>
</clixon-config>
EOF

cat <<EOF > $CFD/autocli.xml
<clixon-config xmlns="http://clicon.org/config">
  <autocli>
     <module-default>false</module-default>
     <list-keyword-default>kw-nokey</list-keyword-default>
     <treeref-state-default>true</treeref-state-default>
     <grouping-treeref>true</grouping-treeref>
     <rule>
       <name>include controller</name>
       <module-name>clixon-controller</module-name>
       <operation>enable</operation>
     </rule>
  </autocli>
</clixon-config>
EOF

cat <<EOF > $fyang
module clixon-test {
    namespace "http://clicon.org/test";
    prefix test;
    import ietf-inet-types { prefix inet; }
    import clixon-controller { prefix ctrl; }
    revision 2023-03-22{
        description "Initial prototype";
    }
    augment "/ctrl:services" {
        list test {
            key service-name;
            leaf service-name {
                type string;
            }
            description "Test service";
            list parameter {
                key name;
                leaf name{
                    type string;
                }
                leaf value{
                    type inet:ipv4-address;
                }
            }
        }
    }
}
EOF

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "Check backend is running"
wait_backend

# Reset controller
ii=1
for ip in $CONTAINERS; do
    NAME="$IMG$ii"
    cmd="set devices device $NAME enabled true"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

    cmd="set devices device $NAME conn-type NETCONF_SSH"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"
    
    cmd="set devices device $NAME user $USER"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"
    
    cmd="set devices device $NAME addr $ip"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

    new "commit"
    expectpart "$($clixon_cli -1 -m configure -f $CFG commit local)" 0 "^$"

    #    sleep $sleep

    new "connection open 1"
    expectpart "$($clixon_cli -1 -f $CFG connection open)" 0 "^$"

    sleep $sleep
    
    new "Verify controller $NAME"
    res=$(${clixon_cli} -1f $CFG show connections | grep OPEN | wc -l)

    if [ "$res" != "$ii" ]; then
        err1 "$ii open devices" "$res"
    fi

    ii=$((ii+1))
done

new "connection close"
expectpart "$($clixon_cli -1 -f $CFG connection close)" 0 "^$"

# see https://github.com/clicon/clixon-controller/issues/98
cmd="set devices device openconfig1 user wrong"
new "Set wrong user: $cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

new "commit"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit local)" 0 "^$"

new "connection open 2"
expectpart "$($clixon_cli -1 -f $CFG connection open)" 0 "^$"

sleep $sleep

new "Verify controller $NAME"
res=$(${clixon_cli} -1f $CFG show connections | grep OPEN | wc -l)

nr1=$((nr-1))
if [ "$res" != "$nr1" ]; then
    err1 "$nr open devices" "$res"
fi

new "Check errmsg"
expectpart "$($clixon_cli -1 -f $CFG show transaction)" 0 "<result>FAILED</result>" "<reason>wrong@" "Permission denied (publickey,password,keyboard-interactive).</reason>" || true
  
cmd="set devices device openconfig1 user $USER"
new "Set right user: $cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

new "commit"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit local)" 0 "^$"

# device profile

new "Delete devices config"
expectpart "$($clixon_cli -1 -m configure -f $CFG delete devices)" 0 "^$"

new "commit local"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit local)" 0 "^$"

new "Verify controller 1"
res=$(${clixon_cli} -1f $CFG show connections | grep OPEN | wc -l)

if [ "$res" != "0" ]; then
    echo "Error: $res devices open, expected 0"
    exit -1;
fi

# Create device-profile myprofile
cmd="set devices device-profile myprofile user $USER"
new "$cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

cmd="set devices device-profile myprofile ssh-stricthostkey true"
new "$cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

cmd="set devices device-profile myprofile conn-type NETCONF_SSH"
new "$cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

cmd="set devices device-profile myprofile yang-config BIND"
new "$cmd"
expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

new "commit"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit local)" 0 "^$"

ii=1
for ip in $CONTAINERS; do
    NAME="$IMG$ii"
    cmd="set devices device $NAME device-profile myprofile"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"
    
    cmd="set devices device $NAME enabled true"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"

    cmd="set devices device $NAME addr $ip"
    new "$cmd"
    expectpart "$($clixon_cli -1 -m configure -f $CFG $cmd)" 0 "^$"
    
    ii=$((ii+1))
done

new "commit"
expectpart "$($clixon_cli -1 -m configure -f $CFG commit local)" 0 "^$"
    
new "connection open 3"
expectpart "$($clixon_cli -1 -f $CFG connection open)" 0 "^$"

sleep $sleep
    
new "Verify controller 2"
res=$(${clixon_cli} -1f $CFG show connections | grep OPEN | wc -l)

nr=$((ii-1))
if [ "$res" != "$nr" ]; then
    err1 "$nr devices" "$res"
fi

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

unset nr
unset ii

endtest

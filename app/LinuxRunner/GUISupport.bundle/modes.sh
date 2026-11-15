#!/bin/sh
# LinuxRunner Modes (Settings > Modes): opt-in services, run as root.
#   modes.sh status              write modes-status for the app
#   modes.sh start rdp|ssh       start a mode in the background
#   modes.sh start liftoff|pocket <user>   Liftoff (~/Sites on :8080) and
#                                Pocket Cloud (~/Pocket Cloud over WebDAV on
#                                :8081), run as that Linux user
#   modes.sh stop <mode>         stop it
#   modes.sh agent <user>        open the Agent Mode launcher as <user>
# Nothing here starts by itself; the app runs it only when the user asks.
# RDP Mode shares the desktop that is already running (its local, password
# protected VNC bridge) through xrdp; the RDP password is that desktop's
# session password, shown in the app and typed on xrdp's own login screen
# (no autorun: Remote Desktop clients do not pass their saved password to
# xrdp without NLA, which xrdp's VNC sessions do not use). SSH Mode uses the Linux users' own
# passwords; root login is refused.
set -u
cd "$(dirname "$0")" || exit 1
state=/run/linuxrunner-modes
mkdir -p "$state" && chmod 700 "$state"
rdp_port=3389 ssh_port=2222 liftoff_port=8080 pocket_port=8081
have() { command -v "$1" >/dev/null 2>&1; }
alive() { [ -s "$state/$1.pid" ] && kill -0 "$(cat "$state/$1.pid")" 2>/dev/null; }

# Port of the running desktop's x11vnc (newest session whose x11vnc is alive).
desktop_port() {
    for port_file in $(ls -t session-*/port 2>/dev/null); do
        port=$(cat "$port_file" 2>/dev/null) || continue
        for cmdline in /proc/[0-9]*/cmdline; do
            tr '\0' ' ' < "$cmdline" 2>/dev/null | grep -q "x11vnc.*-rfbport $port " && { echo "$port"; return 0; }
        done
    done
    return 1
}

write_status() {
    {
        if have xrdp; then echo rdp_installed=1; fi
        if [ -x /usr/sbin/sshd ]; then echo ssh_installed=1; fi
        agents=
        for command in claude codex opencode; do have "$command" && agents="$agents $command"; done
        echo "agents=${agents# }"
        if alive rdp; then echo "rdp=running port=$rdp_port"; else echo rdp=stopped; fi
        if alive ssh; then echo "ssh=running port=$ssh_port"; else echo ssh=stopped; fi
        for m in liftoff pocket arcadia; do [ -f "/var/lib/linuxrunner/mode-$m" ] && echo "${m}_installed=1"; done
        if alive liftoff; then echo "liftoff=running port=$liftoff_port"; else echo liftoff=stopped; fi
        if alive pocket; then echo "pocket=running port=$pocket_port user=linuxrunner password=$(cat "$state/pocket.password" 2>/dev/null)"; else echo pocket=stopped; fi
        if port=$(desktop_port); then echo "desktop=running vnc_port=$port"; else echo desktop=stopped; fi
    } > modes-status.tmp && mv modes-status.tmp modes-status
    # Service logs for the Desktop Report (no passwords are logged).
    for m in rdp ssh liftoff pocket; do [ -f "$state/$m.log" ] && { echo "== $m"; tail -n 60 "$state/$m.log"; }; done > modes.log 2>/dev/null
    [ ! -f /var/log/xrdp-linuxrunner.log ] || { echo "== xrdp"; tail -n 80 /var/log/xrdp-linuxrunner.log; } >> modes.log
    cat modes-status
}

start_rdp() {
    have xrdp || { echo 'MODES: rdp=FAIL reason=not-installed'; return 1; }
    port=$(desktop_port) || { echo 'MODES: rdp=FAIL reason=desktop-not-running'; return 1; }
    alive rdp && kill "$(cat "$state/rdp.pid")" 2>/dev/null
    mkdir -p /etc/xrdp /run/xrdp /var/run/xrdp
    # TLS identity (self-signed, made on this device) and the legacy RDP keys.
    if [ ! -s /etc/xrdp/cert.pem ] || [ ! -s /etc/xrdp/key.pem ]; then
        have openssl && openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=LinuxRunner \
            -keyout /etc/xrdp/key.pem -out /etc/xrdp/cert.pem >/dev/null 2>&1
        chmod 600 /etc/xrdp/key.pem 2>/dev/null
    fi
    [ -s /etc/xrdp/rsakeys.ini ] || { have xrdp-keygen && xrdp-keygen xrdp /etc/xrdp/rsakeys.ini >/dev/null 2>&1; }
    # LinuxRunner's own configuration; the distribution's xrdp.ini is kept.
    # ip=::ffff:127.0.0.1: xrdp turns "127.0.0.1" into IPv6 ::1, where the
    # desktop's x11vnc (-localhost -no6) does not listen; the IPv4-mapped
    # form reaches its IPv4 loopback socket.
    cat > /etc/xrdp/linuxrunner.ini <<EOF
[Globals]
ini_version=1
fork=true
port=$rdp_port
tcp_nodelay=true
tcp_keepalive=true
security_layer=negotiate
crypt_level=high
certificate=/etc/xrdp/cert.pem
key_file=/etc/xrdp/key.pem
ssl_protocols=TLSv1.2, TLSv1.3
allow_channels=true
allow_multimon=false
bitmap_cache=true
bitmap_compression=true
bulk_compression=true
max_bpp=24
new_cursors=true
use_fastpath=both
ls_title=LinuxRunner Desktop - enter the password shown in LinuxRunner
[Logging]
LogFile=/var/log/xrdp-linuxrunner.log
LogLevel=INFO
EnableSyslog=false
[Channels]
rdpdr=false
rdpsnd=false
drdynvc=false
cliprdr=true
rail=false
xrdpvr=false
[LinuxRunner]
name=LinuxRunner Desktop
lib=libvnc.so
ip=::ffff:127.0.0.1
port=$port
username=na
password=ask
EOF
    chmod 600 /etc/xrdp/linuxrunner.ini
    setsid nohup xrdp --nodaemon --config /etc/xrdp/linuxrunner.ini > "$state/rdp.log" 2>&1 < /dev/null &
    echo $! > "$state/rdp.pid"
    sleep 2
    alive rdp || { echo 'MODES: rdp=FAIL reason=xrdp-exited'; tail -n 5 "$state/rdp.log"; return 1; }
    echo "MODES: rdp=PASS port=$rdp_port desktop_vnc_port=$port"
}

start_ssh() {
    [ -x /usr/sbin/sshd ] || { echo 'MODES: ssh=FAIL reason=not-installed'; return 1; }
    alive ssh && kill "$(cat "$state/ssh.pid")" 2>/dev/null
    mkdir -p /run/sshd && chmod 755 /run/sshd
    ssh-keygen -A >/dev/null 2>&1
    setsid nohup /usr/sbin/sshd -D -e -p "$ssh_port" -o PermitRootLogin=no -o PasswordAuthentication=yes \
        -o PidFile="$state/sshd.pid" > "$state/ssh.log" 2>&1 < /dev/null &
    echo $! > "$state/ssh.pid"
    sleep 2
    alive ssh || { echo 'MODES: ssh=FAIL reason=sshd-exited'; tail -n 5 "$state/ssh.log"; return 1; }
    echo "MODES: ssh=PASS port=$ssh_port"
}

# Run a command as a Linux user in its own session (process group); prints the pid.
as_user() { # user log command
    user=$1 log=$2; shift 2
    if [ "$user" = root ]; then setsid nohup /bin/sh -c "$*" > "$log" 2>&1 < /dev/null &
    else setsid nohup su -l "$user" -s /bin/sh -c "$*" > "$log" 2>&1 < /dev/null &
    fi
    echo $!
}
user_home() { home=$(getent passwd "$1" 2>/dev/null | cut -d: -f6); [ -n "$home" ] || home=/root; echo "$home"; }

start_liftoff() {
    user=$1; home=$(user_home "$user")
    have python3 || { echo 'MODES: liftoff=FAIL reason=not-installed'; return 1; }
    stop_mode liftoff > /dev/null
    if [ ! -d "$home/Sites" ]; then
        mkdir -p "$home/Sites"
        cp ./liftoff-index.html "$home/Sites/index.html" 2>/dev/null
        [ "$user" = root ] || chown -R "$user:" "$home/Sites" 2>/dev/null
    fi
    as_user "$user" "$state/liftoff.log" "cd '$home/Sites' && exec python3 -m http.server $liftoff_port --bind 0.0.0.0" > "$state/liftoff.pid"
    sleep 3
    alive liftoff || { echo 'MODES: liftoff=FAIL reason=server-exited'; tail -n 5 "$state/liftoff.log"; return 1; }
    echo "MODES: liftoff=PASS port=$liftoff_port"
}

start_pocket() {
    user=$1; home=$(user_home "$user")
    venv=/opt/linuxrunner/pocket-cloud
    [ -x "$venv/bin/wsgidav" ] || { echo 'MODES: pocket=FAIL reason=not-installed'; return 1; }
    stop_mode pocket > /dev/null
    folder="$home/Pocket Cloud"; mkdir -p "$folder"; [ "$user" = root ] || chown "$user:" "$folder" 2>/dev/null
    # A new password each start (shown in the app): 12 letters and digits.
    password=$(tr -dc 'A-HJ-NP-Za-km-z2-9' < /dev/urandom 2>/dev/null | head -c 12)
    [ -n "$password" ] || { echo 'MODES: pocket=FAIL reason=no-random'; return 1; }
    printf '%s\n' "$password" > "$state/pocket.password"; chmod 600 "$state/pocket.password"
    config=/run/linuxrunner-pocket.yaml
    {
        echo 'host: 0.0.0.0'; echo "port: $pocket_port"; echo 'server: cheroot'
        echo 'provider_mapping:'; echo "  \"/\": \"$folder\""
        echo 'http_authenticator:'; echo '  domain_controller: null'; echo '  accept_basic: true'; echo '  accept_digest: true'; echo '  default_to_digest: true'
        echo 'simple_dc:'; echo '  user_mapping:'; echo '    "*":'; echo '      "linuxrunner":'; echo "        password: \"$password\""
        echo 'dir_browser:'; echo '  enable: true'
        echo 'verbose: 2'
    } > "$config"
    [ "$user" = root ] || chown "$user:" "$config"; chmod 600 "$config"
    as_user "$user" "$state/pocket.log" "exec $venv/bin/wsgidav --config=$config" > "$state/pocket.pid"
    sleep 5
    alive pocket || { echo 'MODES: pocket=FAIL reason=server-exited'; tail -n 5 "$state/pocket.log"; return 1; }
    echo "MODES: pocket=PASS port=$pocket_port"
}

stop_mode() {
    if alive "$1"; then pid=$(cat "$state/$1.pid"); kill -TERM -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null; fi
    rm -f "$state/$1.pid"; echo "MODES: $1=stopped"
}

code=0
case "${1:-status}" in
    status) ;;
    start)
        user=${3:-root}
        case "$user" in *[!a-z0-9_-]*|'') exit 2;; esac
        case "${2:-}" in
            rdp) start_rdp || code=1;; ssh) start_ssh || code=1;;
            liftoff) start_liftoff "$user" || code=1;; pocket) start_pocket "$user" || code=1;;
            *) exit 2;;
        esac;;
    stop)
        case "${2:-}" in rdp|ssh|liftoff|pocket) stop_mode "$2";; *) exit 2;; esac;;
    agent)
        user=${2:-root}
        case "$user" in *[!a-z0-9_-]*|'') exit 2;; esac
        have lr-agent || { echo 'Agent Mode is not installed. Install it in Settings > Modes.'; exit 1; }
        if [ "$user" = root ]; then exec lr-agent; fi
        exec su -l "$user" -c lr-agent;;
    *) exit 2;;
esac
write_status > /dev/null
cat modes-status
exit "$code"

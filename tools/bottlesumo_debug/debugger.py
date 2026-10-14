#!/usr/bin/env python3
"""BottleSumo bench dashboard. Python 3.10+, optional pyserial for USB."""
import argparse
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import socket
import struct
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CONFIG = Path(__file__).with_name('wireless.local.json')
PACKET = struct.Struct('<IHHIIII4H3H3H2h2h2I5I4B')
STALE_SECONDS = 0.3
FLAGS_READY = 2 | 4 | 16


def parse_packet(raw):
    if len(raw) != PACKET.size:
        raise ValueError('Telemetry must contain 84 bytes')
    v = PACKET.unpack(raw)
    if v[:3] != (0x42534D54, 1, 84):
        raise ValueError('Unsupported telemetry header')
    return dict(boot=v[3], sequence=v[4], uptime_ms=v[5], state_elapsed_ms=v[6],
                qre=list(v[7:11]), distance=list(v[11:14]), range_age=list(v[14:17]),
                requested=list(v[17:19]), applied=list(v[19:21]), fg=list(v[21:23]),
                control_max_us=v[23], deadline_misses=v[24], espnow_ok=v[25],
                espnow_fail=v[26], udp_fail=v[27], state=v[28], edges=v[29], flags=v[30])


class Telemetry:
    def __init__(self):
        self.lock = threading.Lock()
        self.packet = None
        self.received = 0.0
        self.accepted = self.gaps = self.out_of_order = self.invalid = 0
        self.history = []

    def ingest(self, raw, now=None):
        now = time.monotonic() if now is None else now
        try:
            packet = parse_packet(raw)
        except ValueError:
            with self.lock:
                self.invalid += 1
            return False
        with self.lock:
            if self.packet and packet['boot'] == self.packet['boot']:
                delta = (packet['sequence'] - self.packet['sequence']) & 0xffffffff
                if delta == 0 or delta >= 0x80000000:
                    self.out_of_order += 1
                    return False
                self.gaps += delta - 1
            elif self.packet:
                self.history.clear()
                self.gaps = self.out_of_order = 0
            self.packet, self.received = packet, now
            self.accepted += 1
            self.history.append(dict(packet, received_s=now))
            self.history = self.history[-240:]
        return True

    def snapshot(self, now=None):
        now = time.monotonic() if now is None else now
        with self.lock:
            age = now - self.received if self.packet else None
            flags = self.packet['flags'] if self.packet else 0
            ready = age is not None and age < STALE_SECONDS and flags & FLAGS_READY == FLAGS_READY and not flags & (1 | 8)
            return dict(packet=self.packet, history=list(self.history), age_ms=None if age is None else round(age * 1000),
                        stale=age is None or age >= STALE_SECONDS, drive_ready=ready,
                        accepted=self.accepted, gaps=self.gaps, out_of_order=self.out_of_order, invalid=self.invalid)


def command_bytes(token, boot, sequence, left=0, right=0, stop=False):
    if any(type(n) is not int for n in (boot, sequence, left, right)):
        raise ValueError('Command fields must be integers')
    if not (0 <= boot <= 0xffffffff and 0 <= sequence <= 0xffffffff and -255 <= left <= 255 and -255 <= right <= 255):
        raise ValueError('Command out of range')
    header = struct.pack('<IHHIIhh', 0x4253434D, 1, 2 if stop else 1, boot, sequence, 0 if stop else left, 0 if stop else right)
    return header + hmac.new(token.encode('utf-8'), header, hashlib.sha256).digest()


def ota_auth(token, boot, size, digest):
    return hmac.new(token.encode('utf-8'), f'ota\n{boot}\n{size}\n{digest}'.encode('utf-8'), hashlib.sha256).hexdigest()


def validate_config(config, require_secrets=True):
    keys = {'wifi_password', 'ota_token', 'wifi_channel', 'robot_mac', 'receiver_mac', 'espnow_pmk', 'espnow_lmk'}
    optional = {'sta_ssid', 'sta_password'}
    if type(config) is not dict or not keys <= set(config) or set(config) - keys - optional:
        raise ValueError('Invalid wireless configuration keys')
    config = dict(config, sta_ssid=config.get('sta_ssid', ''), sta_password=config.get('sta_password', ''))
    ssid, sta_password = config['sta_ssid'], config['sta_password']
    if type(ssid) is not str or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in ssid):
        raise ValueError('Invalid sta_ssid')
    try:
        ssid_size = len(ssid.encode('utf-8'))
    except UnicodeError as exc:
        raise ValueError('Invalid sta_ssid UTF-8') from exc
    if ssid_size > 32:
        raise ValueError('sta_ssid must fit in 32 UTF-8 bytes')
    if (type(sta_password) is not str or any(ord(c) < 32 or ord(c) > 126 for c in sta_password)
            or (not ssid and sta_password) or (ssid and not 8 <= len(sta_password) <= 63)):
        raise ValueError('sta_password must be blank without an SSID, otherwise WPA2 ASCII 8–63 characters')
    for key in keys - {'wifi_channel'}:
        value = config[key]
        if type(value) is not str or any(ord(c) < 32 or ord(c) > 126 for c in value):
            raise ValueError(f'Invalid {key}')
    password = config['wifi_password']
    if (require_secrets or password) and not 8 <= len(password) <= 63:
        raise ValueError('WPA2 password must contain 8–63 printable ASCII characters')
    for key, size in (('ota_token', 64), ('espnow_pmk', 32), ('espnow_lmk', 32)):
        if (require_secrets or config[key]) and not re.fullmatch('[0-9a-fA-F]{' + str(size) + '}', config[key]):
            raise ValueError(f'Invalid {key}')
    for key in ('robot_mac', 'receiver_mac'):
        if not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', config[key]):
            raise ValueError(f'Invalid {key}')
    if type(config['wifi_channel']) is not int or not 1 <= config['wifi_channel'] <= 11:
        raise ValueError('wifi_channel must be an integer from 1 to 11')
    return config


def load_config(path=CONFIG):
    if path.is_symlink():
        raise ValueError('Refusing symlink configuration')
    return validate_config(json.loads(path.read_text(encoding='utf-8')))


def atomic_private(path, content):
    path = Path(path)
    if path.is_symlink():
        raise ValueError('Refusing symlink output')
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        os.chmod(tmp, 0o600)
        with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        if path.is_symlink():
            raise ValueError('Refusing symlink output')
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def configure(path=CONFIG, robot_mac=None, receiver_mac=None):
    path = Path(path)
    if path.is_symlink():
        raise ValueError('Refusing symlink output')
    if path.exists():
        config = validate_config(json.loads(path.read_text(encoding='utf-8')), False)
    else:
        config = dict(wifi_password='', ota_token='', wifi_channel=6, robot_mac='00:00:00:00:00:00',
                      receiver_mac='00:00:00:00:00:00', espnow_pmk='', espnow_lmk='', sta_ssid='', sta_password='')
    for key, factory in (('wifi_password', lambda: ''.join(secrets.choice('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789') for _ in range(20))),
                         ('ota_token', lambda: secrets.token_hex(32)), ('espnow_pmk', lambda: secrets.token_hex(16)), ('espnow_lmk', lambda: secrets.token_hex(16))):
        if not config[key]:
            config[key] = factory()
    if robot_mac is not None:
        config['robot_mac'] = robot_mac
    if receiver_mac is not None:
        config['receiver_mac'] = receiver_mac
    config = validate_config(config)
    atomic_private(path, json.dumps(config, indent=2) + '\n')
    return config


NETWORK_SETTINGS = struct.Struct('<IHH64s65s33s64s6s6s16s16sB')


def network_settings_bytes(config):
    config = validate_config(config)
    return NETWORK_SETTINGS.pack(
        0x42534e43, 1, NETWORK_SETTINGS.size,
        config['wifi_password'].encode('ascii'), config['ota_token'].encode('ascii'),
        config['sta_ssid'].encode('utf-8'), config['sta_password'].encode('ascii'),
        bytes.fromhex(config['robot_mac'].replace(':', '')),
        bytes.fromhex(config['receiver_mac'].replace(':', '')),
        bytes.fromhex(config['espnow_pmk']), bytes.fromhex(config['espnow_lmk']), config['wifi_channel'])


def provision(config, serial_port):
    payload = b'NET:' + network_settings_bytes(config).hex().encode('ascii') + b'\n'
    try:
        import serial
    except ImportError as exc:
        raise ValueError('USB provisioning requires: python -m pip install pyserial==3.5') from exc
    connection = None
    try:
        # Set modem lines before opening; no explicit reset or network action is sent.
        connection = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=1)
        connection.dtr = False
        connection.rts = False
        connection.port = serial_port
        connection.open()
        written = connection.write(payload)
        if written != len(payload):
            raise OSError('Partial provision write')
        deadline = time.monotonic() + 15
        buffered = b''
        while time.monotonic() < deadline:
            buffered += connection.read_until(b'\n', 512)
            while b'\n' in buffered:
                line, buffered = buffered.split(b'\n', 1)
                if line.rstrip(b'\r') == b'NET:OK':
                    return
                if line.rstrip(b'\r') == b'NET:REJECT':
                    raise ValueError('Device rejected configuration; stop the device and check private configuration.')
            if len(buffered) > 1024:
                buffered = b''
        raise OSError('Provision acknowledgement timed out')
    except OSError as exc:
        raise ValueError('Provision outcome may be ambiguous. No retry performed; inspect the device before retrying.') from exc
    finally:
        if connection is not None:
            connection.close()


class Transport:
    def __init__(self, telemetry, robot, serial_port=None):
        self.telemetry, self.robot = telemetry, robot
        self.lock = threading.Lock()
        self.closed = threading.Event()
        self.error = None
        self.serial = None
        self.sock = None
        if serial_port:
            try:
                import serial
            except ImportError as exc:
                raise ValueError('USB transport requires: python -m pip install pyserial==3.5') from exc
            self.serial = serial.Serial(serial_port, 115200, timeout=0.1, write_timeout=0.05)
        else:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.bind(('0.0.0.0', 4242))
            self.sock.settimeout(0.1)
        self.thread = threading.Thread(target=self.receive, daemon=True)
        self.thread.start()

    def send(self, raw):
        with self.lock:
            if self.serial:
                self.serial.write(b'C:' + raw.hex().encode('ascii') + b'\n')
            else:
                self.sock.sendto(raw, (self.robot, 4242))

    def receive(self):
        next_sub = 0
        serial_buffer = b''
        try:
            while not self.closed.is_set():
                if self.serial:
                    serial_buffer += self.serial.read_until(b'\n', 512)
                    while b'\n' in serial_buffer:
                        line, serial_buffer = serial_buffer.split(b'\n', 1)
                        if line.startswith(b'T:'):
                            try:
                                if len(line.rstrip(b'\r')) != 170:
                                    raise ValueError('Invalid serial frame length')
                                self.telemetry.ingest(bytes.fromhex(line[2:].decode('ascii').strip()))
                            except (UnicodeError, ValueError):
                                self.telemetry.ingest(b'')
                    if len(serial_buffer) > 512:
                        serial_buffer = b''
                        self.telemetry.ingest(b'')
                else:
                    if time.monotonic() >= next_sub:
                        self.sock.sendto(b'SUB\n', (self.robot, 4242))
                        next_sub = time.monotonic() + 1
                    try:
                        raw, peer = self.sock.recvfrom(512)
                        if peer == (self.robot, 4242):
                            self.telemetry.ingest(raw)
                    except socket.timeout:
                        pass
        except (OSError, ValueError):
            self.error = 'Transport disconnected; restart the dashboard after reconnecting.'
            self.closed.set()

    def close(self):
        self.closed.set()
        self.thread.join(timeout=0.3)
        if self.serial:
            self.serial.close()
        if self.sock:
            self.sock.close()


class Controller:
    def __init__(self, telemetry, transport, token):
        self.telemetry, self.transport, self.token = telemetry, transport, token
        self.lock = threading.Lock()
        self.desired = None
        self.expires = 0
        self.request_id = -1
        self.desired_boot = None
        self.sequence = 0
        self.boot = None
        self.closed = threading.Event()
        self.error = None

    def request(self, body, now=None):
        now = time.monotonic() if now is None else now
        if type(body) is not dict or body.get('action') not in ('drive', 'stop'):
            raise ValueError('Unknown command')
        request_id = body.get('request_id')
        if type(request_id) is not int or not 0 <= request_id <= 9007199254740991:
            raise ValueError('Invalid request ordering')
        if body['action'] == 'stop':
            if set(body) != {'action', 'request_id'}:
                raise ValueError('Unexpected command fields')
            with self.lock:
                if request_id <= self.request_id:
                    raise ValueError('Old command')
                self.request_id = request_id
                self.desired = None
                self.transmit(0, 0, True)
            return
        if set(body) != {'action', 'request_id', 'left', 'right'} or any(type(body[n]) is not int or not -120 <= body[n] <= 120 for n in ('left', 'right')):
            raise ValueError('Bench PWM must be an integer from -120 to 120')
        with self.lock:
            if request_id <= self.request_id:
                raise ValueError('Old command')
            self.request_id = request_id
            if not self.telemetry.snapshot(now)['drive_ready'] or self.transport.closed.is_set():
                raise ValueError('Fresh calibrated, hardware-confirmed bench telemetry is required')
            self.desired = (body['left'], body['right'])
            self.desired_boot = self.telemetry.snapshot(now)['packet']['boot']
            self.expires = now + 0.1

    def transmit(self, left, right, stop):
        packet = self.telemetry.snapshot()['packet']
        if packet is None:
            return
        if self.boot != packet['boot']:
            self.boot, self.sequence = packet['boot'], packet['uptime_ms']
        candidate = (self.sequence + 1) & 0xffffffff
        uptime = packet['uptime_ms']
        delta = (uptime - candidate) & 0xffffffff
        self.sequence = uptime if 0 < delta < 0x80000000 else candidate
        try:
            self.transport.send(command_bytes(self.token, self.boot, self.sequence, left, right, stop))
        except (OSError, ValueError):
            self.desired = None
            self.error = 'Command transport failed; firmware lease will stop the motors.'

    def tick(self, now=None):
        now = time.monotonic() if now is None else now
        with self.lock:
            if self.desired is None:
                return
            snapshot = self.telemetry.snapshot(now)
            if (now >= self.expires or not snapshot['drive_ready'] or self.transport.closed.is_set()
                    or snapshot['packet']['boot'] != self.desired_boot):
                self.desired = None
                self.transmit(0, 0, True)
            else:
                self.transmit(*self.desired, False)

    def run(self):
        while not self.closed.wait(0.04):
            self.tick()

    def close(self):
        self.closed.set()
        with self.lock:
            self.desired = None
            self.transmit(0, 0, True)


def valid_origin(headers, host, nonce):
    return (headers.get('Host') == host and headers.get('Origin') == 'http://' + host
            and secrets.compare_digest(headers.get('X-CSRF-Token', ''), nonce))


def make_handler(telemetry, controller, nonce):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass  # Request headers and control payloads are never logged.

        def reply(self, status, body, content_type='application/json'):
            encoded = body.encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(encoded)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'")
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            self.wfile.write(encoded)

        def do_GET(self):
            host = f'127.0.0.1:{self.server.server_port}'
            if self.headers.get('Host') != host:
                self.reply(403, '{"error":"Invalid host"}')
                return
            if self.path == '/':
                html = Path(__file__).with_name('dashboard.html').read_text(encoding='utf-8')
                self.reply(200, html.replace('__CSRF__', nonce), 'text/html; charset=utf-8')
            elif self.path == '/api/telemetry':
                data = telemetry.snapshot()
                data['error'] = controller.error or controller.transport.error
                self.reply(200, json.dumps(data))
            else:
                self.reply(404, '{}')

        def do_POST(self):
            host = f'127.0.0.1:{self.server.server_port}'
            if not valid_origin(self.headers, host, nonce):
                self.reply(403, '{"error":"Invalid origin or nonce"}')
                return
            if self.path != '/api/control':
                self.reply(404, '{}')
                return
            try:
                if self.headers.get('Content-Type') != 'application/json' or self.headers.get('Transfer-Encoding') is not None:
                    raise ValueError('JSON body required')
                length = int(self.headers.get('Content-Length', '0'))
                if not 1 <= length <= 256:
                    raise ValueError('Invalid body length')
                self.connection.settimeout(0.2)
                body = json.loads(self.rfile.read(length))
                controller.request(body)
            except (ValueError, UnicodeError, OSError):
                self.reply(400, '{"error":"Command rejected: check input and fresh bench telemetry"}')
                return
            self.reply(200, '{"ok":true}')
    return Handler


def ota(firmware, token, robot):
    data = Path(firmware).read_bytes()
    if not data:
        raise ValueError('Firmware file is empty')
    connection = http.client.HTTPConnection(robot, timeout=10)
    try:
        connection.request('GET', '/status')
        response = connection.getresponse()
        if response.status != 200:
            raise ValueError('Robot status request failed')
        status = json.loads(response.read(4096))
        if type(status) is not dict:
            raise ValueError('Invalid robot status')
        boot = status.get('boot')
        if type(boot) is not int or not 0 <= boot <= 0xffffffff:
            raise ValueError('Robot status has invalid boot nonce')
        flags = status.get('flags')
        if type(flags) is not int or not flags & 32 or flags & 8:
            raise ValueError('Robot must be in the physical OTA window and not updating')
        # Firmware rechecks the window and excludes autonomous rounds at upload.
        digest = hashlib.sha256(data).hexdigest()
        connection.request('PUT', '/ota', body=data, headers={
            'Content-Type': 'application/octet-stream', 'X-Boot': str(boot),
            'X-Image-SHA256': digest, 'X-Auth': ota_auth(token, boot, len(data), digest)})
        response = connection.getresponse()
        response.read(4096)
        if response.status != 200:
            raise ValueError(f'OTA rejected (HTTP {response.status}); no retry performed')
        print('OTA accepted by robot; reconnect and verify the running image.')
    except (OSError, http.client.HTTPException) as exc:
        raise ValueError('OTA connection failed; outcome may be ambiguous. No retry performed; inspect robot status before another upload.') from exc
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=CONFIG)
    sub = parser.add_subparsers(dest='command', required=True)
    config = sub.add_parser('configure')
    config.add_argument('--robot-mac')
    config.add_argument('--receiver-mac')
    provision_cli = sub.add_parser('provision')
    provision_cli.add_argument('--serial', required=True, help='USB port for stopped robot or receiver provisioning')
    gui = sub.add_parser('gui')
    gui.add_argument('--robot', default='192.168.4.1')
    gui.add_argument('--serial', help='USB serial port; omit for UDP over BottleSumo Wi-Fi')
    gui.add_argument('--port', type=int, default=8765, help='localhost HTTP port; 0 selects an available port')
    gui.add_argument('--no-open', action='store_true')
    upload = sub.add_parser('ota')
    upload.add_argument('--firmware', type=Path, required=True)
    upload.add_argument('--robot', default='192.168.4.1')
    args = parser.parse_args()
    try:
        if args.command == 'configure':
            configure(args.config, args.robot_mac, args.receiver_mac)
            print(f'Local configuration: {args.config}')
            return
        config = load_config(args.config)
        if args.command == 'provision':
            provision(config, args.serial)
            print('Configuration stored; reset device.')
            return
        robot = socket.gethostbyname(args.robot)
        if args.command == 'ota':
            ota(args.firmware, config['ota_token'], robot)
            return
        telemetry = Telemetry()
        transport = Transport(telemetry, robot, args.serial)
        controller = Controller(telemetry, transport, config['ota_token'])
        server = ThreadingHTTPServer(('127.0.0.1', args.port), make_handler(telemetry, controller, secrets.token_hex(32)))
        server.daemon_threads = True
        threading.Thread(target=controller.run, daemon=True).start()
        url = f'http://127.0.0.1:{server.server_port}'
        print(f'Dashboard: {url} — Ctrl+C stops and closes it.')
        if not args.no_open:
            import webbrowser
            webbrowser.open(url)
        try:
            server.serve_forever(poll_interval=0.1)
        except KeyboardInterrupt:
            pass
        finally:
            controller.close()
            server.server_close()
            transport.close()
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        if args.command in ('ota', 'provision'):
            parser.exit(1, f'{exc}\n')
        parser.exit(1, 'Operation failed. Check local configuration, input, transport, and robot state. No automatic retry was performed.\n')


if __name__ == '__main__':
    main()

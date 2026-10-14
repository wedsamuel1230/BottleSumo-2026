"""Host-only protocol, control lease, config and localhost boundary checks."""
import hashlib
import hmac
import http.client
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import sys
from types import SimpleNamespace
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from unittest.mock import patch

MODULE = Path(__file__).resolve().parents[1] / 'tools/bottlesumo_debug/debugger.py'
spec = importlib.util.spec_from_file_location('bottlesumo_debug', MODULE)
d = importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)


def packet(boot=1, sequence=1, flags=22, uptime=1234):
    return d.PACKET.pack(0x42534D54, 1, 84, boot, sequence, uptime, 50,
                         10, 20, 30, 40, 100, 200, 300, 1, 2, 3,
                         -120, 120, -110, 110, 7, 8, 900, 2, 3, 4, 5, 6, 3, flags, 0)


class FakeTransport:
    def __init__(self):
        self.closed = threading.Event()
        self.sent = []
        self.error = None

    def send(self, raw):
        self.sent.append(raw)


class ProtocolTests(unittest.TestCase):
    def test_parser_fields_and_rejection(self):
        p = d.parse_packet(packet())
        self.assertEqual(d.PACKET.size, 84)
        self.assertEqual(p['qre'], [10, 20, 30, 40])
        self.assertEqual(p['distance'], [100, 200, 300])
        self.assertEqual(p['range_age'], [1, 2, 3])
        self.assertEqual(p['requested'], [-120, 120])
        self.assertEqual(p['fg'], [7, 8])
        self.assertEqual(p['control_max_us'], 900)
        self.assertEqual(p['flags'], 22)
        for raw in (packet()[:-1], b'\x00' + packet()[1:], packet()[:4] + b'\x02\x00' + packet()[6:]):
            with self.assertRaises(ValueError):
                d.parse_packet(raw)

    def test_sequence_gap_wrap_boot_and_staleness(self):
        t = d.Telemetry()
        self.assertTrue(t.ingest(packet(sequence=0xfffffffe), 1))
        self.assertTrue(t.ingest(packet(sequence=1), 1.1))
        self.assertEqual(t.snapshot(1.2)['gaps'], 2)
        self.assertFalse(t.ingest(packet(sequence=0), 1.2))
        self.assertFalse(t.ingest(packet(sequence=1), 1.2))
        self.assertEqual(t.snapshot(1.2)['out_of_order'], 2)
        self.assertEqual(t.snapshot(1.2)['packet']['sequence'], 1)
        self.assertFalse(t.snapshot(1.5)['drive_ready'])
        self.assertTrue(t.ingest(packet(boot=2, sequence=0), 1.6))
        self.assertEqual(t.snapshot(1.7)['gaps'], 0)
        self.assertEqual(len(t.snapshot(1.7)['history']), 1)
        self.assertFalse(t.ingest(b'bad', 1.7))
        self.assertEqual(t.snapshot(1.7)['invalid'], 1)

    def test_authentication_exact_bytes(self):
        header = struct.pack('<IHHIIhh', 0x4253434D, 1, 1, 5, 9, -120, 120)
        expected = header + hmac.new(b'abc', header, hashlib.sha256).digest()
        self.assertEqual(d.command_bytes('abc', 5, 9, -120, 120), expected)
        stop = d.command_bytes('abc', 5, 10, 100, 100, True)
        self.assertEqual(struct.unpack('<IHHIIhh', stop[:20])[2:], (2, 5, 10, 0, 0))
        digest = 'a' * 64
        expected_ota = hmac.new(b'abc', f'ota\n5\n123\n{digest}'.encode(), hashlib.sha256).hexdigest()
        self.assertEqual(d.ota_auth('abc', 5, 123, digest), expected_ota)
        with self.assertRaises(ValueError):
            d.command_bytes('abc', 1, 1, True, 0)


    def test_serial_fragmentation_invalid_frame_and_restart_counter_wrap(self):
        telemetry = d.Telemetry()
        transport = d.Transport.__new__(d.Transport)
        transport.telemetry = telemetry
        transport.closed = threading.Event()
        transport.error = None
        raw = b'T:' + packet().hex().encode() + b'\n'
        class Serial:
            parts = iter([raw[:20], raw[20:], b'T:bad\n'])
            def read_until(self, *args):
                data = next(self.parts)
                if data == b'T:bad\n':
                    transport.closed.set()
                return data
        transport.serial = Serial()
        transport.receive()
        self.assertEqual(telemetry.snapshot()['accepted'], 1)
        self.assertEqual(telemetry.snapshot()['invalid'], 1)
        tx = FakeTransport()
        c = d.Controller(telemetry, tx, 'abc')
        telemetry.ingest(packet(boot=2, uptime=0xffffffff))
        c.request(dict(action='drive', request_id=1, left=1, right=1))
        c.tick()
        self.assertEqual(struct.unpack('<IHHIIhh', tx.sent[-1][:20])[4], 0)


class ConfigTests(unittest.TestCase):
    def test_generation_preservation_private_no_header_and_symlink(self):
        with tempfile.TemporaryDirectory() as folder:
            config = Path(folder) / 'wireless.local.json'
            a = d.configure(config)
            b = d.configure(config, robot_mac='AA:BB:CC:DD:EE:FF')
            for key in ('wifi_password', 'ota_token', 'espnow_pmk', 'espnow_lmk'):
                self.assertEqual(a[key], b[key])
            self.assertEqual(len(a['wifi_password']), 20)
            self.assertEqual(len(a['ota_token']), 64)
            self.assertEqual(set(Path(folder).iterdir()), {config})
            self.assertFalse(hasattr(d, 'header_text'))
            if os.name != 'nt':
                self.assertEqual(stat.S_IMODE(config.stat().st_mode), 0o600)
                symlink = Path(folder) / 'link'
                symlink.symlink_to(config)
                with self.assertRaises(ValueError):
                    d.configure(symlink)
                with self.assertRaises(ValueError):
                    d.atomic_private(symlink, 'overwrite')
            self.assertEqual(d.load_config(config), b)

    def test_validation_and_binary_string_roundtrip(self):
        example = json.loads(MODULE.with_name('wireless.example.json').read_text())
        d.validate_config(example, False)
        with self.assertRaises(ValueError):
            d.validate_config(example)
        with tempfile.TemporaryDirectory() as folder:
            good = d.configure(Path(folder) / 'c.json')
        for key, value in (('wifi_password', 'bad\npassword'), ('wifi_channel', True), ('wifi_channel', 12),
                           ('ota_token', 'nothex'), ('robot_mac', 'invalid'), ('espnow_lmk', 42)):
            bad = dict(good, **{key: value})
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                d.validate_config(bad)
        quoted = dict(good, wifi_password='abc"def\\ghi')
        packed = d.NETWORK_SETTINGS.unpack(d.network_settings_bytes(quoted))
        self.assertEqual(packed[3].rstrip(b'\x00').decode(), quoted['wifi_password'])
        with self.assertRaises(ValueError):
            d.validate_config(dict(good, extra='x'))


    def test_legacy_sta_migration_preserves_secrets_and_utf8(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'config.json'
            original = d.configure(path)
            old = {key: value for key, value in original.items() if key not in ('sta_ssid', 'sta_password')}
            path.write_text(json.dumps(old))
            loaded = d.load_config(path)
            self.assertEqual(loaded['sta_ssid'], '')
            self.assertEqual(loaded['sta_password'], '')
            migrated = d.configure(path)
            self.assertEqual({key: migrated[key] for key in old}, old)
            self.assertEqual(len(json.loads(path.read_text())), 9)
            configured = dict(migrated, sta_ssid='網絡"\\😀', sta_password='quote"slash\\123')
            path.write_text(json.dumps(configured))
            self.assertEqual(d.configure(path), configured)
            packed = d.NETWORK_SETTINGS.unpack(d.network_settings_bytes(configured))
            self.assertEqual(packed[5].rstrip(b'\x00').decode('utf-8'), configured['sta_ssid'])
            self.assertEqual(packed[6].rstrip(b'\x00').decode('ascii'), configured['sta_password'])

    def test_sta_validation_utf8_limit_controls_and_password(self):
        with tempfile.TemporaryDirectory() as folder:
            good = d.configure(Path(folder) / 'config.json')
        d.validate_config(dict(good, sta_ssid='界' * 10, sta_password='12345678'))
        for ssid, password in (('界' * 11, '12345678'), ('x' * 33, '12345678'),
                               ('bad\nssid', '12345678'), ('bad\x00ssid', '12345678'),
                               ('bad\x85ssid', '12345678'), ('\ud800', '12345678'),
                               ('router', ''), ('', '12345678'), ('router', 'short'),
                               ('router', 'x' * 64), ('router', '密碼12345678'),
                               ('router', 'bad\npassword'), (None, ''), ('router', None)):
            with self.subTest(ssid=repr(ssid), password=repr(password)), self.assertRaises(ValueError):
                d.validate_config(dict(good, sta_ssid=ssid, sta_password=password))



class ProvisionTests(unittest.TestCase):
    def setUp(self):
        with tempfile.TemporaryDirectory() as folder:
            self.config = d.configure(Path(folder) / 'config.json', robot_mac='01:02:03:04:05:06',
                                      receiver_mac='11:12:13:14:15:16')

    def test_network_settings_exact_size_header_padding_and_fields(self):
        raw = d.network_settings_bytes(self.config)
        self.assertEqual(len(raw), 279)
        packed = d.NETWORK_SETTINGS.unpack(raw)
        self.assertEqual(packed[:3], (0x42534e43, 1, 279))
        self.assertEqual(packed[3].rstrip(b'\x00').decode(), self.config['wifi_password'])
        self.assertEqual(packed[4].rstrip(b'\x00').decode(), self.config['ota_token'])
        self.assertEqual(packed[3][-1], 0)
        self.assertEqual(packed[4][-1], 0)
        self.assertEqual(packed[5], b'\x00' * 33)
        self.assertEqual(packed[6], b'\x00' * 64)
        self.assertEqual(packed[7], bytes([1, 2, 3, 4, 5, 6]))
        self.assertEqual(packed[8], bytes([17, 18, 19, 20, 21, 22]))
        self.assertEqual(packed[9], bytes.fromhex(self.config['espnow_pmk']))
        self.assertEqual(packed[10], bytes.fromhex(self.config['espnow_lmk']))
        self.assertEqual(packed[11], 6)

    def mock_serial(self, lines):
        class Connection:
            written = []
            closed = False
            def __init__(self, port, baudrate, timeout, write_timeout):
                self.asserted = (port, baudrate, timeout, write_timeout)
                self.lines = iter(lines)
            def open(self):
                if self.dtr is not False or self.rts is not False:
                    raise AssertionError('Unexpected modem reset')
            def write(self, data):
                self.written.append(data)
                return len(data)
            def read_until(self, *args):
                return next(self.lines)
            def close(self):
                Connection.closed = True
        return Connection

    def test_serial_success_fragmented_ack_no_echo_or_retry(self):
        connection = self.mock_serial([b'boot log\n', b'NET:', b'OK\r\n'])
        with patch.dict(sys.modules, serial=SimpleNamespace(Serial=connection)), patch('builtins.print') as printing:
            d.provision(self.config, 'test-port')
            printing.assert_not_called()
        self.assertEqual(connection.written, [b'NET:' + d.network_settings_bytes(self.config).hex().encode() + b'\n'])
        self.assertTrue(connection.closed)

    def test_serial_reject_and_ambiguous_timeout_do_not_retry(self):
        for lines, clocks, expected in (([b'NET:REJECT\n'], [0, 1], 'rejected'),
                                        ([], [0, 16], 'ambiguous')):
            connection = self.mock_serial(lines)
            with self.subTest(expected=expected), patch.dict(sys.modules, serial=SimpleNamespace(Serial=connection)), \
                    patch.object(d.time, 'monotonic', side_effect=clocks), self.assertRaisesRegex(ValueError, expected):
                d.provision(self.config, 'test-port')
            self.assertEqual(len(connection.written), 1)
            self.assertTrue(connection.closed)


class ControlTests(unittest.TestCase):
    def setUp(self):
        self.t = d.Telemetry()
        self.tx = FakeTransport()
        self.c = d.Controller(self.t, self.tx, 'abc')
        self.t.ingest(packet(), 10)

    def drive(self, request_id=1):
        return dict(action='drive', request_id=request_id, left=90, right=-40)

    def test_lease_stale_gate_and_stop_order(self):
        self.c.request(self.drive(), 10)
        self.c.tick(10.04)
        self.assertEqual(struct.unpack('<IHHIIhh', self.tx.sent[-1][:20])[-2:], (90, -40))
        self.c.tick(10.11)
        self.assertEqual(struct.unpack('<IHHIIhh', self.tx.sent[-1][:20])[2], 2)
        self.assertIsNone(self.c.desired)
        self.c.request(dict(action='stop', request_id=4), 10.12)
        with self.assertRaises(ValueError):
            self.c.request(self.drive(3), 10.13)
        with self.assertRaises(ValueError):
            self.c.request(self.drive(5), 10.4)

    def test_invalid_fields_flags_boot_and_counter_resume(self):
        for body in (dict(action='drive', request_id=1, left=True, right=1), dict(action='drive', request_id=1, left=121, right=0),
                     dict(action='drive', request_id=1, left=0, right=0, extra=1), dict(action='unknown', request_id=1),
                     dict(action='stop'), dict(action='stop', request_id=True)):
            with self.subTest(body=body), self.assertRaises(ValueError):
                self.c.request(body, 10)
        for flags in (0, 2, 4, 16, 22 | 1, 22 | 8):
            self.t.ingest(packet(boot=flags + 10, flags=flags), 10)
            with self.subTest(flags=flags), self.assertRaises(ValueError):
                self.c.request(self.drive(flags + 100), 10)
        self.t.ingest(packet(boot=100, uptime=80000), 10)
        self.c.request(self.drive(1000), 10)
        self.c.tick(10.04)
        self.assertEqual(struct.unpack('<IHHIIhh', self.tx.sent[-1][:20])[4], 80001)
        self.t.ingest(packet(boot=100, sequence=2, uptime=80050), 10.05)
        self.c.tick(10.06)
        self.assertEqual(struct.unpack('<IHHIIhh', self.tx.sent[-1][:20])[4], 80050)
        self.t.ingest(packet(boot=101), 10.07)
        self.c.tick(10.08)
        self.assertIsNone(self.c.desired)
        self.assertEqual(struct.unpack('<IHHIIhh', self.tx.sent[-1][:20])[2], 2)


class HTTPTests(unittest.TestCase):
    def test_local_endpoint_origin_host_nonce_and_payload(self):
        t, tx = d.Telemetry(), FakeTransport()
        c = d.Controller(t, tx, 'abc')
        server = ThreadingHTTPServer(('127.0.0.1', 0), d.make_handler(t, c, 'nonce'))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        host = f'127.0.0.1:{server.server_port}'
        good = {'Host': host, 'Origin': 'http://' + host, 'X-CSRF-Token': 'nonce', 'Content-Type': 'application/json'}
        try:
            for changes in ({'Host': 'evil.example'}, {'Origin': 'https://evil.example'}, {'X-CSRF-Token': 'bad'}):
                conn = http.client.HTTPConnection('127.0.0.1', server.server_port)
                conn.request('POST', '/api/control', body='{"action":"stop","request_id":1}', headers=dict(good, **changes))
                response = conn.getresponse()
                self.assertEqual(response.status, 403)
                response.read()
                conn.close()
            conn = http.client.HTTPConnection('127.0.0.1', server.server_port)
            conn.request('POST', '/api/control', body='{"action":"drive","request_id":2,"left":1,"right":1}', headers=good)
            response = conn.getresponse()
            self.assertEqual(response.status, 400)  # No fresh telemetry.
            response.read()
            conn.close()
            t.ingest(packet())
            conn = http.client.HTTPConnection('127.0.0.1', server.server_port)
            conn.request('POST', '/api/control', body='{"action":"drive","request_id":3,"left":1,"right":1}', headers=good)
            response = conn.getresponse()
            self.assertEqual(response.status, 200)
            response.read()
            conn.close()
            self.assertEqual(c.desired, (1, 1))
            conn = http.client.HTTPConnection('127.0.0.1', server.server_port)
            conn.request('GET', '/', headers={'Host': host})
            response = conn.getresponse()
            self.assertEqual(response.status, 200)
            html = response.read().decode()
            self.assertIn("csrf='nonce'", html)
            self.assertNotIn('__CSRF__', html)
            conn.close()
            conn = http.client.HTTPConnection('127.0.0.1', server.server_port)
            conn.request('GET', '/api/telemetry', headers={'Host': 'evil.example'})
            response = conn.getresponse()
            self.assertEqual(response.status, 403)
            response.read()
            conn.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


class OTATests(unittest.TestCase):
    def test_ota_auth_headers_and_no_retry_after_ambiguous_send(self):
        class Response:
            status = 200
            def read(self, size):
                return b'{"boot":42,"flags":32}'
        class Connection:
            calls = []
            def __init__(self, *args, **kwargs):
                pass
            def request(self, method, path, **kwargs):
                self.calls.append((method, path, kwargs))
                if method == 'PUT':
                    raise OSError('disconnected')
            def getresponse(self):
                return Response()
            def close(self):
                pass
        with tempfile.TemporaryDirectory() as folder:
            image = Path(folder) / 'firmware.bin'
            image.write_bytes(b'firmware')
            with patch.object(d.http.client, 'HTTPConnection', Connection), self.assertRaisesRegex(ValueError, 'ambiguous'):
                d.ota(image, 'abc', '192.168.4.1')
        self.assertEqual([c[:2] for c in Connection.calls], [('GET', '/status'), ('PUT', '/ota')])
        headers = Connection.calls[1][2]['headers']
        digest = hashlib.sha256(b'firmware').hexdigest()
        self.assertEqual(headers['X-Auth'], d.ota_auth('abc', 42, 8, digest))
        self.assertEqual(headers['X-Boot'], '42')


if __name__ == '__main__':
    unittest.main()

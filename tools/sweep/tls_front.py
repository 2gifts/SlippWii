"""TLS in front of the test matchmaking server, for a console that queues the way Slippi Dolphin
does after 3.6.4 (a WebSocket over TLS: Nintendont/kernel/slippi_online/so_mm_ws.c).

    python tools/sweep/tls_front.py --dir RUN_DIR --listen 8443 --backend 127.0.0.1:8080
                                    [--host NAME] [--chain rsa|ec] [--ciphers OPENSSL_LIST] [--make-only]

It takes each TLS connection, opens a plain one to the backend (so_pc_lobby_mm.exe --ws-port) and
passes the bytes both ways. The backend's first line is a PROXY protocol line with the client's
address, as a service behind a load balancer gets it. Python does the listening because Windows
Firewall lets its traffic in and not a new program's (as tools/sweep/udp_front.py for UDP).

The certificates are made here, for a root of the run's own: "CN=SlippWii Test Root", an EC P-256
key, which a test build of the kernel trusts when its settings carry the key (test_tls_root=, the
130 hex digits this prints and writes to RUN_DIR/tls/root.hex). No real authority is involved and
nothing leaves this PC. A folder that already has a root keeps it, so a build made for it earlier
still matches.

  --chain rsa (default): the shape of Slippi's own hosts today (Google Trust Services): an RSA 2048
              certificate from an RSA 2048 authority, itself signed by an RSA 4096 key; that key's
              certificate is signed by the test root (one signature more than the real chain has).
  --chain ec: an EC P-256 certificate from an EC P-384 authority.
  --ciphers:  OpenSSL's names, to make the server pick one suite (e.g. ECDHE-RSA-CHACHA20-POLY1305).
TLS 1.2 only by default, as the kernel speaks; --tls13 lets the server offer 1.3 too (a client that
speaks 1.2 only must still get in).
"""
import argparse
import datetime
import select
import socket
import ssl
import sys
import threading
import time
from pathlib import Path


def log(text):
    print(f'{time.strftime("%H:%M:%S")} tls: {text}', flush=True)


def make_chain(folder, host, kind='rsa'):
    """Writes root.pem, chain.pem, leaf.key and root.hex into `folder`; returns the root key's point in hex."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, rsa
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    pem, der = serialization.Encoding.PEM, serialization.Encoding.DER
    now = datetime.datetime.now(datetime.timezone.utc)

    def name(cn):
        return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])

    def save_key(path, key):
        path.write_bytes(key.private_bytes(pem, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))

    def cert(subject, key, issuer, issuer_key, ca, digest=hashes.SHA256(), names=()):
        b = (x509.CertificateBuilder().subject_name(name(subject)).issuer_name(name(issuer))
             .public_key(key.public_key()).serial_number(x509.random_serial_number())
             .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=60))
             .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
        if ca:
            b = b.add_extension(x509.KeyUsage(digital_signature=True, key_cert_sign=True, crl_sign=True, content_commitment=False,
                                              key_encipherment=False, data_encipherment=False, key_agreement=False,
                                              encipher_only=False, decipher_only=False), critical=True)
        else:
            b = b.add_extension(x509.SubjectAlternativeName([x509.DNSName(n) for n in names]), critical=False)
            b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        return b.sign(issuer_key, digest)

    root_key_path = folder / 'root.key'
    if root_key_path.exists():
        root_key = serialization.load_pem_private_key(root_key_path.read_bytes(), None)
    else:
        root_key = ec.generate_private_key(ec.SECP256R1())
        save_key(root_key_path, root_key)
    root = cert('SlippWii Test Root', root_key, 'SlippWii Test Root', root_key, True)
    # (the kernel holds this name's bytes: so_tls.c, test_root_dn)
    want = bytes([0x30, 0x1D, 0x31, 0x1B, 0x30, 0x19, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0C, 0x12]) + b'SlippWii Test Root'
    if root.subject.public_bytes() != want:
        sys.exit("the test root's name is not encoded as the kernel expects")
    (folder / 'root.pem').write_bytes(root.public_bytes(pem))

    if kind == 'rsa':
        top_key = rsa.generate_private_key(65537, 4096)
        top = cert('SlippWii Test R1', top_key, 'SlippWii Test Root', root_key, True)
        mid_key = rsa.generate_private_key(65537, 2048)
        mid = cert('SlippWii Test WR3', mid_key, 'SlippWii Test R1', top_key, True)
        leaf_key = rsa.generate_private_key(65537, 2048)
        leaf = cert(host, leaf_key, 'SlippWii Test WR3', mid_key, False, names=[host])
        chain = [leaf, mid, top]
    elif kind == 'ec':
        mid_key = ec.generate_private_key(ec.SECP384R1())
        mid = cert('SlippWii Test E1', mid_key, 'SlippWii Test Root', root_key, True)
        leaf_key = ec.generate_private_key(ec.SECP256R1())
        leaf = cert(host, leaf_key, 'SlippWii Test E1', mid_key, False, digest=hashes.SHA384(), names=[host])
        chain = [leaf, mid]
    else:
        sys.exit(f'--chain {kind}: rsa or ec')
    save_key(folder / 'leaf.key', leaf_key)
    (folder / 'chain.pem').write_bytes(b''.join(c.public_bytes(pem) for c in chain))
    point = root_key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint).hex()
    (folder / 'root.hex').write_text(point + '\n')
    sizes = ', '.join(str(len(c.public_bytes(der))) for c in chain)
    log(f'certificates for {host} ({kind} chain of {len(chain)}: {sizes} bytes) under the test root in {folder}')
    return point


def relay(tls, backend, who):
    """Bytes both ways until either side closes."""
    backend.setblocking(True)
    try:
        while True:
            readable, _, _ = select.select([tls, backend], [], [], 1.0)
            if tls in readable or tls.pending():
                data = tls.recv(16384)
                if not data:
                    break
                backend.sendall(data)
            if backend in readable:
                data = backend.recv(16384)
                if not data:
                    break
                tls.sendall(data)
    except (OSError, ssl.SSLError) as e:
        log(f'{who}: {type(e).__name__}: {e}')
    finally:
        for s in (tls, backend):
            try:
                s.close()
            except OSError:
                pass
        log(f'{who}: closed')


def handle(raw, addr, ctx, backend_addr, listen_port):
    who = f'{addr[0]}:{addr[1]}'
    start = time.monotonic()
    try:
        raw.settimeout(20)
        tls = ctx.wrap_socket(raw, server_side=True)
    except (OSError, ssl.SSLError) as e:
        log(f'{who}: the handshake failed after {int((time.monotonic() - start) * 1000)} ms: {type(e).__name__}: {e}')
        raw.close()
        return
    took = int((time.monotonic() - start) * 1000)
    log(f'{who}: handshake done in {took} ms: {tls.version()}, {tls.cipher()[0]}')
    tls.settimeout(None)
    try:
        backend = socket.create_connection(backend_addr, timeout=5)
    except OSError as e:
        log(f'{who}: no backend at {backend_addr[0]}:{backend_addr[1]} ({e})')
        tls.close()
        return
    backend.sendall(f'PROXY TCP4 {addr[0]} {backend_addr[0]} {addr[1]} {listen_port}\r\n'.encode())
    relay(tls, backend, who)


def serve(folder, listen, backend_addr, ciphers=None, tls13=False, stop=None):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    if not tls13:
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    if ciphers:
        ctx.set_ciphers(ciphers)
    ctx.load_cert_chain(str(Path(folder) / 'chain.pem'), str(Path(folder) / 'leaf.key'))
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', listen))
    srv.listen(8)
    srv.settimeout(0.5)
    log(f'listening on TCP {listen} for {backend_addr[0]}:{backend_addr[1]}'
        + (f', ciphers {ciphers}' if ciphers else '') + (', TLS 1.2 and 1.3' if tls13 else ', TLS 1.2'))
    while not (stop and stop.is_set()):
        try:
            raw, addr = srv.accept()
        except socket.timeout:
            continue
        threading.Thread(target=handle, args=(raw, addr, ctx, backend_addr, listen), daemon=True).start()
    srv.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dir', required=True, help='the run folder (certificates go to DIR/tls)')
    ap.add_argument('--listen', type=int, default=8443)
    ap.add_argument('--backend', default='127.0.0.1:8080')
    ap.add_argument('--host', default='matchmaking.slippwii.test', help='the name the certificate is for')
    ap.add_argument('--chain', default='rsa', choices=['rsa', 'ec'])
    ap.add_argument('--ciphers')
    ap.add_argument('--tls13', action='store_true')
    ap.add_argument('--make-only', action='store_true', help='write the certificates and stop')
    args = ap.parse_args()
    folder = Path(args.dir) / 'tls'
    point = make_chain(folder, args.host, args.chain)
    print(f'test_tls_root={point}', flush=True)
    if args.make_only:
        return
    host, port = args.backend.rsplit(':', 1)
    serve(folder, args.listen, (host, int(port)), args.ciphers, args.tls13)


if __name__ == '__main__':
    main()

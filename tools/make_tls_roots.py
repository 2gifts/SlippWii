"""Writes the kernel's list of trusted root certificates (Nintendont/kernel/slippi_online/so_tls_roots.inc).

    python tools/make_tls_roots.py            # write the file
    python tools/make_tls_roots.py --check    # fail if the file is not what this would write

The kernel checks the certificate chain of Slippi's matchmaking service (so_tls.c). It cannot carry
every authority a PC trusts, so it carries the ones Slippi's hosts use today (Google Trust
Services: matchmaking.slippi.gg's certificates come from its "WR3", under GTS Root R1) and the
likeliest to follow (Google's other roots, Let's Encrypt's). If Slippi moves to an authority
that is not here, matchmaking fails with "certificate not trusted" until a SlippWii update adds it
(the loader's updater does not depend on this list).

The certificates are taken from the `certifi` package (Mozilla's list) and each is checked against
the SHA-256 fingerprint written here, so a different bundle cannot change what is trusted.
"""
import argparse
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'Nintendont/kernel/slippi_online/so_tls_roots.inc'
# name in the certificate, SHA-256 of the certificate
ROOTS = [
    ('GTS Root R1', 'd947432abde7b7fa90fc2e6b59101b1280e0e1c7e4e40fa3c6887fff57a7f4cf'),
    ('GTS Root R2', '8d25cd97229dbf70356bda4eb3cc734031e24cf00fafcfd32dc76eb5841c7ea8'),
    ('GTS Root R3', '34d8a73ee208d9bcdb0d956520934b4e40e69482596e8b6f73c8426b010a6f48'),
    ('GTS Root R4', '349dfa4058c5e263123b398ae795573c4e1313c83fe68f93556cd5e8031b3c7d'),
    ('ISRG Root X1', '96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6'),
    ('ISRG Root X2', '69729b8e15a86efc177a57afb7171dfc64add28c2fca8cf1507e34453ccb1470'),
]
CURVES = {'secp256r1': 'BR_EC_secp256r1', 'secp384r1': 'BR_EC_secp384r1', 'secp521r1': 'BR_EC_secp521r1'}


def array(name, data):
    rows = [', '.join(f'0x{b:02X}' for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    return f'static const unsigned char {name}[] = {{\n\t' + ',\n\t'.join(rows) + ',\n};\n'


def render(certs):
    from cryptography.hazmat.primitives.asymmetric import ec, rsa
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
    out = ['/* Written by tools/make_tls_roots.py: do not edit. The roots the kernel trusts (so_tls.c):\n']
    out += [f' *   {name} ({fp[:16]}...)\n' for (name, fp), _ in certs]
    out.append(' */\n')
    entries = []
    for i, ((name, _), cert) in enumerate(certs):
        key = cert.public_key()
        out.append(f'/* {name} */\n')
        out.append(array(f'so_root{i}_dn', cert.subject.public_bytes()))
        if isinstance(key, rsa.RSAPublicKey):
            nums = key.public_numbers()
            out.append(array(f'so_root{i}_n', nums.n.to_bytes((nums.n.bit_length() + 7) // 8, 'big')))
            out.append(array(f'so_root{i}_e', nums.e.to_bytes((nums.e.bit_length() + 7) // 8, 'big')))
            entries.append(f'\t{{ {{ (unsigned char *)so_root{i}_dn, sizeof(so_root{i}_dn) }}, BR_X509_TA_CA,\n'
                           f'\t\t{{ BR_KEYTYPE_RSA, {{ .rsa = {{ (unsigned char *)so_root{i}_n, sizeof(so_root{i}_n),\n'
                           f'\t\t\t(unsigned char *)so_root{i}_e, sizeof(so_root{i}_e) }} }} }} }},\n')
        elif isinstance(key, ec.EllipticCurvePublicKey):
            out.append(array(f'so_root{i}_q', key.public_bytes(Encoding.X962, PublicFormat.UncompressedPoint)))
            entries.append(f'\t{{ {{ (unsigned char *)so_root{i}_dn, sizeof(so_root{i}_dn) }}, BR_X509_TA_CA,\n'
                           f'\t\t{{ BR_KEYTYPE_EC, {{ .ec = {{ {CURVES[key.curve.name]}, (unsigned char *)so_root{i}_q,\n'
                           f'\t\t\tsizeof(so_root{i}_q) }} }} }} }},\n')
        else:
            sys.exit(f'{name}: a key of a kind the kernel cannot check')
    out.append('static const br_x509_trust_anchor so_tls_roots[] = {\n' + ''.join(entries) + '};\n')
    return ''.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', action='store_true', help='compare with the file instead of writing it')
    args = ap.parse_args()
    import certifi
    from cryptography import x509
    from cryptography.hazmat.primitives.serialization import Encoding
    by_print = {}
    for cert in x509.load_pem_x509_certificates(Path(certifi.where()).read_bytes()):
        by_print[hashlib.sha256(cert.public_bytes(Encoding.DER)).hexdigest()] = cert
    certs = []
    for name, fp in ROOTS:
        cert = by_print.get(fp)
        if cert is None:
            sys.exit(f'{name}: no certificate with the fingerprint {fp} in {certifi.where()}')
        if f'CN={name}' not in cert.subject.rfc4514_string() or cert.subject != cert.issuer:
            sys.exit(f'{name}: the certificate with that fingerprint is {cert.subject.rfc4514_string()}')
        certs.append(((name, fp), cert))
    text = render(certs)
    if args.check:
        if not OUT.exists() or OUT.read_text(encoding='utf-8') != text:
            sys.exit(f'{OUT} is not what this tool writes: run it without --check')
        print(f'{OUT.name}: as written by this tool ({len(certs)} roots)')
        return
    OUT.write_text(text, encoding='utf-8', newline='\n')
    print(f'{OUT}: {len(certs)} roots')


if __name__ == '__main__':
    main()

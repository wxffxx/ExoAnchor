"""Verified TLS with optional physical pairing to one device certificate."""
from __future__ import annotations

import functools
import hashlib
import hmac
from http.client import HTTPSConnection
from pathlib import Path
import ssl
from urllib.request import HTTPSHandler


class _PairedConnection(HTTPSConnection):
    def __init__(self, *args, paired_digest: bytes, **kwargs):
        super().__init__(*args, **kwargs)
        self._paired_digest = paired_digest

    def connect(self):
        super().connect()
        digest = hashlib.sha256(self.sock.getpeercert(binary_form=True)).digest()
        if not hmac.compare_digest(digest, self._paired_digest):
            self.close()
            raise ssl.SSLCertVerificationError("device certificate does not match physical pairing")


def verified_tls_context(certificate_file: str | None = None) -> tuple[ssl.SSLContext, bytes | None]:
    """Return chain verification plus an optional physically paired leaf digest."""
    if not certificate_file:
        return ssl.create_default_context(), None
    pem = Path(certificate_file).expanduser().read_text(encoding="ascii")
    if pem.count("-----BEGIN CERTIFICATE-----") != 1:
        raise ValueError("pairing file must contain exactly one device certificate")
    digest = hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem)).digest()
    context = ssl.create_default_context(cafile=str(Path(certificate_file).expanduser()))
    # The leaf digest identifies this device when DHCP changes its IP. Chain,
    # signature and validity checks remain enabled. Check the pin before send.
    context.check_hostname = False
    return context, digest


def verified_https_handler(certificate_file: str | None = None) -> HTTPSHandler:
    context, digest = verified_tls_context(certificate_file)
    if digest is None:
        return HTTPSHandler(context=context)

    class PairedHTTPSHandler(HTTPSHandler):
        def https_open(self, request):
            return self.do_open(functools.partial(_PairedConnection, paired_digest=digest),
                                request, context=context, check_hostname=False)

    return PairedHTTPSHandler(context=context)

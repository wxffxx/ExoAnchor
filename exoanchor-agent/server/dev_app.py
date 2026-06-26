"""
ASGI entrypoint for the local ExoAnchor development server.
"""

from .test_app import create_test_app


app = create_test_app()

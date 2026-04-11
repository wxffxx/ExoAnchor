"""
ExoAnchor Test Server — Mac 本地测试用

使用 Mock HID/Video/GPIO + 真实 SSH 连接到 192.168.1.67 (WSL)

启动: python test_server.py
访问: http://localhost:8090/docs (Swagger UI)
"""

import logging

from exoanchor.server import create_test_app


logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
    datefmt="%H:%M:%S",
)

app = create_test_app()


if __name__ == "__main__":
    import uvicorn

    uvicorn.run("test_server:app", host="0.0.0.0", port=8090, reload=True)

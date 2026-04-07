

import json
import socket
from typing import Optional, Tuple

from modules.bridget_logger import BridgeLogger


class UdpMessageSender:
    """Send JSON messages back to the last known UDP peer."""

    def __init__(self, logger: BridgeLogger) -> None:
        self.logger = logger
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send_json(self, payload: dict, addr: Optional[Tuple[str, int]]) -> None:
        """Send a JSON payload over UDP if a remote address is available."""
        if not addr:
            self.logger.log("Cannot send UDP JSON: remote address is unknown", "WARN")
            return

        try:
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.sock.sendto(data, addr)
            self.logger.log(
                f"UDP JSON sent to {addr}: {data.decode('utf-8', errors='ignore')}",
                "UDP",
            )
        except Exception as exc:
            self.logger.log(f"Failed to send UDP JSON: {exc}", "ERROR")

    def close(self) -> None:
        """Close the UDP sender socket."""
        try:
            self.sock.close()
        except Exception:
            pass
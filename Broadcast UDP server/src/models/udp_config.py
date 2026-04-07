from dataclasses import dataclass


@dataclass(frozen=True)
class UdpConfig:
    """Configuration values related to UDP audio transport."""

    server_port: int = 5000
    buffer_size: int = 4096
    sample_rate: int = 16000
    samples_per_packet: int = 160
    metadata_size: int = 4
    jitter_ms: int = 250
    max_buffer: int = 800
    max_missing_before_loss: int = 5

    @property
    def packet_audio_size(self) -> int:
        """Return the PCM payload size in bytes for each UDP packet."""
        return self.samples_per_packet * 2  # 16-bit mono PCM

    @property
    def packet_duration_ms(self) -> float:
        """Return the duration of each UDP packet in milliseconds."""
        return (self.samples_per_packet / self.sample_rate) * 1000

    @property
    def jitter_size(self) -> int:
        """Return the number of packets required for the jitter buffer."""
        return max(10, int(self.jitter_ms / self.packet_duration_ms))


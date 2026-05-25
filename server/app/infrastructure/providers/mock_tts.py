import io
import wave

from app.ports.tts import TextToSpeechPort


class MockTextToSpeechProvider(TextToSpeechPort):
    async def synthesize(self, text: str) -> bytes:
        sample_rate = 16000
        duration_seconds = 1
        frame_count = sample_rate * duration_seconds

        buffer = io.BytesIO()
        with wave.open(buffer, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(sample_rate)
            wav.writeframes(b"\x00\x00" * frame_count)
        return buffer.getvalue()


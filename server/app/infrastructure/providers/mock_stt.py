from app.ports.stt import SpeechToTextPort


class MockSpeechToTextProvider(SpeechToTextPort):
    async def transcribe(self, audio_bytes: bytes, filename: str, content_type: str) -> str:
        size = len(audio_bytes)
        return f"Ban vua gui audio {filename}, dung luong {size} bytes."


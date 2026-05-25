from uuid import uuid4

from app.ports.audio_storage import AudioStoragePort
from app.ports.llm import LlmPort
from app.ports.stt import SpeechToTextPort
from app.ports.tts import TextToSpeechPort


class ChatService:
    def __init__(
        self,
        llm: LlmPort,
        stt: SpeechToTextPort,
        tts: TextToSpeechPort,
        audio_storage: AudioStoragePort,
    ) -> None:
        self._llm = llm
        self._stt = stt
        self._tts = tts
        self._audio_storage = audio_storage

    async def chat_text(self, message: str, session_id: str | None = None) -> dict:
        session_id = session_id or str(uuid4())
        reply_text = await self._llm.generate_reply(message=message, session_id=session_id)
        audio_url = await self._synthesize_reply(reply_text)

        return {
            "session_id": session_id,
            "input_text": message,
            "reply_text": reply_text,
            "audio_url": audio_url,
        }

    async def chat_voice(
        self,
        audio_bytes: bytes,
        filename: str,
        content_type: str,
        session_id: str | None = None,
    ) -> dict:
        session_id = session_id or str(uuid4())
        transcript = await self._stt.transcribe(
            audio_bytes=audio_bytes,
            filename=filename,
            content_type=content_type,
        )
        reply_text = await self._llm.generate_reply(message=transcript, session_id=session_id)
        audio_url = await self._synthesize_reply(reply_text)

        return {
            "session_id": session_id,
            "input_text": transcript,
            "transcript": transcript,
            "reply_text": reply_text,
            "audio_url": audio_url,
        }

    async def _synthesize_reply(self, text: str) -> str | None:
        audio = await self._tts.synthesize(text)
        if audio is None:
            return None
        filename = self._audio_storage.save_wav(audio)
        return self._audio_storage.public_url(filename)


from __future__ import annotations

from typing import Optional
from uuid import uuid4

from app.ports.chat_store import ChatStorePort
from app.ports.llm import LlmPort
from app.ports.stt import SpeechToTextPort
from app.ports.tts import TextToSpeechPort


class ChatService:
    def __init__(
        self,
        llm: LlmPort,
        stt: SpeechToTextPort,
        tts: TextToSpeechPort,
        chat_store: ChatStorePort,
    ) -> None:
        self._llm = llm
        self._stt = stt
        self._tts = tts
        self._chat_store = chat_store

    async def chat_text(self, message: str, session_id: Optional[str] = None) -> dict:
        session_id = session_id or str(uuid4())
        history = await self._chat_store.get_messages(session_id)
        reply_text = await self._llm.generate_reply(message=message, session_id=session_id, history=history)
        await self._chat_store.append_message(session_id, "user", message)
        await self._chat_store.append_message(session_id, "assistant", reply_text)
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
        session_id: Optional[str] = None,
    ) -> dict:
        session_id = session_id or str(uuid4())
        transcript = await self._stt.transcribe(
            audio_bytes=audio_bytes,
            filename=filename,
            content_type=content_type,
        )
        history = await self._chat_store.get_messages(session_id)
        reply_text = await self._llm.generate_reply(message=transcript, session_id=session_id, history=history)
        await self._chat_store.append_message(session_id, "user", transcript)
        await self._chat_store.append_message(session_id, "assistant", reply_text)
        audio_url = await self._synthesize_reply(reply_text)

        return {
            "session_id": session_id,
            "input_text": transcript,
            "transcript": transcript,
            "reply_text": reply_text,
            "audio_url": audio_url,
        }

    async def _synthesize_reply(self, text: str) -> Optional[str]:
        return None

    async def get_history(self, session_id: str, limit: int = 50) -> list[dict]:
        return await self._chat_store.get_messages(session_id, limit=limit)

    async def get_sessions(self, limit: int = 50) -> list[dict]:
        return await self._chat_store.get_sessions(limit=limit)


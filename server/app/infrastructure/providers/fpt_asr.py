from typing import Any

import httpx

from app.ports.stt import SpeechToTextPort


class FptAsrProvider(SpeechToTextPort):
    def __init__(self, api_key: str, url: str) -> None:
        if not api_key:
            raise ValueError("FPT_ASR_API_KEY is required")
        self._api_key = api_key
        self._url = url

    async def transcribe(self, audio_bytes: bytes, filename: str, content_type: str) -> str:
        result = await self.transcribe_raw(audio_bytes, filename, content_type)
        transcript = extract_transcript(result)
        if not transcript:
            raise ValueError(f"FPT ASR response did not include transcript: {result}")
        return transcript

    async def transcribe_raw(self, audio_bytes: bytes, filename: str, content_type: str) -> dict[str, Any]:
        headers = {
            "api-key": self._api_key,
            "Content-Type": content_type or "application/octet-stream",
        }
        async with httpx.AsyncClient(timeout=60.0) as client:
            response = await client.post(self._url, headers=headers, content=audio_bytes)

        response.raise_for_status()
        return response.json()


def extract_transcript(payload: dict[str, Any]) -> str:
    direct_keys = ("text", "transcript", "utterance", "result")
    for key in direct_keys:
        value = payload.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()

    hypotheses = payload.get("hypotheses")
    if isinstance(hypotheses, list):
        for item in hypotheses:
            if not isinstance(item, dict):
                continue
            for key in direct_keys:
                value = item.get(key)
                if isinstance(value, str) and value.strip():
                    return value.strip()

    data = payload.get("data")
    if isinstance(data, dict):
        return extract_transcript(data)

    return ""

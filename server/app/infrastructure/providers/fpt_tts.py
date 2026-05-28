from __future__ import annotations

import asyncio
import logging
from typing import Any, Optional

import httpx

from app.ports.tts import TextToSpeechPort

logger = logging.getLogger(__name__)


class FptTtsProvider(TextToSpeechPort):
    def __init__(
        self,
        api_key: str,
        url: str,
        voice: str,
        speed: str,
        poll_attempts: int = 45,
        poll_interval_seconds: float = 1.0,
    ) -> None:
        if not api_key:
            raise ValueError("FPT_TTS_API_KEY is required")
        self._api_key = api_key
        self._url = url
        self._voice = voice
        self._speed = speed
        self._poll_attempts = poll_attempts
        self._poll_interval_seconds = poll_interval_seconds

    async def synthesize(self, text: str) -> Optional[bytes]:
        if not text.strip():
            return None

        headers = {
            "api-key": self._api_key,
            "voice": self._voice,
            "speed": self._speed,
            "Content-Type": "text/plain; charset=utf-8",
        }

        async with httpx.AsyncClient(timeout=60.0, follow_redirects=True) as client:
            response = await client.post(self._url, headers=headers, content=text.encode("utf-8"))
            response.raise_for_status()

            content_type = response.headers.get("content-type", "")
            if content_type.startswith("audio/"):
                return response.content

            payload = response.json()
            audio_url = extract_audio_url(payload)
            if not audio_url:
                raise ValueError(f"FPT TTS response did not include audio URL: {payload}")

            return await self._download_audio(client, audio_url)

    async def _download_audio(self, client: httpx.AsyncClient, audio_url: str) -> bytes:
        last_error: Optional[Exception] = None
        for attempt in range(self._poll_attempts):
            try:
                response = await client.get(audio_url)
                if response.status_code == 200 and response.content:
                    return response.content
                if response.status_code in (202, 404):
                    logger.info(
                        "FPT TTS audio is not ready yet status=%s attempt=%s url=%s",
                        response.status_code,
                        attempt + 1,
                        audio_url,
                    )
                    await asyncio.sleep(self._poll_interval_seconds)
                    continue
                response.raise_for_status()
            except httpx.HTTPError as exc:
                last_error = exc
            await asyncio.sleep(self._poll_interval_seconds)

        if last_error is not None:
            raise last_error
        raise TimeoutError(f"FPT TTS audio was not ready: {audio_url}")


def extract_audio_url(payload: dict[str, Any]) -> str:
    error = payload.get("error")
    if error not in (None, 0, "0"):
        raise ValueError(f"FPT TTS returned error: {payload}")

    for key in ("async", "audio_url", "url", "link"):
        value = payload.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()

    data = payload.get("data")
    if isinstance(data, str) and data.strip():
        return data.strip()
    if isinstance(data, dict):
        return extract_audio_url(data)

    return ""

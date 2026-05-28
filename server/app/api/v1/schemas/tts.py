from __future__ import annotations

from typing import Optional

from pydantic import BaseModel, Field


class TtsRequest(BaseModel):
    text: str = Field(min_length=1, max_length=4096)
    voice: Optional[str] = Field(default=None, max_length=64)
    speed: Optional[str] = Field(default=None, max_length=16)


class TtsResponse(BaseModel):
    text: str
    voice: str
    speed: str
    audio_url: str
    size: int

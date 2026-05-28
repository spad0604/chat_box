from typing import Any

from pydantic import BaseModel


class AsrResponse(BaseModel):
    filename: str
    content_type: str
    size: int
    transcript: str
    raw: dict[str, Any]

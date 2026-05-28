from __future__ import annotations

from typing import Optional

from pydantic import BaseModel


class DocumentUploadResponse(BaseModel):
    filename: str
    content_type: str
    size: int
    file_id: str
    vector_store_id: str
    vector_store_file_id: Optional[str] = None
    status: str

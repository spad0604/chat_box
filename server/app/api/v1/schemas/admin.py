from __future__ import annotations

from typing import Optional

from pydantic import BaseModel


class UsageBucket(BaseModel):
    bucket: str
    total_messages: int
    user_messages: int
    assistant_messages: int
    sessions: int


class UsageStatsResponse(BaseModel):
    period: str
    total_messages: int
    user_messages: int
    assistant_messages: int
    sessions: int
    buckets: list[UsageBucket]


class TopQuestionResponse(BaseModel):
    question: str
    count: int
    last_asked_at: str


class UnansweredQuestionResponse(BaseModel):
    session_id: str
    question: str
    answer: str
    asked_at: str


class AdminDocumentResponse(BaseModel):
    id: int
    filename: str
    content_type: str
    size: int
    file_id: str
    vector_store_id: str
    vector_store_file_id: Optional[str] = None
    status: str
    created_at: str
    updated_at: str


class DocumentDeleteResponse(BaseModel):
    id: int
    deleted: bool


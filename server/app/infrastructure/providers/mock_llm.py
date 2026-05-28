from __future__ import annotations

from typing import Optional

from app.ports.llm import LlmPort


class MockLlmProvider(LlmPort):
    async def generate_reply(self, message: str, session_id: str, history: Optional[list[dict]] = None) -> str:
        return f"Toi da nhan: {message}. Day la cau tra loi mau cho session {session_id[:8]}."


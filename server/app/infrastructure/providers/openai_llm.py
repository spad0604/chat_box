from __future__ import annotations

import asyncio
from pathlib import Path
from typing import Any, Optional

import httpx

from app.ports.llm import LlmPort


SYSTEM_INSTRUCTIONS = """
Ban la UniMate, tro ly sinh vien cua FPT University.
Tra loi bang tieng Viet, ngan gon, ro rang.
Neu cau hoi lien quan den quy che, quy dinh, hoc vu, hoc phi, lich thi, thuc tap,
hoac chinh sach noi bo, chi duoc tra loi dua tren tai lieu duoc tim thay bang file_search.
Neu khong tim thay can cu trong tai lieu, hay noi: "Minh chua co thong tin nay trong tai lieu quy che duoc cung cap."
Khong bia so lieu, moc thoi gian, dieu khoan, muc phi, dieu kien, hay quy trinh.
""".strip()


class OpenAiLlmProvider(LlmPort):
    def __init__(
        self,
        api_key: str,
        model: str,
        vector_store_id: str = "",
        vector_store_id_file: str = "",
    ) -> None:
        if not api_key:
            raise ValueError("OPENAI_API_KEY is required")
        self._api_key = api_key
        self._model = model
        self._vector_store_id = vector_store_id
        self._vector_store_id_file = Path(vector_store_id_file) if vector_store_id_file else None

    async def generate_reply(self, message: str, session_id: str, history: Optional[list[dict]] = None) -> str:
        headers = {
            "Authorization": f"Bearer {self._api_key}",
            "Content-Type": "application/json",
        }
        payload: dict[str, Any] = {
            "model": self._model,
            "instructions": SYSTEM_INSTRUCTIONS,
            "input": build_input(message, history or []),
        }

        vector_store_id = self._get_vector_store_id()
        if vector_store_id:
            payload["tools"] = [
                {
                    "type": "file_search",
                    "vector_store_ids": [vector_store_id],
                }
            ]

        async with httpx.AsyncClient(timeout=90.0) as client:
            response = await post_with_retry(
                client=client,
                url="https://api.openai.com/v1/responses",
                headers=headers,
                payload=payload,
            )

        if response.status_code >= 400:
            raise OpenAiProviderError.from_response(response)

        return extract_output_text(response.json())

    def _get_vector_store_id(self) -> str:
        if self._vector_store_id:
            return self._vector_store_id
        if self._vector_store_id_file and self._vector_store_id_file.exists():
            return self._vector_store_id_file.read_text(encoding="utf-8").strip()
        return ""


def build_input(message: str, history: list[dict]) -> str:
    recent = history[-10:]
    lines = ["Lich su gan day:"]
    for item in recent:
        role = item.get("role", "user")
        content = item.get("content", "")
        lines.append(f"{role}: {content}")
    lines.append(f"user: {message}")
    return "\n".join(lines)


def extract_output_text(payload: dict[str, Any]) -> str:
    output_text = payload.get("output_text")
    if isinstance(output_text, str) and output_text.strip():
        return output_text.strip()

    parts: list[str] = []
    for output in payload.get("output", []):
        if not isinstance(output, dict):
            continue
        for content in output.get("content", []):
            if not isinstance(content, dict):
                continue
            text = content.get("text")
            if isinstance(text, str) and text.strip():
                parts.append(text.strip())
    if parts:
        return "\n".join(parts)
    raise ValueError(f"OpenAI response did not include output text: {payload}")


class OpenAiProviderError(Exception):
    def __init__(self, status_code: int, message: str) -> None:
        super().__init__(message)
        self.status_code = status_code
        self.message = message

    @classmethod
    def from_response(cls, response: httpx.Response) -> "OpenAiProviderError":
        message = response.text
        try:
            payload = response.json()
            error = payload.get("error")
            if isinstance(error, dict):
                message = error.get("message") or message
        except ValueError:
            pass

        if response.status_code == 429:
            message = f"OpenAI rate limit or quota reached: {message}"
        return cls(status_code=response.status_code, message=message)


async def post_with_retry(
    client: httpx.AsyncClient,
    url: str,
    headers: dict[str, str],
    payload: dict[str, Any],
) -> httpx.Response:
    response: Optional[httpx.Response] = None
    for attempt in range(3):
        response = await client.post(url, headers=headers, json=payload)
        if response.status_code != 429:
            return response

        retry_after = parse_retry_after(response.headers.get("retry-after"))
        delay = retry_after if retry_after is not None else 1.0 + attempt
        await asyncio.sleep(delay)

    return response


def parse_retry_after(value: Optional[str]) -> Optional[float]:
    if not value:
        return None
    try:
        return max(0.0, min(float(value), 10.0))
    except ValueError:
        return None

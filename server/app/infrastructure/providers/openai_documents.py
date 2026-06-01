from pathlib import Path
from typing import Any

import httpx


class OpenAiDocumentProvider:
    def __init__(
        self,
        api_key: str,
        vector_store_id: str,
        vector_store_id_file: str,
        vector_store_name: str,
    ) -> None:
        if not api_key:
            raise ValueError("OPENAI_API_KEY is required")
        self._api_key = api_key
        self._vector_store_id = vector_store_id
        self._vector_store_id_file = Path(vector_store_id_file)
        self._vector_store_name = vector_store_name

    async def upload_document(self, filename: str, content_type: str, content: bytes) -> dict[str, Any]:
        headers = {"Authorization": f"Bearer {self._api_key}"}
        async with httpx.AsyncClient(timeout=120.0) as client:
            vector_store_id = await self._get_or_create_vector_store_id(client, headers)
            file_response = await client.post(
                "https://api.openai.com/v1/files",
                headers=headers,
                files={
                    "purpose": (None, "assistants"),
                    "file": (filename, content, content_type or "application/octet-stream"),
                },
            )
            file_response.raise_for_status()
            file_payload = file_response.json()
            file_id = file_payload["id"]

            attach_response = await client.post(
                f"https://api.openai.com/v1/vector_stores/{vector_store_id}/files",
                headers={**headers, "Content-Type": "application/json"},
                json={"file_id": file_id},
            )
            attach_response.raise_for_status()
            vector_file_payload = attach_response.json()

        return {
            "filename": filename,
            "content_type": content_type,
            "size": len(content),
            "file_id": file_id,
            "vector_store_id": vector_store_id,
            "vector_store_file_id": vector_file_payload.get("id"),
            "status": vector_file_payload.get("status", "in_progress"),
        }

    async def delete_document(self, vector_store_id: str, file_id: str) -> None:
        headers = {"Authorization": f"Bearer {self._api_key}"}
        async with httpx.AsyncClient(timeout=60.0) as client:
            vector_response = await client.delete(
                f"https://api.openai.com/v1/vector_stores/{vector_store_id}/files/{file_id}",
                headers=headers,
            )
            if vector_response.status_code not in (200, 404):
                vector_response.raise_for_status()

            file_response = await client.delete(
                f"https://api.openai.com/v1/files/{file_id}",
                headers=headers,
            )
            if file_response.status_code not in (200, 404):
                file_response.raise_for_status()

    async def _get_or_create_vector_store_id(self, client: httpx.AsyncClient, headers: dict[str, str]) -> str:
        if self._vector_store_id:
            return self._vector_store_id
        if self._vector_store_id_file.exists():
            vector_store_id = self._vector_store_id_file.read_text(encoding="utf-8").strip()
            if vector_store_id:
                return vector_store_id

        response = await client.post(
            "https://api.openai.com/v1/vector_stores",
            headers={**headers, "Content-Type": "application/json"},
            json={"name": self._vector_store_name},
        )
        response.raise_for_status()
        vector_store_id = response.json()["id"]
        self._vector_store_id_file.parent.mkdir(parents=True, exist_ok=True)
        self._vector_store_id_file.write_text(vector_store_id, encoding="utf-8")
        return vector_store_id

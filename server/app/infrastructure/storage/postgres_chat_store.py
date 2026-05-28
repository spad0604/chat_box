import asyncio
from urllib.parse import urlparse, urlunparse

from app.ports.chat_store import ChatStorePort


class PostgresChatStore(ChatStorePort):
    def __init__(self, database_url: str) -> None:
        if not database_url:
            raise ValueError("DATABASE_URL is required when CHAT_STORE_PROVIDER=postgres")
        self._database_url = ensure_sslmode(database_url)
        self._initialized = False
        self._init_lock = asyncio.Lock()

    async def append_message(self, session_id: str, role: str, content: str) -> None:
        await self._ensure_initialized()
        await asyncio.to_thread(self._append_message_sync, session_id, role, content)

    async def get_messages(self, session_id: str, limit: int = 20) -> list[dict]:
        await self._ensure_initialized()
        return await asyncio.to_thread(self._get_messages_sync, session_id, limit)

    async def get_sessions(self, limit: int = 50) -> list[dict]:
        await self._ensure_initialized()
        return await asyncio.to_thread(self._get_sessions_sync, limit)

    async def _ensure_initialized(self) -> None:
        if self._initialized:
            return
        async with self._init_lock:
            if self._initialized:
                return
            await asyncio.to_thread(self._init_sync)
            self._initialized = True

    def _connect(self):
        import psycopg

        return psycopg.connect(self._database_url)

    def _init_sync(self) -> None:
        with self._connect() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    """
                    create table if not exists chat_messages (
                        id bigserial primary key,
                        session_id text not null,
                        role text not null check (role in ('user', 'assistant', 'system')),
                        content text not null,
                        created_at timestamptz not null default now()
                    )
                    """
                )
                cur.execute(
                    """
                    create index if not exists idx_chat_messages_session_created
                    on chat_messages (session_id, created_at)
                    """
                )
            conn.commit()

    def _append_message_sync(self, session_id: str, role: str, content: str) -> None:
        with self._connect() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "insert into chat_messages (session_id, role, content) values (%s, %s, %s)",
                    (session_id, role, content),
                )
            conn.commit()

    def _get_messages_sync(self, session_id: str, limit: int) -> list[dict]:
        with self._connect() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    """
                    select role, content, created_at
                    from chat_messages
                    where session_id = %s
                    order by created_at desc, id desc
                    limit %s
                    """,
                    (session_id, limit),
                )
                rows = cur.fetchall()
        rows.reverse()
        return [
            {
                "role": role,
                "content": content,
                "created_at": created_at.isoformat(),
            }
            for role, content, created_at in rows
        ]

    def _get_sessions_sync(self, limit: int) -> list[dict]:
        with self._connect() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    """
                    select distinct on (session_id) session_id, created_at, content
                    from chat_messages
                    order by session_id, created_at desc, id desc
                    """
                )
                rows = cur.fetchall()
        sessions = [
            {
                "session_id": session_id,
                "updated_at": created_at.isoformat(),
                "last_message": content,
            }
            for session_id, created_at, content in rows
        ]
        sessions.sort(key=lambda item: item["updated_at"], reverse=True)
        return sessions[:limit]


def ensure_sslmode(database_url: str) -> str:
    parsed = urlparse(database_url)
    if "sslmode=" in parsed.query:
        return database_url
    query = f"{parsed.query}&sslmode=require" if parsed.query else "sslmode=require"
    return urlunparse(parsed._replace(query=query))

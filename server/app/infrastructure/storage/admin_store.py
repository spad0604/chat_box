import asyncio
from urllib.parse import urlparse, urlunparse


UNANSWERED_MARKERS = (
    "Minh chua co thong tin nay trong tai lieu quy che duoc cung cap.",
    "Mình chưa có thông tin này trong tài liệu quy chế được cung cấp.",
    "chua co thong tin",
    "chưa có thông tin",
    "khong tim thay",
    "không tìm thấy",
)


class PostgresAdminStore:
    def __init__(self, database_url: str) -> None:
        if not database_url:
            raise ValueError("DATABASE_URL is required")
        self._database_url = ensure_sslmode(database_url)
        self._initialized = False
        self._init_lock = asyncio.Lock()

    async def ensure_initialized(self) -> None:
        if self._initialized:
            return
        async with self._init_lock:
            if self._initialized:
                return
            await asyncio.to_thread(self._init_sync)
            self._initialized = True

    async def usage_stats(self, period: str) -> dict:
        await self.ensure_initialized()
        return await asyncio.to_thread(self._usage_stats_sync, period)

    async def top_questions(self, limit: int = 20) -> list[dict]:
        await self.ensure_initialized()
        return await asyncio.to_thread(self._top_questions_sync, limit)

    async def unanswered_questions(self, limit: int = 50) -> list[dict]:
        await self.ensure_initialized()
        return await asyncio.to_thread(self._unanswered_questions_sync, limit)

    async def list_documents(self) -> list[dict]:
        await self.ensure_initialized()
        return await asyncio.to_thread(self._list_documents_sync)

    async def get_document(self, document_id: int) -> dict | None:
        await self.ensure_initialized()
        return await asyncio.to_thread(self._get_document_sync, document_id)

    async def save_document(self, document: dict) -> dict:
        await self.ensure_initialized()
        return await asyncio.to_thread(self._save_document_sync, document)

    async def update_document(self, document_id: int, document: dict) -> dict:
        await self.ensure_initialized()
        return await asyncio.to_thread(self._update_document_sync, document_id, document)

    async def delete_document(self, document_id: int) -> bool:
        await self.ensure_initialized()
        return await asyncio.to_thread(self._delete_document_sync, document_id)

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
                    create table if not exists documents (
                        id bigserial primary key,
                        filename text not null,
                        content_type text not null,
                        size bigint not null,
                        file_id text not null,
                        vector_store_id text not null,
                        vector_store_file_id text,
                        status text not null,
                        created_at timestamptz not null default now(),
                        updated_at timestamptz not null default now()
                    )
                    """
                )
                cur.execute(
                    """
                    create index if not exists idx_documents_updated
                    on documents (updated_at desc)
                    """
                )
            conn.commit()

    def _usage_stats_sync(self, period: str) -> dict:
        trunc_unit = {"day": "day", "week": "week", "month": "month"}.get(period, "day")
        with self._connect() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    """
                    select
                        count(*)::int as total_messages,
                        count(*) filter (where role = 'user')::int as user_messages,
                        count(*) filter (where role = 'assistant')::int as assistant_messages,
                        count(distinct session_id)::int as sessions
                    from chat_messages
                    """
                )
                total_messages, user_messages, assistant_messages, sessions = cur.fetchone()
                cur.execute(
                    f"""
                    select
                        date_trunc('{trunc_unit}', created_at) as bucket,
                        count(*)::int as total_messages,
                        count(*) filter (where role = 'user')::int as user_messages,
                        count(*) filter (where role = 'assistant')::int as assistant_messages,
                        count(distinct session_id)::int as sessions
                    from chat_messages
                    group by bucket
                    order by bucket desc
                    limit 30
                    """
                )
                rows = cur.fetchall()

        return {
            "period": period,
            "total_messages": total_messages,
            "user_messages": user_messages,
            "assistant_messages": assistant_messages,
            "sessions": sessions,
            "buckets": [
                {
                    "bucket": bucket.isoformat(),
                    "total_messages": total,
                    "user_messages": users,
                    "assistant_messages": assistants,
                    "sessions": bucket_sessions,
                }
                for bucket, total, users, assistants, bucket_sessions in rows
            ],
        }

    def _top_questions_sync(self, limit: int) -> list[dict]:
        with self._connect() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    """
                    select trim(content) as question, count(*)::int as count, max(created_at) as last_asked_at
                    from chat_messages
                    where role = 'user' and length(trim(content)) > 0
                    group by lower(trim(content)), trim(content)
                    order by count desc, last_asked_at desc
                    limit %s
                    """,
                    (limit,),
                )
                rows = cur.fetchall()
        return [
            {
                "question": question,
                "count": count,
                "last_asked_at": last_asked_at.isoformat(),
            }
            for question, count, last_asked_at in rows
        ]

    def _unanswered_questions_sync(self, limit: int) -> list[dict]:
        marker_conditions = " or ".join(["lower(answer) like %s" for _ in UNANSWERED_MARKERS])
        marker_values = [f"%{marker.lower()}%" for marker in UNANSWERED_MARKERS]
        with self._connect() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    f"""
                    with ordered as (
                        select
                            session_id,
                            role,
                            content,
                            created_at,
                            lead(role) over (partition by session_id order by created_at, id) as next_role,
                            lead(content) over (partition by session_id order by created_at, id) as answer
                        from chat_messages
                    )
                    select session_id, content as question, answer, created_at
                    from ordered
                    where role = 'user'
                      and next_role = 'assistant'
                      and ({marker_conditions})
                    order by created_at desc
                    limit %s
                    """,
                    (*marker_values, limit),
                )
                rows = cur.fetchall()
        return [
            {
                "session_id": session_id,
                "question": question,
                "answer": answer,
                "asked_at": asked_at.isoformat(),
            }
            for session_id, question, answer, asked_at in rows
        ]

    def _list_documents_sync(self) -> list[dict]:
        with self._connect() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    """
                    select id, filename, content_type, size, file_id, vector_store_id,
                           vector_store_file_id, status, created_at, updated_at
                    from documents
                    order by updated_at desc, id desc
                    """
                )
                rows = cur.fetchall()
        return [document_row_to_dict(row) for row in rows]

    def _get_document_sync(self, document_id: int) -> dict | None:
        with self._connect() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    """
                    select id, filename, content_type, size, file_id, vector_store_id,
                           vector_store_file_id, status, created_at, updated_at
                    from documents
                    where id = %s
                    """,
                    (document_id,),
                )
                row = cur.fetchone()
        return document_row_to_dict(row) if row else None

    def _save_document_sync(self, document: dict) -> dict:
        with self._connect() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    """
                    insert into documents (
                        filename, content_type, size, file_id, vector_store_id,
                        vector_store_file_id, status
                    )
                    values (%s, %s, %s, %s, %s, %s, %s)
                    returning id, filename, content_type, size, file_id, vector_store_id,
                              vector_store_file_id, status, created_at, updated_at
                    """,
                    (
                        document["filename"],
                        document["content_type"],
                        document["size"],
                        document["file_id"],
                        document["vector_store_id"],
                        document.get("vector_store_file_id"),
                        document["status"],
                    ),
                )
                row = cur.fetchone()
            conn.commit()
        return document_row_to_dict(row)

    def _update_document_sync(self, document_id: int, document: dict) -> dict:
        with self._connect() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    """
                    update documents
                    set filename = %s,
                        content_type = %s,
                        size = %s,
                        file_id = %s,
                        vector_store_id = %s,
                        vector_store_file_id = %s,
                        status = %s,
                        updated_at = now()
                    where id = %s
                    returning id, filename, content_type, size, file_id, vector_store_id,
                              vector_store_file_id, status, created_at, updated_at
                    """,
                    (
                        document["filename"],
                        document["content_type"],
                        document["size"],
                        document["file_id"],
                        document["vector_store_id"],
                        document.get("vector_store_file_id"),
                        document["status"],
                        document_id,
                    ),
                )
                row = cur.fetchone()
            conn.commit()
        if not row:
            raise KeyError(document_id)
        return document_row_to_dict(row)

    def _delete_document_sync(self, document_id: int) -> bool:
        with self._connect() as conn:
            with conn.cursor() as cur:
                cur.execute("delete from documents where id = %s", (document_id,))
                deleted = cur.rowcount > 0
            conn.commit()
        return deleted


def document_row_to_dict(row) -> dict:
    (
        document_id,
        filename,
        content_type,
        size,
        file_id,
        vector_store_id,
        vector_store_file_id,
        status,
        created_at,
        updated_at,
    ) = row
    return {
        "id": document_id,
        "filename": filename,
        "content_type": content_type,
        "size": size,
        "file_id": file_id,
        "vector_store_id": vector_store_id,
        "vector_store_file_id": vector_store_file_id,
        "status": status,
        "created_at": created_at.isoformat(),
        "updated_at": updated_at.isoformat(),
    }


def ensure_sslmode(database_url: str) -> str:
    parsed = urlparse(database_url)
    if "sslmode=" in parsed.query:
        return database_url
    query = f"{parsed.query}&sslmode=require" if parsed.query else "sslmode=require"
    return urlunparse(parsed._replace(query=query))


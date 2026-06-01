import httpx
from fastapi import APIRouter, Depends, File, HTTPException, UploadFile

from app.api.v1.schemas.admin import AdminDocumentResponse, DocumentDeleteResponse
from app.api.v1.schemas.documents import DocumentUploadResponse
from app.infrastructure.config import settings
from app.infrastructure.dependencies import get_admin_store
from app.infrastructure.providers.openai_documents import OpenAiDocumentProvider
from app.infrastructure.storage.admin_store import PostgresAdminStore

router = APIRouter()


def get_document_provider() -> OpenAiDocumentProvider:
    return OpenAiDocumentProvider(
        api_key=settings.openai_api_key,
        vector_store_id=settings.openai_vector_store_id,
        vector_store_id_file=settings.openai_vector_store_id_file,
        vector_store_name=settings.openai_vector_store_name,
    )


@router.post("/upload", response_model=DocumentUploadResponse)
async def upload_document(
    file: UploadFile = File(...),
    store: PostgresAdminStore = Depends(get_admin_store),
) -> DocumentUploadResponse:
    content = await file.read()
    result = await get_document_provider().upload_document(
        filename=file.filename or "document",
        content_type=file.content_type or "application/octet-stream",
        content=content,
    )
    await store.save_document(result)
    return DocumentUploadResponse.model_validate(result)


@router.get("", response_model=list[AdminDocumentResponse])
async def list_documents(
    store: PostgresAdminStore = Depends(get_admin_store),
) -> list[AdminDocumentResponse]:
    documents = await store.list_documents()
    return [AdminDocumentResponse.model_validate(item) for item in documents]


@router.put("/{document_id}", response_model=AdminDocumentResponse)
async def replace_document(
    document_id: int,
    file: UploadFile = File(...),
    store: PostgresAdminStore = Depends(get_admin_store),
) -> AdminDocumentResponse:
    existing = await store.get_document(document_id)
    if existing is None:
        raise HTTPException(status_code=404, detail="Document not found")

    provider = get_document_provider()
    try:
        await provider.delete_document(
            vector_store_id=existing["vector_store_id"],
            file_id=existing["file_id"],
        )
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail=f"OpenAI document delete failed: {exc}") from exc

    content = await file.read()
    uploaded = await provider.upload_document(
        filename=file.filename or existing["filename"],
        content_type=file.content_type or "application/octet-stream",
        content=content,
    )
    updated = await store.update_document(document_id, uploaded)
    return AdminDocumentResponse.model_validate(updated)


@router.delete("/{document_id}", response_model=DocumentDeleteResponse)
async def delete_document(
    document_id: int,
    store: PostgresAdminStore = Depends(get_admin_store),
) -> DocumentDeleteResponse:
    existing = await store.get_document(document_id)
    if existing is None:
        raise HTTPException(status_code=404, detail="Document not found")

    try:
        await get_document_provider().delete_document(
            vector_store_id=existing["vector_store_id"],
            file_id=existing["file_id"],
        )
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail=f"OpenAI document delete failed: {exc}") from exc

    deleted = await store.delete_document(document_id)
    return DocumentDeleteResponse(id=document_id, deleted=deleted)

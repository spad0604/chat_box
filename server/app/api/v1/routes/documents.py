from fastapi import APIRouter, File, UploadFile

from app.api.v1.schemas.documents import DocumentUploadResponse
from app.infrastructure.config import settings
from app.infrastructure.providers.openai_documents import OpenAiDocumentProvider

router = APIRouter()


def get_document_provider() -> OpenAiDocumentProvider:
    return OpenAiDocumentProvider(
        api_key=settings.openai_api_key,
        vector_store_id=settings.openai_vector_store_id,
        vector_store_id_file=settings.openai_vector_store_id_file,
        vector_store_name=settings.openai_vector_store_name,
    )


@router.post("/upload", response_model=DocumentUploadResponse)
async def upload_document(file: UploadFile = File(...)) -> DocumentUploadResponse:
    content = await file.read()
    result = await get_document_provider().upload_document(
        filename=file.filename or "document",
        content_type=file.content_type or "application/octet-stream",
        content=content,
    )
    return DocumentUploadResponse.model_validate(result)

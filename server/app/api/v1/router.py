from fastapi import APIRouter

from app.api.v1.routes import asr, audio, chat, documents, health, tts

api_router = APIRouter()
api_router.include_router(health.router, tags=["health"])
api_router.include_router(chat.router, prefix="/chat", tags=["chat"])
api_router.include_router(audio.router, prefix="/audio", tags=["audio"])
api_router.include_router(asr.router, prefix="/asr", tags=["asr"])
api_router.include_router(tts.router, prefix="/tts", tags=["tts"])
api_router.include_router(documents.router, prefix="/documents", tags=["documents"])


from fastapi import APIRouter

from app.api.v1.routes import audio, chat, health

api_router = APIRouter()
api_router.include_router(health.router, tags=["health"])
api_router.include_router(chat.router, prefix="/chat", tags=["chat"])
api_router.include_router(audio.router, prefix="/audio", tags=["audio"])


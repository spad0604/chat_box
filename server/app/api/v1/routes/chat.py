from __future__ import annotations

import asyncio
import logging
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional
from uuid import uuid4

from fastapi import APIRouter, Depends, File, Form, HTTPException, UploadFile

from app.api.v1.schemas.chat import (
    ChatMessageResponse,
    ChatResponse,
    ChatSessionResponse,
    ChatTextRequest,
    VoiceChatResponse,
)
from app.application.chat_service import ChatService
from app.infrastructure.config import settings
from app.infrastructure.dependencies import get_chat_service
from app.infrastructure.providers.openai_llm import OpenAiProviderError

router = APIRouter()
logger = logging.getLogger(__name__)


@router.get("/sessions", response_model=list[ChatSessionResponse])
async def list_chat_sessions(
    limit: int = 50,
    service: ChatService = Depends(get_chat_service),
) -> list[ChatSessionResponse]:
    sessions = await service.get_sessions(limit=limit)
    return [ChatSessionResponse.model_validate(item) for item in sessions]


@router.get("/history/{session_id}", response_model=list[ChatMessageResponse])
async def get_chat_history(
    session_id: str,
    limit: int = 50,
    service: ChatService = Depends(get_chat_service),
) -> list[ChatMessageResponse]:
    messages = await service.get_history(session_id=session_id, limit=limit)
    return [ChatMessageResponse.model_validate(item) for item in messages]


@router.post("/text", response_model=ChatResponse)
async def chat_text(
    request: ChatTextRequest,
    service: ChatService = Depends(get_chat_service),
) -> ChatResponse:
    try:
        result = await service.chat_text(
            message=request.message,
            session_id=request.session_id,
        )
    except OpenAiProviderError as exc:
        raise HTTPException(status_code=exc.status_code, detail=exc.message) from exc
    return ChatResponse.model_validate(result)


@router.post("/voice", response_model=VoiceChatResponse)
async def chat_voice(
    file: UploadFile = File(...),
    session_id: Optional[str] = Form(default=None),
    service: ChatService = Depends(get_chat_service),
) -> VoiceChatResponse:
    audio_bytes = await file.read()
    saved_path = save_voice_upload(audio_bytes, file.filename or "recording.wav")
    asr_path = saved_path
    asr_filename = file.filename or "recording.wav"
    asr_content_type = file.content_type or "application/octet-stream"

    if saved_path.suffix.lower() != ".m4a":
        try:
            asr_path = await convert_audio_to_m4a(saved_path)
            audio_bytes = asr_path.read_bytes()
            asr_filename = asr_path.name
            asr_content_type = "audio/x-m4a"
        except AudioConvertError as exc:
            logger.warning(
                "Voice convert failed path=%s size=%s error=%s",
                saved_path,
                len(audio_bytes),
                exc,
            )
            raise HTTPException(
                status_code=422,
                detail={
                    "message": str(exc),
                    "saved_voice_file": str(saved_path),
                    "size": len(audio_bytes),
                    "hint": "Install ffmpeg on EC2 or upload M4A directly.",
                },
            ) from exc

    try:
        result = await service.chat_voice(
            audio_bytes=audio_bytes,
            filename=asr_filename,
            content_type=asr_content_type,
            session_id=session_id,
        )
    except OpenAiProviderError as exc:
        raise HTTPException(status_code=exc.status_code, detail=exc.message) from exc
    except ValueError as exc:
        logger.warning(
            "Voice ASR failed saved_path=%s asr_path=%s size=%s content_type=%s error=%s",
            saved_path,
            asr_path,
            len(audio_bytes),
            asr_content_type,
            exc,
        )
        raise HTTPException(
            status_code=422,
            detail={
                "message": str(exc),
                "saved_voice_file": str(saved_path),
                "asr_file": str(asr_path),
                "size": len(audio_bytes),
                "content_type": asr_content_type,
            },
        ) from exc
    return VoiceChatResponse.model_validate(result)


def save_voice_upload(audio_bytes: bytes, original_filename: str) -> Path:
    upload_dir = Path(settings.voice_upload_dir)
    upload_dir.mkdir(parents=True, exist_ok=True)

    suffix = Path(original_filename).suffix.lower()
    if suffix not in {".wav", ".mp3", ".m4a", ".aac", ".raw", ".pcm"}:
        suffix = ".wav"

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    path = upload_dir / f"voice_{timestamp}_{uuid4().hex}{suffix}"
    path.write_bytes(audio_bytes)
    logger.info("Saved voice upload path=%s size=%s", path, len(audio_bytes))
    return path


class AudioConvertError(RuntimeError):
    pass


async def convert_audio_to_m4a(input_path: Path) -> Path:
    output_path = input_path.with_name(f"{input_path.stem}_asr.m4a")
    command = [
        "ffmpeg",
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(input_path),
        "-ac",
        "1",
        "-ar",
        "16000",
        "-codec:a",
        "aac",
        "-b:a",
        "64k",
        "-movflags",
        "+faststart",
        str(output_path),
    ]

    try:
        process = await asyncio.create_subprocess_exec(
            *command,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
    except FileNotFoundError as exc:
        raise AudioConvertError("ffmpeg is not installed on this server") from exc

    _, stderr = await process.communicate()
    if process.returncode != 0:
        message = stderr.decode("utf-8", errors="replace").strip()
        raise AudioConvertError(f"ffmpeg convert failed: {message}")

    if not output_path.exists() or output_path.stat().st_size == 0:
        raise AudioConvertError("ffmpeg convert failed: output M4A is empty")

    logger.info("Converted voice upload input=%s output=%s size=%s", input_path, output_path, output_path.stat().st_size)
    return output_path


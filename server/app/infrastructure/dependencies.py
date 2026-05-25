from functools import lru_cache

from app.application.chat_service import ChatService
from app.infrastructure.config import settings
from app.infrastructure.providers.mock_llm import MockLlmProvider
from app.infrastructure.providers.mock_stt import MockSpeechToTextProvider
from app.infrastructure.providers.mock_tts import MockTextToSpeechProvider
from app.infrastructure.storage.local_audio_storage import LocalAudioStorage
from app.ports.audio_storage import AudioStoragePort
from app.ports.llm import LlmPort
from app.ports.stt import SpeechToTextPort
from app.ports.tts import TextToSpeechPort


@lru_cache
def get_llm_provider() -> LlmPort:
    if settings.llm_provider == "mock":
        return MockLlmProvider()
    raise ValueError(f"Unsupported LLM provider: {settings.llm_provider}")


@lru_cache
def get_stt_provider() -> SpeechToTextPort:
    if settings.stt_provider == "mock":
        return MockSpeechToTextProvider()
    raise ValueError(f"Unsupported STT provider: {settings.stt_provider}")


@lru_cache
def get_tts_provider() -> TextToSpeechPort:
    if settings.tts_provider == "mock":
        return MockTextToSpeechProvider()
    raise ValueError(f"Unsupported TTS provider: {settings.tts_provider}")


@lru_cache
def get_audio_storage() -> AudioStoragePort:
    return LocalAudioStorage(
        storage_dir=settings.audio_storage_dir,
        public_base_url=settings.public_base_url,
    )


def get_chat_service() -> ChatService:
    return ChatService(
        llm=get_llm_provider(),
        stt=get_stt_provider(),
        tts=get_tts_provider(),
        audio_storage=get_audio_storage(),
    )


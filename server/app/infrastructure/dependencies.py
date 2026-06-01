from functools import lru_cache

from app.application.chat_service import ChatService
from app.infrastructure.config import settings
from app.infrastructure.providers.fpt_asr import FptAsrProvider
from app.infrastructure.providers.fpt_tts import FptTtsProvider
from app.infrastructure.providers.mock_llm import MockLlmProvider
from app.infrastructure.providers.mock_stt import MockSpeechToTextProvider
from app.infrastructure.providers.mock_tts import MockTextToSpeechProvider
from app.infrastructure.providers.openai_llm import OpenAiLlmProvider
from app.infrastructure.storage.local_audio_storage import LocalAudioStorage
from app.infrastructure.storage.admin_store import PostgresAdminStore
from app.infrastructure.storage.memory_chat_store import MemoryChatStore
from app.infrastructure.storage.postgres_chat_store import PostgresChatStore
from app.ports.audio_storage import AudioStoragePort
from app.ports.chat_store import ChatStorePort
from app.ports.llm import LlmPort
from app.ports.stt import SpeechToTextPort
from app.ports.tts import TextToSpeechPort


@lru_cache
def get_llm_provider() -> LlmPort:
    if settings.llm_provider == "mock":
        return MockLlmProvider()
    if settings.llm_provider == "openai":
        return OpenAiLlmProvider(
            api_key=settings.openai_api_key,
            model=settings.openai_model,
            vector_store_id=settings.openai_vector_store_id,
            vector_store_id_file=settings.openai_vector_store_id_file,
        )
    raise ValueError(f"Unsupported LLM provider: {settings.llm_provider}")


@lru_cache
def get_stt_provider() -> SpeechToTextPort:
    if settings.stt_provider == "mock":
        return MockSpeechToTextProvider()
    if settings.stt_provider == "fpt":
        return FptAsrProvider(
            api_key=settings.fpt_asr_api_key,
            url=settings.fpt_asr_url,
        )
    raise ValueError(f"Unsupported STT provider: {settings.stt_provider}")


@lru_cache
def get_tts_provider() -> TextToSpeechPort:
    if settings.tts_provider == "mock":
        return MockTextToSpeechProvider()
    if settings.tts_provider == "fpt":
        return FptTtsProvider(
            api_key=settings.fpt_tts_api_key or settings.fpt_asr_api_key,
            url=settings.fpt_tts_url,
            voice=settings.fpt_tts_voice,
            speed=settings.fpt_tts_speed,
            poll_attempts=settings.fpt_tts_poll_attempts,
            poll_interval_seconds=settings.fpt_tts_poll_interval_seconds,
        )
    raise ValueError(f"Unsupported TTS provider: {settings.tts_provider}")


@lru_cache
def get_audio_storage() -> AudioStoragePort:
    return LocalAudioStorage(
        storage_dir=settings.audio_storage_dir,
        public_base_url=settings.public_base_url,
    )


@lru_cache
def get_chat_store() -> ChatStorePort:
    if settings.chat_store_provider == "memory":
        return MemoryChatStore()
    if settings.chat_store_provider == "postgres":
        return PostgresChatStore(settings.database_url)
    raise ValueError(f"Unsupported chat store provider: {settings.chat_store_provider}")


@lru_cache
def get_admin_store() -> PostgresAdminStore:
    return PostgresAdminStore(settings.database_url)


def get_chat_service() -> ChatService:
    return ChatService(
        llm=get_llm_provider(),
        stt=get_stt_provider(),
        tts=get_tts_provider(),
        chat_store=get_chat_store(),
        audio_storage=get_audio_storage(),
    )


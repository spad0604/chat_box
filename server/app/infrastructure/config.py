from functools import lru_cache

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8")

    app_name: str = "ChatBox Server"
    app_env: str = "dev"
    public_base_url: str = "http://localhost:8000"

    llm_provider: str = "mock"
    stt_provider: str = "mock"
    tts_provider: str = "mock"
    chat_store_provider: str = "memory"

    audio_storage_dir: str = "storage/audio"
    database_url: str = ""

    openai_api_key: str = ""
    openai_model: str = "gpt-4.1"
    openai_vector_store_id: str = ""
    openai_vector_store_id_file: str = "storage/openai_vector_store_id.txt"
    openai_vector_store_name: str = "fpt-university-regulations"

    fpt_asr_url: str = "https://api.fpt.ai/hmi/asr/general"
    fpt_asr_api_key: str = ""

    fpt_tts_url: str = "https://api.fpt.ai/hmi/tts/v5"
    fpt_tts_api_key: str = ""
    fpt_tts_voice: str = "banmai"
    fpt_tts_speed: str = ""
    fpt_tts_poll_attempts: int = 45
    fpt_tts_poll_interval_seconds: float = 1.0


@lru_cache
def get_settings() -> Settings:
    return Settings()


settings = get_settings()


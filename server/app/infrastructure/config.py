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

    audio_storage_dir: str = "storage/audio"


@lru_cache
def get_settings() -> Settings:
    return Settings()


settings = get_settings()


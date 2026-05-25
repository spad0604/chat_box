from fastapi import FastAPI

from app.api.v1.router import api_router
from app.infrastructure.config import settings


def create_app() -> FastAPI:
    app = FastAPI(title=settings.app_name)
    app.include_router(api_router, prefix="/api/v1")
    return app


app = create_app()


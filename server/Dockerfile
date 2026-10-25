# Server FastAPI (docs/SERVER.md §8). Build từ thư mục gốc repo vì cần cả ml/ (features.py, models):
#   docker compose up -d --build
FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PIP_NO_CACHE_DIR=1 \
    TZ=Asia/Ho_Chi_Minh

# tzdata: đặc trưng hour_sin/hour_cos tính theo giờ Việt Nam
RUN apt-get update && apt-get install -y --no-install-recommends tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY server/requirements.txt server/requirements.txt
RUN pip install -r server/requirements.txt

COPY ml ml
COPY server server

WORKDIR /app/server
EXPOSE 8000
CMD ["python", "-m", "app"]

FROM python:3.11-slim

WORKDIR /app

# Install dependencies for ebook conversion (if any system deps are needed)
# RUN apt-get update && apt-get install -y pandoc && rm -rf /var/lib/apt/lists/*

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

# Coolify exposes PORT env variable, defaulting to 8000
ENV PORT=8000

CMD ["sh", "-c", "uvicorn app:app --host 0.0.0.0 --port ${PORT}"]

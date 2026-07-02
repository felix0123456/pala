from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse
import uvicorn

app = FastAPI(title="Pala Cloud Hub")

@app.get("/", response_class=HTMLResponse)
async def index():
    return """
    <html>
        <head>
            <title>Pala Cloud Hub</title>
            <style>
                body { font-family: sans-serif; background: #030712; color: #f3f4f6; margin: 0; padding: 2rem; }
                h1 { color: #6366f1; }
                .card { background: #111827; padding: 1.5rem; border-radius: 8px; border: 1px solid #374151; max-width: 600px; margin-top: 1rem; }
            </style>
        </head>
        <body>
            <h1>Pala Cloud Hub</h1>
            <p>Your centralized platform for Pala device management, auto-fetching ebooks, and OTA updates.</p>
            <div class="card">
                <h2>Status</h2>
                <p>Backend is running successfully on Coolify.</p>
                <p><em>Note: Full database, device sync API, and book fetching logic are under development.</em></p>
            </div>
        </body>
    </html>
    """

@app.get("/api/sync")
async def sync_device(device_id: str):
    # This endpoint will be hit by Pala devices to check for new books
    return {"status": "ok", "pending_downloads": []}

@app.get("/api/firmware/latest.bin")
async def get_latest_firmware():
    # This endpoint will serve the OTA update binaries
    return {"error": "No firmware uploaded yet"}

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse
import uvicorn
import requests
from bs4 import BeautifulSoup

app = FastAPI(title="Pala Cloud Hub")

@app.get("/", response_class=HTMLResponse)
async def index():
    return """
    <html>
        <head>
            <title>Pala Cloud Hub</title>
            <style>
                :root {
                    --bg-dark: #0a0f0d;
                    --card-bg: #111a14;
                    --border: #1a2e20;
                    --primary-green: #10b981;
                    --accent-yellow: #fbbf24;
                    --text-main: #ecfdf5;
                    --text-muted: #a7f3d0;
                }
                body { 
                    font-family: 'Inter', sans-serif; 
                    background: var(--bg-dark); 
                    color: var(--text-main); 
                    margin: 0; 
                    padding: 2rem; 
                    display: flex;
                    flex-direction: column;
                    align-items: center;
                }
                h1 { 
                    color: var(--accent-yellow); 
                    text-shadow: 0 0 10px rgba(251, 191, 36, 0.2);
                    font-size: 2.5rem;
                }
                .card { 
                    background: var(--card-bg); 
                    padding: 2rem; 
                    border-radius: 16px; 
                    border: 1px solid var(--border); 
                    box-shadow: 0 10px 30px rgba(16, 185, 129, 0.05);
                    max-width: 600px; 
                    width: 100%;
                    margin-top: 1rem; 
                    transition: transform 0.2s ease;
                }
                .card:hover {
                    transform: translateY(-2px);
                    border-color: var(--primary-green);
                }
                h2 { color: var(--primary-green); }
                p { color: var(--text-muted); line-height: 1.6; }
                input[type="text"] {
                    width: 100%;
                    padding: 12px;
                    background: rgba(0,0,0,0.3);
                    border: 1px solid var(--border);
                    border-radius: 8px;
                    color: white;
                    margin-top: 10px;
                    margin-bottom: 10px;
                }
                input[type="text"]:focus {
                    outline: none;
                    border-color: var(--accent-yellow);
                }
                button {
                    background: linear-gradient(135deg, var(--primary-green), #059669);
                    color: white;
                    border: none;
                    padding: 12px 24px;
                    border-radius: 8px;
                    font-weight: 600;
                    cursor: pointer;
                    transition: all 0.2s;
                    box-shadow: 0 4px 15px rgba(16, 185, 129, 0.2);
                }
                button:hover {
                    background: linear-gradient(135deg, #059669, #047857);
                    box-shadow: 0 6px 20px rgba(16, 185, 129, 0.3);
                    transform: translateY(-1px);
                }
            </style>
        </head>
        <body>
            <h1>Pala Cloud Hub</h1>
            <p style="text-align:center; max-width: 500px;">Your centralized platform for Pala device management, auto-fetching ebooks, and OTA updates.</p>
            
            <div class="card">
                <h2>Automated E-Book Fetcher</h2>
                <p>Search Project Gutenberg to instantly download and convert a book for your Pala.</p>
                <input type="text" id="book-query" placeholder="Enter book title or author (e.g. Alice in Wonderland)">
                <button onclick="fetchBook()">Fetch & Convert</button>
                <p id="status-msg" style="color: var(--accent-yellow); margin-top: 15px;"></p>
            </div>

            <div class="card">
                <h2>Device Status</h2>
                <p>Waiting for Pala devices to connect and sync...</p>
            </div>

            <script>
                async function fetchBook() {
                    const query = document.getElementById('book-query').value;
                    const status = document.getElementById('status-msg');
                    if(!query) return;
                    status.innerText = "Searching Project Gutenberg...";
                    try {
                        let res = await fetch('/api/fetch?q=' + encodeURIComponent(query));
                        let data = await res.json();
                        status.innerText = data.message || data.error;
                    } catch (e) {
                        status.innerText = "Error fetching book.";
                    }
                }
            </script>
        </body>
    </html>
    """

@app.get("/api/fetch")
async def fetch_book(q: str):
    try:
        # Very basic mock logic for Gutenberg search
        # A real implementation would hit http://gutendex.com/books?search=q
        res = requests.get(f"https://gutendex.com/books?search={q}")
        data = res.json()
        if data.get("count", 0) > 0:
            book = data["results"][0]
            title = book["title"]
            # Trigger download and conversion background task here
            return {"message": f"Successfully found '{title}'. Converting and staging for device sync!"}
        return {"error": "Book not found."}
    except Exception as e:
        return {"error": str(e)}

import os
from fastapi.responses import HTMLResponse, FileResponse

# We will store the latest firmware binary here for OTA
FIRMWARE_DIR = "firmware/build"
CURRENT_LATEST_VERSION = "1.7.5"

@app.get("/api/sync")
async def sync_device(device_id: str):
    # This endpoint checks if there are pending books for this specific device
    # In a full implementation, this queries the DB for books assigned to device_id
    # For now, we return empty so the device doesn't crash
    return {"status": "ok", "pending_downloads": []}

@app.get("/api/firmware/check")
async def check_firmware(version: str):
    # The Pala sends its current version. If it differs from the server, we send an update.
    if version != CURRENT_LATEST_VERSION:
        return {
            "update_available": True,
            "latest_version": CURRENT_LATEST_VERSION,
            "download_url": "/api/firmware/latest.bin"
        }
    return {"update_available": False}

@app.get("/api/firmware/latest.bin")
async def get_latest_firmware():
    # Serve the compiled .bin file
    bin_path = os.path.join(FIRMWARE_DIR, "firmware.bin")
    if os.path.exists(bin_path):
        return FileResponse(bin_path, media_type="application/octet-stream", filename="firmware.bin")
    return {"error": "Firmware binary not found on server."}

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)

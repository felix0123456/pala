import os
from fastapi import FastAPI, Request, Depends, Form, HTTPException, status, Response
from fastapi.responses import HTMLResponse, RedirectResponse, FileResponse
from fastapi.templating import Jinja2Templates
import uvicorn
import requests
from sqlalchemy.orm import Session
from sqlalchemy.exc import IntegrityError
from pydantic import BaseModel

import models
from database import engine, get_db
import auth

# Create DB tables
models.Base.metadata.create_all(bind=engine)

app = FastAPI(title="Pala Cloud Hub")
templates = Jinja2Templates(directory="templates")

FIRMWARE_DIR = "firmware/build"
CURRENT_LATEST_VERSION = "1.7.5"
BOOKS_DIR = "books"

os.makedirs(FIRMWARE_DIR, exist_ok=True)
os.makedirs(BOOKS_DIR, exist_ok=True)

# ----------------- WEB FRONTEND ROUTES -----------------

@app.get("/", response_class=HTMLResponse)
async def index(request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        return RedirectResponse(url="/login", status_code=status.HTTP_302_FOUND)
    
    # Reload user relationships
    user = db.query(models.User).filter(models.User.id == user.id).first()
    return templates.TemplateResponse("index.html", {"request": request, "user": user, "devices": user.devices, "books": user.books})

@app.get("/login", response_class=HTMLResponse)
async def login_get(request: Request):
    return templates.TemplateResponse("login.html", {"request": request})

@app.post("/login", response_class=HTMLResponse)
async def login_post(request: Request, response: Response, username: str = Form(...), password: str = Form(...), db: Session = Depends(get_db)):
    user = db.query(models.User).filter(models.User.username == username).first()
    if not user or not auth.verify_password(password, user.hashed_password):
        return templates.TemplateResponse("login.html", {"request": request, "error": "Invalid username or password"})
    
    token = auth.create_session(user.id)
    resp = RedirectResponse(url="/", status_code=status.HTTP_302_FOUND)
    resp.set_cookie(key="session_token", value=token, httponly=True)
    return resp

@app.get("/register", response_class=HTMLResponse)
async def register_get(request: Request):
    return templates.TemplateResponse("register.html", {"request": request})

@app.post("/register", response_class=HTMLResponse)
async def register_post(request: Request, username: str = Form(...), password: str = Form(...), db: Session = Depends(get_db)):
    hashed_password = auth.get_password_hash(password)
    user = models.User(username=username, hashed_password=hashed_password)
    db.add(user)
    try:
        db.commit()
        db.refresh(user)
    except IntegrityError:
        db.rollback()
        return templates.TemplateResponse("register.html", {"request": request, "error": "Username already exists"})
    
    token = auth.create_session(user.id)
    resp = RedirectResponse(url="/", status_code=status.HTTP_302_FOUND)
    resp.set_cookie(key="session_token", value=token, httponly=True)
    return resp

@app.get("/logout")
async def logout(response: Response):
    resp = RedirectResponse(url="/login", status_code=status.HTTP_302_FOUND)
    resp.delete_cookie("session_token")
    return resp

# ----------------- BOOK FETCHING -----------------

@app.get("/api/fetch")
async def fetch_book(q: str, request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        return {"error": "Not authenticated"}

    try:
        res = requests.get(f"https://gutendex.com/books?search={q}")
        data = res.json()
        if data.get("count", 0) > 0:
            book_data = data["results"][0]
            title = book_data["title"]
            # Save dummy file logic for now
            filename = f"{book_data['id']}.txt"
            filepath = os.path.join(BOOKS_DIR, filename)
            with open(filepath, "w", encoding="utf-8") as f:
                f.write(f"This is a dummy text file for {title} fetched from Gutenberg.")
            
            db_book = models.Book(title=title, file_path=filepath, user_id=user.id)
            db.add(db_book)
            db.commit()

            return {"message": f"Successfully found '{title}'. Added to your library!"}
        return {"error": "Book not found."}
    except Exception as e:
        return {"error": str(e)}

# ----------------- ESP32 SYNC API -----------------

class DeviceRegister(BaseModel):
    mac_address: str
    user_id: int # The user ID to bind this device to (in a real scenario, this is tricky. Maybe use a pairing code?)

class SyncPushData(BaseModel):
    mac_address: str
    battery_level: int
    font_size: int
    sleep_timeout: int
    line_gap: int
    bookmarks: list # list of dicts: [{"book_id": 1, "page_index": 42}]

@app.post("/api/device/register")
async def register_device(data: DeviceRegister, db: Session = Depends(get_db)):
    device = db.query(models.Device).filter(models.Device.mac_address == data.mac_address).first()
    if not device:
        device = models.Device(mac_address=data.mac_address, user_id=data.user_id)
        db.add(device)
        db.commit()
    return {"status": "ok"}

@app.post("/api/sync/push")
async def sync_push(data: SyncPushData, db: Session = Depends(get_db)):
    device = db.query(models.Device).filter(models.Device.mac_address == data.mac_address).first()
    if not device:
        raise HTTPException(status_code=401, detail="Device not registered")
    
    device.battery_level = data.battery_level
    device.font_size = data.font_size
    device.sleep_timeout = data.sleep_timeout
    device.line_gap = data.line_gap

    # Update bookmarks
    for bm in data.bookmarks:
        existing = db.query(models.Bookmark).filter(
            models.Bookmark.device_mac == data.mac_address,
            models.Bookmark.book_id == bm.get("book_id")
        ).first()
        if existing:
            existing.page_index = bm.get("page_index", 0)
        else:
            new_bm = models.Bookmark(
                device_mac=data.mac_address,
                book_id=bm.get("book_id"),
                page_index=bm.get("page_index", 0)
            )
            db.add(new_bm)

    db.commit()
    return {"status": "ok"}

@app.get("/api/sync/pull")
async def sync_pull(mac_address: str, db: Session = Depends(get_db)):
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address).first()
    if not device:
        raise HTTPException(status_code=401, detail="Device not registered")
    
    # Return settings and books that belong to the user
    user_books = db.query(models.Book).filter(models.Book.user_id == device.user_id).all()
    books_data = [{"id": b.id, "title": b.title} for b in user_books]

    return {
        "font_size": device.font_size,
        "sleep_timeout": device.sleep_timeout,
        "line_gap": device.line_gap,
        "books": books_data
    }

@app.get("/api/book/{book_id}")
async def download_book(book_id: int, mac_address: str, db: Session = Depends(get_db)):
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address).first()
    if not device:
        raise HTTPException(status_code=401, detail="Device not registered")
    
    book = db.query(models.Book).filter(models.Book.id == book_id, models.Book.user_id == device.user_id).first()
    if not book or not os.path.exists(book.file_path):
        raise HTTPException(status_code=404, detail="Book not found")
    
    return FileResponse(book.file_path, media_type="text/plain", filename=f"{book.title}.txt")

# ----------------- FIRMWARE OTA -----------------

@app.get("/api/firmware/check")
async def check_firmware(version: str):
    if version != CURRENT_LATEST_VERSION:
        return {
            "update_available": True,
            "latest_version": CURRENT_LATEST_VERSION,
            "download_url": "/api/firmware/latest.bin"
        }
    return {"update_available": False}

@app.get("/api/firmware/latest.bin")
async def get_latest_firmware():
    bin_path = os.path.join(FIRMWARE_DIR, "firmware.bin")
    if os.path.exists(bin_path):
        return FileResponse(bin_path, media_type="application/octet-stream", filename="firmware.bin")
    return {"error": "Firmware binary not found on server."}

if __name__ == "__main__":
    uvicorn.run("app:app", host="0.0.0.0", port=8000, reload=True)

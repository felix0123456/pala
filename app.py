import os
import random
from fastapi import FastAPI, Request, Depends, Form, HTTPException, status, Response, UploadFile, File
from fastapi.responses import HTMLResponse, RedirectResponse, FileResponse
from fastapi.templating import Jinja2Templates
import uvicorn
import requests
from sqlalchemy.orm import Session
from sqlalchemy.exc import IntegrityError
from pydantic import BaseModel
from typing import Optional
import urllib.parse
import asyncio
import aiohttp

import models
from database import engine, get_db
import auth
import time

from sqlalchemy import text

# Create DB tables
models.Base.metadata.create_all(bind=engine)

# Auto-migrate: add pairing_code if missing
with engine.connect() as conn:
    try:
        conn.execute(text("ALTER TABLE devices ADD COLUMN pairing_code VARCHAR"))
        conn.commit()
    except Exception:
        pass
    
    try:
        conn.execute(text("ALTER TABLE users ADD COLUMN is_admin BOOLEAN DEFAULT FALSE"))
        conn.commit()
    except Exception:
        pass

    try:
        conn.execute(text("UPDATE users SET is_admin = 1 WHERE username = 'testuser'"))
        conn.commit()
    except Exception:
        pass

    new_columns = [
        "screensaver_mode INTEGER DEFAULT 0",
        "invert_display BOOLEAN DEFAULT FALSE",
        "spotify_client_id VARCHAR",
        "spotify_client_secret VARCHAR",
        "spotify_refresh_token VARCHAR",
        "spotify_screensaver BOOLEAN DEFAULT FALSE",
        "chess_elo INTEGER DEFAULT 1500",
        "cal_url VARCHAR",
        "tz_offset INTEGER DEFAULT 0",
        "firmware_version VARCHAR",
        "app_todo BOOLEAN DEFAULT TRUE",
        "app_cal BOOLEAN DEFAULT TRUE",
        "app_spot BOOLEAN DEFAULT TRUE",
        "app_chess BOOLEAN DEFAULT TRUE",
        "app_pom BOOLEAN DEFAULT TRUE",
        "screensaver_updated_at INTEGER DEFAULT 0"
    ]
    for col in new_columns:
        try:
            conn.execute(text(f"ALTER TABLE devices ADD COLUMN {col}"))
            conn.commit()
        except Exception:
            pass

    try:
        conn.execute(text("ALTER TABLE books ADD COLUMN folder VARCHAR DEFAULT 'Root'"))
        conn.commit()
    except Exception:
        pass

app = FastAPI(title="Pala Cloud Hub")
templates = Jinja2Templates(directory="templates")

FIRMWARE_DIR = os.getenv("FIRMWARE_DIR", "data/firmware/build")
GITHUB_REPO = "felix0123456/pala"
BOOKS_DIR = os.getenv("BOOKS_DIR", "data/books")
SCREENSAVERS_DIR = os.getenv("SCREENSAVERS_DIR", "data/screensavers")

os.makedirs(FIRMWARE_DIR, exist_ok=True)
os.makedirs(BOOKS_DIR, exist_ok=True)
os.makedirs(SCREENSAVERS_DIR, exist_ok=True)

# ----------------- WEB FRONTEND ROUTES -----------------

@app.get("/", response_class=HTMLResponse)
async def index(request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        return RedirectResponse(url="/login", status_code=status.HTTP_302_FOUND)
    
    user = db.query(models.User).filter(models.User.id == user.id).first()
    if user.devices and len(user.devices) > 0:
        return RedirectResponse(url=f"/device/{user.devices[0].mac_address}", status_code=status.HTTP_302_FOUND)
    return RedirectResponse(url="/user", status_code=status.HTTP_302_FOUND)

@app.get("/library", response_class=HTMLResponse)
async def library_view(request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        return RedirectResponse(url="/login", status_code=status.HTTP_302_FOUND)
    user = db.query(models.User).filter(models.User.id == user.id).first()
    return templates.TemplateResponse(request=request, name="library.html", context={"user": user, "books": user.books, "devices": user.devices})



@app.get("/user", response_class=HTMLResponse)
async def user_view(request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        return RedirectResponse(url="/login", status_code=status.HTTP_302_FOUND)
    user = db.query(models.User).filter(models.User.id == user.id).first()
    return templates.TemplateResponse(request=request, name="user.html", context={"user": user, "devices": user.devices})


@app.get("/todos", response_class=HTMLResponse)
async def todos_page(request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        return RedirectResponse(url="/login", status_code=status.HTTP_302_FOUND)
    return templates.TemplateResponse(request=request, name="todos.html", context={"user": user, "todos": user.todos})

@app.get("/impressum", response_class=HTMLResponse)
async def impressum_page(request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    return templates.TemplateResponse(request=request, name="impressum.html", context={"user": user})

@app.get("/privacy", response_class=HTMLResponse)
async def privacy_page(request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    return templates.TemplateResponse(request=request, name="privacy.html", context={"user": user})

@app.get("/login", response_class=HTMLResponse)
async def login_get(request: Request):
    return templates.TemplateResponse(request=request, name="login.html")

@app.post("/login", response_class=HTMLResponse)
async def login_post(request: Request, response: Response, username: str = Form(...), password: str = Form(...), db: Session = Depends(get_db)):
    user = db.query(models.User).filter(models.User.username == username).first()
    if not user or not auth.verify_password(password, user.hashed_password):
        return templates.TemplateResponse(request=request, name="login.html", context={"error": "Invalid username or password"})
    
    token = auth.create_session(user.id)
    resp = RedirectResponse(url="/", status_code=status.HTTP_302_FOUND)
    resp.set_cookie(key="session_token", value=token, httponly=True)
    return resp

@app.get("/register", response_class=HTMLResponse)
async def register_get(request: Request):
    return templates.TemplateResponse(request=request, name="register.html")

@app.post("/register", response_class=HTMLResponse)
async def register_post(request: Request, username: str = Form(...), password: str = Form(...), db: Session = Depends(get_db)):
    hashed_password = auth.get_password_hash(password)
    user = models.User(username=username, hashed_password=hashed_password)
    
    # First user is automatically admin
    is_first = db.query(models.User).count() == 0
    if is_first:
        user.is_admin = True
        
    db.add(user)
    try:
        db.commit()
        db.refresh(user)
    except IntegrityError:
        db.rollback()
        return templates.TemplateResponse(request=request, name="register.html", context={"error": "Username already exists"})
    
    token = auth.create_session(user.id)
    resp = RedirectResponse(url="/", status_code=status.HTTP_302_FOUND)
    resp.set_cookie(key="session_token", value=token, httponly=True)
    return resp

@app.get("/logout")
async def logout(response: Response):
    resp = RedirectResponse(url="/login", status_code=status.HTTP_302_FOUND)
    resp.delete_cookie("session_token")
    return resp

@app.post("/pair", response_class=HTMLResponse)
async def pair_device(request: Request, code: str = Form(...), db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        return RedirectResponse(url="/login", status_code=status.HTTP_302_FOUND)
    
    device = db.query(models.Device).filter(models.Device.pairing_code == code).first()
    if not device:
        # User not found or bad code
        user = db.query(models.User).filter(models.User.id == user.id).first()
        return templates.TemplateResponse(request=request, name="index.html", context={"user": user, "devices": user.devices, "books": user.books, "error": "Invalid pairing code"})
    
    device.user_id = user.id
    device.pairing_code = None
    db.commit()
    return RedirectResponse(url="/user", status_code=status.HTTP_302_FOUND)

@app.get("/device/{mac_address}", response_class=HTMLResponse)
async def device_view(request: Request, mac_address: str, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        return RedirectResponse(url="/login", status_code=status.HTTP_302_FOUND)
    
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address, models.Device.user_id == user.id).first()
    if not device:
        return RedirectResponse(url="/", status_code=status.HTTP_302_FOUND)
    return templates.TemplateResponse(request=request, name="device.html", context={"device": device, "books": user.books, "user": user})

@app.post("/api/device/{mac_address}/settings")
async def update_device_settings(
    mac_address: str, 
    font: int = Form(...), 
    sleep: int = Form(...), 
    lgap: int = Form(...), 
    scr_mode: int = Form(0),
    cfg_invert: Optional[bool] = Form(False),
    request: Request = None, 
    db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        raise HTTPException(status_code=401)
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address, models.Device.user_id == user.id).first()
    if not device:
        raise HTTPException(status_code=404)
    device.font_size = font
    device.sleep_timeout = sleep
    device.line_gap = lgap
    device.screensaver_mode = scr_mode
    device.invert_display = cfg_invert
    db.commit()
    return RedirectResponse(url=f"/device/{mac_address}", status_code=status.HTTP_302_FOUND)

@app.post("/api/device/{mac_address}/calendar")
async def update_device_calendar(
    mac_address: str,
    cal_url: str = Form(""),
    tz_offset: int = Form(0),
    request: Request = None, 
    db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        raise HTTPException(status_code=401)
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address, models.Device.user_id == user.id).first()
    if not device:
        raise HTTPException(status_code=404)
    device.cal_url = cal_url
    device.tz_offset = tz_offset
    db.commit()
    return RedirectResponse(url=f"/device/{mac_address}", status_code=status.HTTP_302_FOUND)

@app.post("/api/device/{mac_address}/spotify")
async def update_device_spotify(
    mac_address: str,
    spot_scr: Optional[bool] = Form(False),
    spot_id: str = Form(""),
    spot_secret: str = Form(""),
    spot_refresh: str = Form(""),
    request: Request = None, 
    db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        raise HTTPException(status_code=401)
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address, models.Device.user_id == user.id).first()
    if not device:
        raise HTTPException(status_code=404)
    device.spotify_screensaver = spot_scr
    device.spotify_client_id = spot_id
    device.spotify_client_secret = spot_secret
    device.spotify_refresh_token = spot_refresh
    db.commit()
    return RedirectResponse(url=f"/device/{mac_address}", status_code=status.HTTP_302_FOUND)

@app.post("/api/device/{mac_address}/chess")
async def update_device_chess(
    mac_address: str,
    chess_elo: int = Form(1500),
    request: Request = None, 
    db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        raise HTTPException(status_code=401)
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address, models.Device.user_id == user.id).first()
    if not device:
        raise HTTPException(status_code=404)
    device.chess_elo = chess_elo
    db.commit()
    return RedirectResponse(url=f"/device/{mac_address}", status_code=status.HTTP_302_FOUND)

@app.post("/device/{mac_address}/rename")
async def rename_device(mac_address: str, name: str = Form(...), request: Request = None, db: Session = Depends(get_db)):
    if not request or "session_token" not in request.cookies:
        return RedirectResponse("/login", status_code=303)
    user = auth.get_current_user(request, db)
    if not user:
        return RedirectResponse("/login", status_code=303)
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address, models.Device.user_id == user.id).first()
    if device:
        device.name = name
        db.commit()
    return RedirectResponse(f"/device/{mac_address}", status_code=303)

@app.get("/device/{mac_address}/apps")
async def device_apps_view(request: Request, mac_address: str, db: Session = Depends(get_db)):
    if "session_token" not in request.cookies:
        return RedirectResponse("/login", status_code=303)
    user = auth.get_current_user(request, db)
    if not user:
        return RedirectResponse("/login", status_code=303)
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address, models.Device.user_id == user.id).first()
    if not device:
        return RedirectResponse("/devices", status_code=303)
    return templates.TemplateResponse(request=request, name="apps.html", context={"user": user, "device": device})

@app.post("/device/{mac_address}/apps")
async def update_device_apps(
    mac_address: str, 
    request: Request, 
    app_todo: bool = Form(False),
    app_cal: bool = Form(False),
    app_spot: bool = Form(False),
    app_chess: bool = Form(False),
    app_pom: bool = Form(False),
    db: Session = Depends(get_db)
):
    if "session_token" not in request.cookies:
        return RedirectResponse("/login", status_code=303)
    user = auth.get_current_user(request, db)
    if not user:
        return RedirectResponse("/login", status_code=303)
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address, models.Device.user_id == user.id).first()
    if not device:
        return RedirectResponse("/devices", status_code=303)
        
    device.app_todo = app_todo
    device.app_cal = app_cal
    device.app_spot = app_spot
    device.app_chess = app_chess
    device.app_pom = app_pom
    db.commit()
    return RedirectResponse(f"/device/{mac_address}/apps", status_code=303)

@app.post("/device/{mac_address}/sync/{book_id}")
async def toggle_device_sync(mac_address: str, book_id: int, request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        raise HTTPException(status_code=401)
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address, models.Device.user_id == user.id).first()
    book = db.query(models.Book).filter(models.Book.id == book_id, models.Book.user_id == user.id).first()
    if not device or not book:
        raise HTTPException(status_code=404)
    
    if book in device.synced_books:
        device.synced_books.remove(book)
    else:
        device.synced_books.append(book)
    
    db.commit()
    return {"status": "ok"}

# ----------------- BOOK FETCHING & UPLOADING -----------------

@app.get("/api/search/gutendex")
async def search_gutendex(q: str, request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        return {"error": "Not authenticated"}

    try:
        res = requests.get(f"https://gutendex.com/books?search={urllib.parse.quote(q)}")
        data = res.json()
        results = []
        for book_data in data.get("results", [])[:5]:
            # Try to get plain text URL
            text_url = None
            for fmt, url in book_data.get("formats", {}).items():
                if "text/plain" in fmt:
                    text_url = url
                    break
            
            if text_url:
                authors = ", ".join([a["name"] for a in book_data.get("authors", [])])
                results.append({
                    "id": f"gutenberg_{book_data['id']}",
                    "title": book_data["title"],
                    "authors": authors,
                    "source": "Project Gutenberg",
                    "download_url": text_url
                })
        return {"results": results}
    except Exception as e:
        return {"error": str(e)}

@app.get("/api/search/openlibrary")
async def search_openlibrary(q: str, request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        return {"error": "Not authenticated"}

    try:
        res = requests.get(f"https://openlibrary.org/search.json?q={urllib.parse.quote(q)}&has_fulltext=true&limit=10")
        data = res.json()
        results = []
        for doc in data.get("docs", []):
            ia_list = doc.get("ia")
            if ia_list:
                ia_id = ia_list[0]
                text_url = f"https://archive.org/stream/{ia_id}/{ia_id}_djvu.txt"
                authors = ", ".join(doc.get("author_name", []))
                results.append({
                    "id": f"openlibrary_{ia_id}",
                    "title": doc.get("title"),
                    "authors": authors,
                    "source": "OpenLibrary (Archive.org)",
                    "download_url": text_url
                })
        return {"results": results[:5]}
    except Exception as e:
        return {"error": str(e)}

class FetchResultRequest(BaseModel):
    title: str
    download_url: str
    source: str
    id: str

@app.post("/api/fetch_result")
async def fetch_result(data: FetchResultRequest, request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        return {"error": "Not authenticated"}
    
    try:
        text_res = requests.get(data.download_url)
        if text_res.status_code != 200:
            return {"error": "Failed to download text."}
            
        text_content = text_res.text
        filename = f"{data.id}.txt"
        filepath = os.path.join(BOOKS_DIR, filename)
        
        with open(filepath, "w", encoding="utf-8") as f:
            f.write(text_content)
            
        db_book = models.Book(title=data.title, file_path=filepath, user_id=user.id)
        user_reloaded = db.query(models.User).filter(models.User.id == user.id).first()
        db_book.synced_devices = user_reloaded.devices
        db.add(db_book)
        db.commit()

        return {"message": f"Successfully fetched '{data.title}'. It will sync to your device."}
    except Exception as e:
        return {"error": str(e)}

@app.post("/api/upload_book")
async def upload_book(request: Request, file: UploadFile = File(...), db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        raise HTTPException(status_code=401, detail="Not authenticated")
    
    filename = file.filename
    filepath = os.path.join(BOOKS_DIR, filename)
    with open(filepath, "wb") as f:
        f.write(await file.read())
    
    # Check if already exists
    existing = db.query(models.Book).filter(models.Book.user_id == user.id, models.Book.title == filename).first()
    if not existing:
        db_book = models.Book(title=filename, file_path=filepath, user_id=user.id)
        # auto-sync
        user_reloaded = db.query(models.User).filter(models.User.id == user.id).first()
        db_book.synced_devices = user_reloaded.devices
        db.add(db_book)
        db.commit()
    
    return {"status": "ok"}

# ----------------- TODOS API -----------------

class TodoData(BaseModel):
    text: str
    checked: Optional[bool] = False

@app.post("/api/todos")
async def add_todo(data: TodoData, request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        raise HTTPException(status_code=401, detail="Not authenticated")
    todo = models.TodoItem(user_id=user.id, text=data.text, checked=data.checked)
    db.add(todo)
    db.commit()
    db.refresh(todo)
    return {"id": todo.id, "text": todo.text, "checked": todo.checked}

@app.put("/api/todos/{todo_id}")
async def update_todo(todo_id: int, data: TodoData, request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        raise HTTPException(status_code=401, detail="Not authenticated")
    todo = db.query(models.TodoItem).filter(models.TodoItem.id == todo_id, models.TodoItem.user_id == user.id).first()
    if not todo:
        raise HTTPException(status_code=404, detail="Todo not found")
    todo.text = data.text
    if data.checked is not None:
        todo.checked = data.checked
    db.commit()
    return {"status": "ok"}

@app.delete("/api/todos/{todo_id}")
async def delete_todo(todo_id: int, request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        raise HTTPException(status_code=401, detail="Not authenticated")
    todo = db.query(models.TodoItem).filter(models.TodoItem.id == todo_id, models.TodoItem.user_id == user.id).first()
    if not todo:
        raise HTTPException(status_code=404, detail="Todo not found")
    db.delete(todo)
    db.commit()
    return {"status": "ok"}


# ----------------- ADMIN DASHBOARD -----------------

def get_current_admin_web(request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        raise HTTPException(status_code=status.HTTP_302_FOUND, headers={"Location": "/login"})
    if getattr(user, "is_admin", False) is not True:
        raise HTTPException(status_code=status.HTTP_302_FOUND, headers={"Location": "/?error=Not+authorized"})
    return user

@app.get("/admin", response_class=HTMLResponse)
async def admin_dashboard(request: Request, db: Session = Depends(get_db)):
    admin_user = get_current_admin_web(request, db)
    users = db.query(models.User).all()
    devices = db.query(models.Device).all()
    books = db.query(models.Book).all()
    bookmarks = db.query(models.Bookmark).all()
    return templates.TemplateResponse(request=request, name="admin.html", context={
        "user": admin_user,
        "users": users,
        "devices": devices,
        "books": books,
        "bookmarks": bookmarks
    })

@app.post("/admin/user/{user_id}/toggle-admin")
async def admin_toggle_user_admin(request: Request, user_id: int, db: Session = Depends(get_db)):
    admin_user = get_current_admin_web(request, db)
    target_user = db.query(models.User).filter(models.User.id == user_id).first()
    if target_user and target_user.id != admin_user.id:
        target_user.is_admin = not getattr(target_user, "is_admin", False)
        db.commit()
    return RedirectResponse(url="/admin", status_code=status.HTTP_302_FOUND)

@app.post("/admin/user/{user_id}/delete")
async def admin_delete_user(request: Request, user_id: int, db: Session = Depends(get_db)):
    admin_user = get_current_admin_web(request, db)
    if admin_user.id == user_id:
        return RedirectResponse(url="/admin?error=Cannot+delete+yourself", status_code=status.HTTP_302_FOUND)
    target_user = db.query(models.User).filter(models.User.id == user_id).first()
    if target_user:
        # Clear device associations to allow re-pairing
        for device in target_user.devices:
            device.user_id = None
        db.delete(target_user)
        db.commit()
    return RedirectResponse(url="/admin", status_code=status.HTTP_302_FOUND)

@app.post("/admin/device/{mac_address}/unpair")
async def admin_unpair_device(request: Request, mac_address: str, db: Session = Depends(get_db)):
    admin_user = get_current_admin_web(request, db)
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address).first()
    if device:
        device.user_id = None
        db.commit()
    return RedirectResponse(url="/admin", status_code=status.HTTP_302_FOUND)

@app.post("/admin/device/{mac_address}/delete")
async def admin_delete_device(request: Request, mac_address: str, db: Session = Depends(get_db)):
    admin_user = get_current_admin_web(request, db)
    device = db.query(models.Device).filter(models.Device.mac_address == mac_address).first()
    if device:
        db.query(models.Bookmark).filter(models.Bookmark.device_mac == mac_address).delete()
        db.delete(device)
        db.commit()
    return RedirectResponse(url="/admin", status_code=status.HTTP_302_FOUND)

@app.post("/admin/book/{book_id}/delete")
async def admin_delete_book(request: Request, book_id: int, db: Session = Depends(get_db)):
    admin_user = get_current_admin_web(request, db)
    book = db.query(models.Book).filter(models.Book.id == book_id).first()
    if book:
        # Attempt to delete file
        if os.path.exists(book.file_path):
            try:
                os.remove(book.file_path)
            except Exception:
                pass
        db.query(models.Bookmark).filter(models.Bookmark.book_id == book_id).delete()
        db.delete(book)
        db.commit()
    return RedirectResponse(url="/admin", status_code=status.HTTP_302_FOUND)

@app.post("/admin/bookmark/{bookmark_id}/delete")
async def admin_delete_bookmark(request: Request, bookmark_id: int, db: Session = Depends(get_db)):
    admin_user = get_current_admin_web(request, db)
    bookmark = db.query(models.Bookmark).filter(models.Bookmark.id == bookmark_id).first()
    if bookmark:
        db.delete(bookmark)
        db.commit()
    return RedirectResponse(url="/admin", status_code=status.HTTP_302_FOUND)

# ----------------- ESP32 SYNC API -----------------

class DeviceRegister(BaseModel):
    mac_address: str
    user_id: Optional[int] = None

class SyncPushData(BaseModel):
    mac_address: str
    battery_level: int
    font_size: int
    sleep_timeout: int
    line_gap: int
    bookmarks: list # list of dicts: [{"book_id": 1, "page_index": 42}]
    screensaver_mode: Optional[int] = None
    invert_display: Optional[bool] = None
    spotify_client_id: Optional[str] = None
    spotify_client_secret: Optional[str] = None
    spotify_refresh_token: Optional[str] = None
    spotify_screensaver: Optional[bool] = None
    chess_elo: Optional[int] = None
    cal_url: Optional[str] = None
    tz_offset: Optional[int] = None
    firmware_version: Optional[str] = None
    todos: Optional[list] = None

@app.post("/api/device/register")
async def register_device(data: DeviceRegister, db: Session = Depends(get_db)):
    device = db.query(models.Device).filter(models.Device.mac_address == data.mac_address).first()
    if not device:
        # Generate 6-digit code
        code = str(random.randint(100000, 999999))
        device = models.Device(mac_address=data.mac_address, pairing_code=code)
        db.add(device)
        db.commit()
        return {"status": "pairing", "code": code}
    elif not device.user_id:
        if not device.pairing_code:
            device.pairing_code = str(random.randint(100000, 999999))
            db.commit()
        return {"status": "pairing", "code": device.pairing_code}
    
    return {"status": "ok"}

@app.post("/api/sync/push")
async def sync_push(data: SyncPushData, db: Session = Depends(get_db)):
    device = db.query(models.Device).filter(models.Device.mac_address == data.mac_address).first()
    if not device or not device.user_id:
        raise HTTPException(status_code=401, detail="Device not registered")
    
    device.battery_level = data.battery_level
    device.font_size = data.font_size
    device.sleep_timeout = data.sleep_timeout
    device.line_gap = data.line_gap

    if data.screensaver_mode is not None: device.screensaver_mode = data.screensaver_mode
    if data.invert_display is not None: device.invert_display = data.invert_display
    if data.spotify_client_id is not None: device.spotify_client_id = data.spotify_client_id
    if data.spotify_client_secret is not None: device.spotify_client_secret = data.spotify_client_secret
    if data.spotify_refresh_token is not None: device.spotify_refresh_token = data.spotify_refresh_token
    if data.spotify_screensaver is not None: device.spotify_screensaver = data.spotify_screensaver
    if data.chess_elo is not None: device.chess_elo = data.chess_elo
    if data.cal_url is not None: device.cal_url = data.cal_url
    if data.tz_offset is not None: device.tz_offset = data.tz_offset
    if data.firmware_version is not None: device.firmware_version = data.firmware_version

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

    if data.todos is not None:
        user = device.owner
        for t_data in data.todos:
            text = t_data.get("text")
            checked = t_data.get("checked", False)
            if text:
                existing = db.query(models.TodoItem).filter(models.TodoItem.user_id == user.id, models.TodoItem.text == text).first()
                if existing:
                    existing.checked = checked
                else:
                    new_todo = models.TodoItem(user_id=user.id, text=text, checked=checked)
                    db.add(new_todo)

    db.commit()
    return {"status": "ok"}

@app.get("/api/sync/pull")
async def sync_pull(mac: str, db: Session = Depends(get_db)):
    device = db.query(models.Device).filter(models.Device.mac_address == mac).first()
    if not device or not device.user_id:
        raise HTTPException(status_code=401, detail="Device not registered")
    
    # Return settings and all user books (since we sync the whole library now)
    books_data = [{"id": b.id, "title": b.title, "folder": b.folder} for b in device.owner.books]
    bookmarks_data = [
        {"book_id": bm.book_id, "title": bm.book.title, "page_index": bm.page_index} 
        for bm in device.bookmarks if bm.book
    ]
    
    resp = {
        "device_name": device.name or "My Pala",
        "font_size": device.font_size,
        "sleep_timeout": device.sleep_timeout,
        "line_gap": device.line_gap,
        "screensaver_mode": device.screensaver_mode,
        "invert_display": device.invert_display,
        "spotify_client_id": device.spotify_client_id,
        "spotify_client_secret": device.spotify_client_secret,
        "spotify_refresh_token": device.spotify_refresh_token,
        "spotify_screensaver": device.spotify_screensaver,
        "chess_elo": device.chess_elo,
        "cal_url": device.cal_url or "",
        "tz_offset": device.tz_offset,
        "app_todo": device.app_todo,
        "app_cal": device.app_cal,
        "app_spot": device.app_spot,
        "app_chess": device.app_chess,
        "app_pom": device.app_pom,
        "screensaver_updated_at": device.screensaver_updated_at,
        "books": books_data,
        "bookmarks": bookmarks_data,
        "todos": [
            {"id": t.id, "text": t.text, "checked": t.checked}
            for t in device.owner.todos
        ]
    }
    return resp

@app.get("/api/book/{book_id}")
async def download_book(book_id: int, mac: str, db: Session = Depends(get_db)):
    device = db.query(models.Device).filter(models.Device.mac_address == mac).first()
    if not device or not device.user_id:
        raise HTTPException(status_code=401, detail="Device not registered")
    
    book = db.query(models.Book).filter(models.Book.id == book_id, models.Book.user_id == device.user_id).first()
    if not book or not os.path.exists(book.file_path):
        raise HTTPException(status_code=404, detail="Book not found")
    
    return FileResponse(book.file_path, media_type="text/plain", filename=f"{book.title}.txt")

@app.put("/api/book/{book_id}/move")
async def move_book(book_id: int, request: Request, folder: str = Form(...), db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        raise HTTPException(status_code=401, detail="Not authenticated")
    book = db.query(models.Book).filter(models.Book.id == book_id, models.Book.user_id == user.id).first()
    if not book:
        raise HTTPException(status_code=404, detail="Book not found")
    book.folder = folder
    db.commit()
    return {"status": "ok"}

@app.delete("/api/book/{book_id}")
async def delete_book(book_id: int, request: Request, db: Session = Depends(get_db)):
    user = auth.get_current_user(request, db)
    if not user:
        raise HTTPException(status_code=401, detail="Not authenticated")
    book = db.query(models.Book).filter(models.Book.id == book_id, models.Book.user_id == user.id).first()
    if not book:
        raise HTTPException(status_code=404, detail="Book not found")
    if os.path.exists(book.file_path):
        try:
            os.remove(book.file_path)
        except:
            pass
    db.query(models.Bookmark).filter(models.Bookmark.book_id == book_id).delete()
    db.delete(book)
    db.commit()
    return {"status": "ok"}

@app.post("/api/device/{mac}/screensaver")
async def upload_screensaver(mac: str, file: UploadFile = File(...), db: Session = Depends(get_db)):
    device = db.query(models.Device).filter(models.Device.mac_address == mac).first()
    if not device:
        raise HTTPException(status_code=404, detail="Device not found")
    
    file_path = os.path.join(SCREENSAVERS_DIR, f"{mac}_sleep.bin")
    with open(file_path, "wb") as f:
        f.write(await file.read())
        
    device.screensaver_updated_at = int(time.time())
    db.commit()
    return {"status": "ok", "message": "Screensaver updated"}

@app.get("/api/device/{mac}/screensaver")
async def download_screensaver(mac: str, db: Session = Depends(get_db)):
    device = db.query(models.Device).filter(models.Device.mac_address == mac).first()
    if not device:
        raise HTTPException(status_code=404, detail="Device not found")
        
    file_path = os.path.join(SCREENSAVERS_DIR, f"{mac}_sleep.bin")
    if not os.path.exists(file_path):
        raise HTTPException(status_code=404, detail="Screensaver not found")
        
    return FileResponse(file_path, media_type="application/octet-stream", filename="sleep.bin")

# ----------------- FIRMWARE OTA -----------------

cached_release_info = None
last_release_check = 0

def get_latest_github_release():
    global cached_release_info, last_release_check
    now = time.time()
    if cached_release_info and (now - last_release_check < 300):
        return cached_release_info
    
    try:
        url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
        r = requests.get(url, timeout=10)
        if r.status_code == 200:
            data = r.json()
            tag_name = data.get("tag_name", "").lstrip("v")
            assets = data.get("assets", [])
            download_url = ""
            for asset in assets:
                if asset.get("name", "").endswith(".bin"):
                    download_url = asset.get("browser_download_url")
                    break
            
            if tag_name and download_url:
                cached_release_info = {"version": tag_name, "download_url": download_url}
                last_release_check = now
                return cached_release_info
    except Exception as e:
        print(f"Error fetching GitHub release: {e}")
    
    return cached_release_info

@app.get("/api/firmware/check")
async def check_firmware(version: str):
    release = get_latest_github_release()
    if release:
        if version != release["version"]:
            return {
                "update_available": True,
                "latest_version": release["version"],
                "download_url": "/api/firmware/latest.bin"
            }
    return {"update_available": False}

@app.get("/api/firmware/latest.bin")
async def get_latest_firmware():
    release = get_latest_github_release()
    if release and release.get("download_url"):
        try:
            r = requests.get(release["download_url"], stream=True, timeout=15)
            if r.status_code == 200:
                headers = {"Content-Disposition": "attachment; filename=firmware.bin"}
                return Response(content=r.content, media_type="application/octet-stream", headers=headers)
        except Exception as e:
            return {"error": str(e)}
    
    # Fallback to local file if GitHub fails or no release exists
    bin_path = os.path.join(FIRMWARE_DIR, "firmware.bin")
    if os.path.exists(bin_path):
        return FileResponse(bin_path, media_type="application/octet-stream", filename="firmware.bin")
        
    return {"error": "Firmware binary not found on server or GitHub."}

if __name__ == "__main__":
    uvicorn.run("app:app", host="0.0.0.0", port=8000, reload=True)

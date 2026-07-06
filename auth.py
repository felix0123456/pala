import secrets
from fastapi import Request, Depends, HTTPException, status
from sqlalchemy.orm import Session
from passlib.context import CryptContext
import models
from database import get_db

pwd_context = CryptContext(schemes=["bcrypt"], deprecated="auto")

# Simple in-memory session store for cookie auth
sessions = {}

def verify_password(plain_password, hashed_password):
    return pwd_context.verify(plain_password, hashed_password)

def get_password_hash(password):
    return pwd_context.hash(password)

def create_session(user_id: int):
    token = secrets.token_urlsafe(32)
    sessions[token] = user_id
    return token

def get_current_user(request: Request, db: Session = Depends(get_db)):
    token = request.cookies.get("session_token")
    if not token or token not in sessions:
        return None
    user_id = sessions[token]
    user = db.query(models.User).filter(models.User.id == user_id).first()
    return user

def get_current_user_api(device_mac: str, db: Session = Depends(get_db)):
    # For API endpoints from the ESP32, the ESP32 will send its MAC address
    device = db.query(models.Device).filter(models.Device.mac_address == device_mac).first()
    if not device:
        raise HTTPException(status_code=401, detail="Device not registered")
    return device.owner

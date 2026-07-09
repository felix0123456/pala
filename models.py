from sqlalchemy import Boolean, Column, ForeignKey, Integer, String, Table
from sqlalchemy.orm import relationship
from database import Base

device_books = Table(
    "device_books", Base.metadata,
    Column("device_mac", String, ForeignKey("devices.mac_address", ondelete="CASCADE"), primary_key=True),
    Column("book_id", Integer, ForeignKey("books.id", ondelete="CASCADE"), primary_key=True)
)
class User(Base):
    __tablename__ = "users"

    id = Column(Integer, primary_key=True, index=True)
    username = Column(String, unique=True, index=True)
    hashed_password = Column(String)
    is_admin = Column(Boolean, default=False)

    devices = relationship("Device", back_populates="owner")
    books = relationship("Book", back_populates="owner")
    todos = relationship("TodoItem", back_populates="owner", cascade="all, delete-orphan")

class Device(Base):
    __tablename__ = "devices"

    mac_address = Column(String, primary_key=True, index=True)
    name = Column(String, default="My Pala")
    user_id = Column(Integer, ForeignKey("users.id"), nullable=True)
    pairing_code = Column(String, index=True, nullable=True)
    battery_level = Column(Integer, default=100)
    firmware_version = Column(String, nullable=True)
    
    font_size = Column(Integer, default=10)
    sleep_timeout = Column(Integer, default=120)
    line_gap = Column(Integer, default=0)
    
    screensaver_mode = Column(Integer, default=0)
    invert_display = Column(Boolean, default=False)
    spotify_client_id = Column(String, nullable=True)
    spotify_client_secret = Column(String, nullable=True)
    spotify_refresh_token = Column(String, nullable=True)
    spotify_screensaver = Column(Boolean, default=False)
    chess_elo = Column(Integer, nullable=True)
    cal_url = Column(String, nullable=True)
    tz_offset = Column(Integer, default=0)
    screensaver_updated_at = Column(Integer, default=0)
    
    app_todo = Column(Boolean, default=True)
    app_cal = Column(Boolean, default=True)
    app_spot = Column(Boolean, default=True)
    app_chess = Column(Boolean, default=True)
    app_pom = Column(Boolean, default=True)

    owner = relationship("User", back_populates="devices")
    bookmarks = relationship("Bookmark", back_populates="device")
    synced_books = relationship("Book", secondary=device_books, back_populates="synced_devices")

class Book(Base):
    __tablename__ = "books"

    id = Column(Integer, primary_key=True, index=True)
    title = Column(String, index=True)
    file_path = Column(String)
    user_id = Column(Integer, ForeignKey("users.id"))

    owner = relationship("User", back_populates="books")
    bookmarks = relationship("Bookmark", back_populates="book")
    synced_devices = relationship("Device", secondary=device_books, back_populates="synced_books")

class Bookmark(Base):
    __tablename__ = "bookmarks"

    id = Column(Integer, primary_key=True, index=True)
    device_mac = Column(String, ForeignKey("devices.mac_address"))
    book_id = Column(Integer, ForeignKey("books.id"))
    page_index = Column(Integer, default=0)

    device = relationship("Device", back_populates="bookmarks")
    book = relationship("Book", back_populates="bookmarks")

class TodoItem(Base):
    __tablename__ = "todos"

    id = Column(Integer, primary_key=True, index=True)
    user_id = Column(Integer, ForeignKey("users.id"))
    text = Column(String)
    checked = Column(Boolean, default=False)

    owner = relationship("User", back_populates="todos")

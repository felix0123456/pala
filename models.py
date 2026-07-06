from sqlalchemy import Boolean, Column, ForeignKey, Integer, String
from sqlalchemy.orm import relationship
from database import Base

class User(Base):
    __tablename__ = "users"

    id = Column(Integer, primary_key=True, index=True)
    username = Column(String, unique=True, index=True)
    hashed_password = Column(String)

    devices = relationship("Device", back_populates="owner")
    books = relationship("Book", back_populates="owner")

class Device(Base):
    __tablename__ = "devices"

    mac_address = Column(String, primary_key=True, index=True)
    name = Column(String, default="My Pala")
    user_id = Column(Integer, ForeignKey("users.id"))
    battery_level = Column(Integer, default=100)
    
    font_size = Column(Integer, default=10)
    sleep_timeout = Column(Integer, default=120)
    line_gap = Column(Integer, default=0)

    owner = relationship("User", back_populates="devices")
    bookmarks = relationship("Bookmark", back_populates="device")

class Book(Base):
    __tablename__ = "books"

    id = Column(Integer, primary_key=True, index=True)
    title = Column(String, index=True)
    file_path = Column(String)
    user_id = Column(Integer, ForeignKey("users.id"))

    owner = relationship("User", back_populates="books")
    bookmarks = relationship("Bookmark", back_populates="book")

class Bookmark(Base):
    __tablename__ = "bookmarks"

    id = Column(Integer, primary_key=True, index=True)
    device_mac = Column(String, ForeignKey("devices.mac_address"))
    book_id = Column(Integer, ForeignKey("books.id"))
    page_index = Column(Integer, default=0)

    device = relationship("Device", back_populates="bookmarks")
    book = relationship("Book", back_populates="bookmarks")

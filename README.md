# Pala Cloud Hub (Backend Server)

Pala Cloud Hub is the centralized backend ecosystem for the Pala E-reader. It replaces the local web portal on the ESP32 with a powerful, database-backed web application and REST API.

## Core Features
1. **Cloud Sync:** Bidirectional synchronization between the Pala device and the server.
2. **Centralized Settings:** Manage your E-reader's font size, sleep timeouts, and line gaps from a beautiful web dashboard.
3. **Over-The-Air (OTA) Updates:** Deliver new `.bin` firmware updates directly to the device.
4. **E-Book Fetcher:** Search Project Gutenberg and instantly stage books for download to the device.

---

## 🏗 Architecture & Structure

### Technology Stack
- **Framework:** FastAPI (Python)
- **Database:** SQLite (via SQLAlchemy ORM)
- **Authentication:** Cookie-based Sessions with bcrypt password hashing (`passlib`)
- **Frontend:** Server-side rendered HTML using Jinja2 (`fastapi.templating`)

### File Structure
- `app.py`: The main FastAPI application. Contains the web routes, API endpoints, and the server-side rendering logic.
- `database.py`: Handles the SQLite connection and provides the `get_db()` dependency.
- `models.py`: Defines the SQLAlchemy database schema (User, Device, Book, Bookmark).
- `auth.py`: Manages password hashing, verification, and the in-memory session store.
- `templates/`: Contains the Jinja2 HTML files (`base.html`, `index.html`, `login.html`, `register.html`).
- `firmware/build/`: Directory where OTA firmware `.bin` files are staged.
- `books/`: Directory where fetched/uploaded `.txt` books are stored.

---

## 🔒 Authentication System
The backend uses a standard cookie-based authentication system for the web frontend.
1. A user logs in via `/login` (POST).
2. The server verifies the bcrypt hash and generates a secure, 32-byte session token.
3. The token is sent to the browser via an `httponly` cookie named `session_token`.
4. The `get_current_user` dependency intercepts this cookie on protected routes (like the `/` dashboard) to load the user's library and devices.

---

## 🔄 The Sync Protocol (ESP32 API)

The Pala ESP32 operates as a "Smart Client". When the user presses the buttons to enter Upload Mode, the ESP32 connects to their home WiFi and executes the following sequence:

### 1. Device Registration (`POST /api/device/register`)
- **Payload:** `{"mac_address": "AA:BB:CC:DD:EE:FF", "user_id": 1}`
- **Purpose:** Ensures the device exists in the database and is bound to a user.

### 2. Push State (`POST /api/sync/push`)
- **Payload:** `{"mac_address": "...", "battery_level": 95, "font_size": 10, "sleep_timeout": 120, "line_gap": 1, "bookmarks": [...]}`
- **Purpose:** The ESP32 uploads its current battery percentage, any local settings it might have, and its latest reading progress (bookmarks) to the server.

### 3. Pull State (`GET /api/sync/pull?mac_address=...`)
- **Response:** `{"font_size": 12, "sleep_timeout": 300, "line_gap": 2, "books": [{"id": 1, "title": "Alice"}]}`
- **Purpose:** The server returns any settings that the user modified via the Web Dashboard. The ESP32 overrides its local settings with these cloud settings.

### 4. Fetch Books (`GET /api/book/{book_id}?mac_address=...`)
- **Purpose:** Downloads the raw `.txt` file for any new books added to the user's library.

### 5. Check OTA Firmware (`GET /api/firmware/check?version=1.5.0`)
- **Purpose:** Compares the ESP32's current firmware version with `CURRENT_LATEST_VERSION` on the server. If an update is available, the ESP32 can download the `.bin` via `/api/firmware/latest.bin`.

---

## 🚀 How to Run

1. **Install Dependencies:**
   ```bash
   pip install -r requirements.txt
   ```

2. **Start the Server:**
   ```bash
   uvicorn app:app --host 0.0.0.0 --port 8000 --reload
   ```

3. **Access the Dashboard:**
   Open your browser and navigate to `http://localhost:8000` (or the IP of the server on your network).

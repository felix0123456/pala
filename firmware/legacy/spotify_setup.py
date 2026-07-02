#!/usr/bin/env python3
import sys
import os
import json
import base64
import urllib.request
import urllib.parse
import webbrowser
from http.server import HTTPServer, BaseHTTPRequestHandler
import socket

# Local server config
REDIRECT_PORT = 8888
REDIRECT_URI = f"http://127.0.0.1:{REDIRECT_PORT}/callback"

class CallbackHandler(BaseHTTPRequestHandler):
    """Handles the redirect from Spotify authorization page."""
    
    def log_message(self, format, *args):
        # Suppress logging to console for cleaner CLI output
        return

    def do_GET(self):
        # Parse query parameters from redirect URL
        parsed_url = urllib.parse.urlparse(self.path)
        if parsed_url.path == "/callback":
            query_params = urllib.parse.parse_qs(parsed_url.query)
            
            if "code" in query_params:
                # Store the authorization code in the server instance
                self.server.auth_code = query_params["code"][0]
                
                # Send confirmation HTML page to browser
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                
                html = """
                <!DOCTYPE html>
                <html>
                <head>
                    <title>PalaOne Auth Success</title>
                    <style>
                        body {
                            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
                            background-color: #121212;
                            color: #ffffff;
                            text-align: center;
                            padding: 50px;
                        }
                        .container {
                            background-color: #1e1e1e;
                            border-radius: 12px;
                            padding: 40px;
                            display: inline-block;
                            box-shadow: 0 4px 12px rgba(0,0,0,0.5);
                        }
                        h1 { color: #1DB954; margin-bottom: 20px; }
                        p { font-size: 1.1em; color: #b3b3b3; }
                    </style>
                </head>
                <body>
                    <div class="container">
                        <h1>Authorization Successful!</h1>
                        <p>You have successfully authorized PalaOne E-Reader with Spotify.</p>
                        <p>You can close this tab and return to the command prompt.</p>
                    </div>
                </body>
                </html>
                """
                self.wfile.write(html.encode("utf-8"))
            else:
                error = query_params.get("error", ["Unknown error"])[0]
                self.send_response(400)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(f"<h1>Authorization Failed</h1><p>{error}</p>".encode("utf-8"))
        else:
            self.send_response(404)
            self.end_headers()

def run_local_server():
    """Starts the temporary HTTP server on REDIRECT_PORT."""
    server = HTTPServer(("127.0.0.1", REDIRECT_PORT), CallbackHandler)
    server.auth_code = None
    return server

def request_refresh_token(client_id, client_secret, code):
    """Exchanges the authorization code for a Spotify refresh token."""
    token_url = "https://accounts.spotify.com/api/token"
    
    # Request headers & body
    auth_str = f"{client_id}:{client_secret}"
    auth_base64 = base64.b64encode(auth_str.encode("utf-8")).decode("utf-8")
    
    headers = {
        "Authorization": f"Basic {auth_base64}",
        "Content-Type": "application/x-www-form-urlencoded"
    }
    
    data = urllib.parse.urlencode({
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": REDIRECT_URI
    }).encode("utf-8")
    
    req = urllib.request.Request(token_url, data=data, headers=headers, method="POST")
    
    try:
        with urllib.request.urlopen(req) as response:
            res_data = json.loads(response.read().decode("utf-8"))
            return res_data.get("refresh_token")
    except urllib.error.HTTPError as e:
        print(f"\n[Error] Failed to exchange code: {e.read().decode('utf-8')}")
        return None
    except Exception as e:
        print(f"\n[Error] Connection failed: {e}")
        return None

def upload_to_ereader(reader_ip, client_id, client_secret, refresh_token):
    """Sends the configured settings directly to the E-Reader's /settings POST endpoint."""
    url = f"http://{reader_ip}/settings"
    
    # Send configuration data as application/x-www-form-urlencoded
    data = urllib.parse.urlencode({
        "spot_id": client_id,
        "spot_secret": client_secret,
        "spot_refresh": refresh_token,
        "spot_scr": "on"
    }).encode("utf-8")
    
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    
    try:
        print(f"Connecting to E-Reader at {url}...")
        with urllib.request.urlopen(req, timeout=10) as response:
            if response.status == 200:
                return True
    except Exception as e:
        print(f"[Warning] Failed to push directly to E-Reader: {e}")
    return False

def main():
    print("=" * 60)
    print("           PalaOne E-Reader Spotify Auto-Setup Tool")
    print("=" * 60)
    print("\nInstructions:")
    print("1. Log in to the Spotify Developer Dashboard:")
    print("   https://developer.spotify.com/dashboard")
    print("2. Click 'Create App' button.")
    print("3. Enter 'PalaOne' as App Name and Description.")
    print("4. Set 'Redirect URIs' to exactly: http://127.0.0.1:8888/callback")
    print("5. Check the terms checkbox and click 'Save'.")
    print("6. Go to the App Settings and copy the 'Client ID' & 'Client Secret'.")
    print("-" * 60)
    
    # 1. Inputs
    client_id = input("\nEnter Spotify Client ID: ").strip()
    if not client_id:
        print("Client ID is required.")
        return
        
    client_secret = input("Enter Spotify Client Secret: ").strip()
    if not client_secret:
        print("Client Secret is required.")
        return
        
    default_ip = "192.168.4.1"
    reader_ip = input(f"Enter E-Reader IP or hostname [{default_ip}]: ").strip()
    if not reader_ip:
        reader_ip = default_ip
        
    # 2. Spin up local HTTP redirect listener
    try:
        server = run_local_server()
    except OSError as e:
        if e.errno == 98 or e.errno == 10048:
            print(f"\n[Error] Port {REDIRECT_PORT} is already in use by another application.")
            print("Please close any process using this port and run the script again.")
        else:
            print(f"\n[Error] Failed to start local server: {e}")
        return
        
    # 3. Open browser authorization page
    auth_url = (
        "https://accounts.spotify.com/authorize?"
        + urllib.parse.urlencode({
            "response_type": "code",
            "client_id": client_id,
            "scope": "user-read-currently-playing user-modify-playback-state",
            "redirect_uri": REDIRECT_URI
        })
    )
    
    print("\nStarting local listener and opening your browser...")
    webbrowser.open(auth_url)
    print("Waiting for Spotify authorization code redirect in browser...")
    
    # Handle the redirected request
    server.handle_request()
    server.server_close()
    
    if not server.auth_code:
        print("[Error] Failed to capture authorization code from callback redirect.")
        return
        
    # 4. Exchange Auth Code for Refresh Token
    print("Received callback authorization code. Requesting refresh token...")
    refresh_token = request_refresh_token(client_id, client_secret, server.auth_code)
    
    if not refresh_token:
        print("[Error] Could not retrieve Spotify refresh token.")
        return
        
    print(f"\n[Success] Obtained Spotify Refresh Token: {refresh_token[:15]}...")
    
    # 5. Push config to E-Reader
    success = upload_to_ereader(reader_ip, client_id, client_secret, refresh_token)
    
    print("\n" + "=" * 60)
    if success:
        print("🎉 Spotify credentials configured on PalaOne successfully!")
    else:
        print("Spotify credentials retrieved successfully, but E-Reader was not found.")
        print("Please configure them manually in your PalaOne browser dashboard:")
        print(f"  Spotify Client ID:     {client_id}")
        print(f"  Spotify Client Secret: {client_secret}")
        print(f"  Spotify Refresh Token: {refresh_token}")
    print("=" * 60)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nOperation cancelled by user.")
        sys.exit(0)

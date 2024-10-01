from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import json
import os
import re
import signal
import socket
import threading
import time
from urllib.parse import urlparse, parse_qs
from db_mysql import get_files, get_tags

api = None
cameraTracker = None
cameras = {}

# TODO: move this elsewhere
fp = open('../config.json', 'r')
config = json.load(fp)


class PimeraRequestHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        url = urlparse(self.path)
        query = parse_qs(url.query)
        for k,v in query.items():
            query[k] = ",".join(v)

        if self.path == "/" or self.path == "/index.html":
            self.pimera_index()
        elif self.path == "/api/cameras.json":
            self.pimera_cameras()
        elif self.path.startswith("/api/tags"):
            self.pimera_apitags(url, query)
        elif self.path.startswith("/api/files"):
            self.pimera_apifiles(url, query)
        elif self.path.startswith("/movies"):
            self.pimera_movies(url)
        elif self.path.startswith("/images"):
            self.pimera_images(url)
        else:
            self.pimera_404()
    
    def do_POST(self):
        url = urlparse(self.path)
        query = parse_qs(url.query)
        for k,v in query.items():
            query[k] = ",".join(v)

        if self.path.startswith("/api/image/"):
            self.pimera_save_image(url, query)
        else:
            self.pimera_404()
    
    def pimera_index(self):
        self.send_response(200)
        self.send_header('Content-type', 'text/html')
        self.end_headers()

        with open("public/index.html") as f:
            self.wfile.write( f.read().encode("utf8") )

    def pimera_cameras(self):
        global cameras
        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.end_headers()

        # prune old cameras
        cutoff = time.time() - 10.0
        for key in cameras:
            ts = cameras[key]
            if ts < cutoff:
                print('removing %s' % key)
                del cameras[key]
        self.wfile.write( json.dumps(cameras).encode("utf8") )
    
    def pimera_apitags(self, url, query):
        out = get_tags(query)

        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.end_headers()
        self.wfile.write( json.dumps(out).encode("utf8") )
    
    def pimera_apifiles(self, url, query):
        out = get_files(query)
        #print(out)

        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.end_headers()
        self.wfile.write( json.dumps(out).encode("utf8") )
    
    def pimera_movies(self, url):
        p = url.path.replace("..", "")
        video_file = config["videoPath"] + p.replace("/movies", "")
        # print("Sending video", video_file)
        try:
            sz = os.path.getsize(video_file)
        except e:
            self.pimera_404()
            return

        self.send_response(200)
        self.send_header('Content-type', 'video/mp4')
        self.send_header('Content-length', sz)
        self.end_headers()

        with open(video_file, "rb") as f:
            self.wfile.write( f.read() )
    

    def pimera_images(self, url):
        global config
        p = url.path.replace("..", "")
        video_file = config["videoPath"] + p.replace("/images", "")
        #print("Sending image", video_file)
        try:
            sz = os.path.getsize(video_file)
        except Exception as e:
            self.pimera_404()
            return

        self.send_response(200)
        self.send_header('Content-type', 'image/jpeg')
        self.send_header('Content-length', sz)
        self.end_headers()

        with open(video_file, "rb") as f:
            self.wfile.write( f.read() )

    def pimera_save_image(self, url, query):
        global config
        # Bail if content-type not image/jpeg
        if self.headers.get("content-type") != "image/jpeg":
            print("Invalid headers")
            return self.pimera_500()

        length = int(self.headers.get('content-length'))
        data = self.rfile.read(length)

        # Bail if we were sent more than 5000000 bytes
        print("Content length", length)
        if length > 10000000:
            print("File too large")
            return self.pimera_500()

        # Try to sanitize path a bit
        destination_path = url.path.replace("/api/image", "").replace("..", "")
        print("destination_path", destination_path)

        # Make sure path matches expected pattern
        pattern = re.compile("^\/[0-9]{8}\/[^.]+\.jpg$")
        if pattern.match(destination_path) is None:
            return self.pimera_500()

        final_path = config["videoPath"] + destination_path
        print("Saving to ", final_path)

        # Save file to disk
        with open(final_path, "wb") as f:
            f.write(data)

        self.send_response(200)
        self.send_header('Content-type', 'text/plain')
        self.end_headers()
        self.wfile.write("OK".encode("utf8"))
    
    def pimera_404(self):
        self.send_response(404)
        self.send_header('Content-type', 'text/plain')
        self.end_headers()
        self.wfile.write("404 Not Found".encode("utf8"))
    def pimera_500(self):
        self.send_response(500)
        self.send_header('Content-type', 'text/plain')
        self.end_headers()
        self.wfile.write("500 No thanks".encode("utf8"))


class PimeraAPI(threading.Thread):
    def run(self):
        print("PimeraAPI starting")
        server_class = ThreadingHTTPServer
        handler_class = PimeraRequestHandler

        server_address = ('', 8000)
        # This needs to happen in another thread
        self.pimera_httpd = server_class(server_address, handler_class)
        self.pimera_httpd.serve_forever()
    
    def stop(self):
        # is this going to work?
        print("Stopping PimeraAPI")
        self.pimera_httpd.shutdown()

class CameraTracker(threading.Thread):
    def __init__(self):
        threading.Thread.__init__(self)
        self.running = True
        self.start()

    def run(self):
        global cameras
        print("CameraTracker starting")
        UDP_IP = "0.0.0.0"
        UDP_PORT = 5001
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((UDP_IP, UDP_PORT))
        # Set non-blocking so we can check running occasionally
        sock.settimeout(1.0)

        while self.running:
            try:
                data, addr = sock.recvfrom(1024) # buffer size is 1024 bytes
                if data:
                    print("Camera heartbeat %s" % addr[0])
                    cameras[ addr[0] ] = time.time()
            except TimeoutError:
                continue


def signal_handler(sig, frame):
    global cameraTracker
    global api
    print('Exiting ...')
    cameraTracker.running = False
    api.stop()

signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)

cameraTracker = CameraTracker()
api = PimeraAPI()
api.start()


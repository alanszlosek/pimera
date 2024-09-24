from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import json
import signal
import socket
import threading
import time

api = None
cameraTracker = None
cameras = {}

class PimeraRequestHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        global cameras

        if self.path == "/" or self.path == "/index.html":
            self.pimera_index()
        elif self.path == "/api/cameras.json":
            self.pimera_cameras()
        elif self.path.startswith("/api/tags"):
            self.pimera_tags()
        else:
            self.pimera_404()
    
    def pimera_index(self):
        self.send_response(200)
        self.send_header('Content-type', 'text/html')
        self.end_headers()

        with open("public/index.html") as f:
            self.wfile.write( f.read().encode("utf8") )

    def pimera_cameras(self):
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
    
    def pimera_tags(self):
        global api
        noop

    
    def pimera_404(self):
        self.send_response(404)
        self.send_header('Content-type', 'text/plain')
        self.end_headers()
        self.wfile.write("404 Not Found".encode("utf8"))


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
                    #print("Camera heartbeat %s" % addr[0])
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

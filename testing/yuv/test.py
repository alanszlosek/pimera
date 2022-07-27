# from: https://stackoverflow.com/questions/2231518/how-to-read-a-frame-from-yuv-file-in-opencv
import cv2
import numpy as np

# given yuv420 still from libcamera-still, this converts to rgb, then to jpeg

class VideoCaptureYUV:
    def __init__(self, filename, size):
        self.height, self.width = size
        self.frame_len = int(self.width * self.height * 1.5)
        self.f = open(filename, 'rb')
        t = int(self.width / 2) * int(self.height / 2)
        self.shape = (int(self.height * 1.5), self.width)

    def read_raw(self):
        try:
            print("Reading %d" % self.frame_len)
            raw = self.f.read(self.frame_len)
            yuv = np.frombuffer(raw, dtype=np.uint8)
            yuv = yuv.reshape(self.shape)
        except Exception as e:
            print(str(e))
            return False, None
        return True, yuv

    def read(self):
        ret, yuv = self.read_raw()
        if not ret:
            return ret, yuv
        bgr = cv2.cvtColor(yuv, cv2.COLOR_YUV420p2BGR)
        return ret, bgr


if __name__ == "__main__":
    filename = "/home/pi/temp/test.yuv"
    size = (1080, 1920)
    cap = VideoCaptureYUV(filename, size)

    while 1:
        ret, frame = cap.read()
        if ret:
            cv2.imwrite("yuv.jpeg", frame)
            #cv2.imshow("frame", frame)
            #cv2.waitKey(30)
            break
        else:
            break

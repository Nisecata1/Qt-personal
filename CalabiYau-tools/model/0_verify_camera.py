import cv2
import time

# Manual camera config: set these values yourself.
CAMERA_INDEX = 0  # e.g. 0/1/2
CAMERA_BACKEND = "DSHOW"  # DSHOW / MSMF / ANY
CAMERA_FOURCC = "YUY2"  # YUY2 / MJPG
CAMERA_WIDTH = 1920
CAMERA_HEIGHT = 1080
CAMERA_TARGET_FPS = 60


def configure_camera_capture(cap):
    f0, f1, f2, f3 = (CAMERA_FOURCC + "    ")[:4]
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(f0, f1, f2, f3))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, CAMERA_WIDTH)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, CAMERA_HEIGHT)
    cap.set(cv2.CAP_PROP_FPS, CAMERA_TARGET_FPS)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)


def get_camera_backend_value(name):
    backend_map = {
        "DSHOW": getattr(cv2, "CAP_DSHOW", None),
        "MSMF": getattr(cv2, "CAP_MSMF", None),
        "ANY": getattr(cv2, "CAP_ANY", None),
    }
    return backend_map.get(str(name).upper())


def get_codec_info(cap):
    fourcc = int(cap.get(cv2.CAP_PROP_FOURCC))
    codec = "".join([chr((fourcc >> (8 * i)) & 0xFF) for i in range(4)]).upper().strip()
    return fourcc, codec


def test_camera():
    print("Start camera verify (manual config)...")

    backend_name = str(CAMERA_BACKEND).upper()
    backend_value = get_camera_backend_value(backend_name)
    if backend_value is None:
        print(f"[ERR] Invalid CAMERA_BACKEND: {CAMERA_BACKEND}")
        print("Valid values: DSHOW / MSMF / ANY")
        return

    cap = cv2.VideoCapture(CAMERA_INDEX, backend_value)
    if not cap.isOpened():
        print(f"[ERR] Open failed: backend={backend_name}, index={CAMERA_INDEX}")
        print("Check if this index is your capture card (not DroidCamera).")
        return

    configure_camera_capture(cap)
    ret, _ = cap.read()
    if not ret:
        cap.release()
        print(f"[ERR] Read failed: backend={backend_name}, index={CAMERA_INDEX}")
        return

    actual_w = cap.get(cv2.CAP_PROP_FRAME_WIDTH)
    actual_h = cap.get(cv2.CAP_PROP_FRAME_HEIGHT)
    driver_fps = cap.get(cv2.CAP_PROP_FPS)
    fourcc, codec = get_codec_info(cap)

    print("----------------------------------------")
    print(f"Backend/Index: {backend_name}[{CAMERA_INDEX}]")
    print(f"Resolution: {actual_w} x {actual_h}")
    print(f"FourCC: {fourcc} -> '{codec}'")
    print(f"Target FPS: {CAMERA_TARGET_FPS} | Driver FPS: {driver_fps}")
    print("----------------------------------------")
    print("Press 'q' in window to quit")

    prev_time = 0.0
    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                print("[WARN] Frame read failed, stop verify")
                break

            current_time = time.time()
            fps = 1.0 / (current_time - prev_time) if prev_time > 0 else 0.0
            prev_time = current_time

            color = (0, 255, 0) if fps > 50 else (0, 0, 255)
            cv2.putText(
                frame,
                f"FPS: {fps:.1f} | {codec} | {backend_name}[{CAMERA_INDEX}]",
                (20, 60),
                cv2.FONT_HERSHEY_SIMPLEX,
                1.0,
                color,
                2,
            )
            cv2.imshow("Camera Latency Test", frame)

            if cv2.waitKey(1) & 0xFF == ord("q"):
                break
    finally:
        cap.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    test_camera()

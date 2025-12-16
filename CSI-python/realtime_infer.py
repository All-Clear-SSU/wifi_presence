import os, json, sys, numpy as np, paho.mqtt.client as mqtt
from collections import deque
import joblib

DEF_BROKER = "allclear.sytes.net"
DEF_PORT   = 4341
OUT_TOPIC  = "AllClear/1"

MODEL_PATH  = "outputs/model.pkl"
SCALER_PATH = "outputs/scaler.pkl"
OFFSET_PATH = "offset.npz"
CONFIG_PATH = "config.json"

SUBCARRIERS_RAW   = 43
INDICES_TO_REMOVE = list(range(26, 35))
PRED_WINDOW       = 10

def make_client():
    try:
        from paho.mqtt.client import CallbackAPIVersion
        return mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2)
    except Exception:
        return mqtt.Client()

def load_config(path):
    if not os.path.exists(path):
        print(f"[ERR] missing {path}")
        sys.exit(1)
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)

def parse_payload_to_vec(payload: str):
    line = None
    for ln in payload.split("\n"):
        if ln.strip().startswith("CSI values:"):
            line = ln.strip(); break
    if line is None: return None
    nums = [x for x in line.replace("CSI values:", "").strip().split() if x]
    try:
        arr = np.array([float(x) for x in nums], dtype=float)
    except ValueError:
        return None
    if arr.size % 2 != 0: return None
    re = arr[0::2]; im = arr[1::2]
    amp_full = np.sqrt(re**2 + im**2)
    amp = amp_full[:SUBCARRIERS_RAW]
    kept = [amp[i] for i in range(len(amp)) if i not in INDICES_TO_REMOVE]
    return np.asarray(kept, dtype=float)

def load_offset(path, dim):
    if not os.path.exists(path):
        print(f"[ERR] missing offset file: {path}")
        sys.exit(1)
    z = np.load(path)
    b = z.get("baseline")
    if b is None:
        print("[ERR] offset file has no 'baseline'")
        sys.exit(1)
    b = np.asarray(b, dtype=float).reshape(-1)
    if b.size != dim:
        print(f"[ERR] offset dim mismatch: got {b.size}, want {dim}")
        sys.exit(1)
    return b

def main():
    cfg = load_config(CONFIG_PATH)
    in_topic = cfg.get("publish_topic")
    if not in_topic:
        print("[ERR] missing 'publish_topic' in config.json")
        sys.exit(1)

    if not (os.path.exists(MODEL_PATH) and os.path.exists(SCALER_PATH)):
        print(f"[ERR] missing model/scaler: {MODEL_PATH}, {SCALER_PATH}")
        sys.exit(1)
    clf = joblib.load(MODEL_PATH)
    scaler = joblib.load(SCALER_PATH)

    feat_dim = SUBCARRIERS_RAW - len([i for i in range(SUBCARRIERS_RAW) if i in INDICES_TO_REMOVE])
    offset = load_offset(OFFSET_PATH, feat_dim)

    win = deque(maxlen=PRED_WINDOW)

    def on_connect(c, u, f, rc, p=None):
        if rc == 0:
            print(f"[OK] connect {DEF_BROKER}:{DEF_PORT}  sub={in_topic}  pub={OUT_TOPIC}")
            c.subscribe(in_topic)
        else:
            print(f"[ERR] connect rc={rc}")

    def publish_json(c, survivor, last_vec):
        out = {
            "sensor_id": 1,
            "survivor_detected": bool(survivor),
            "csi_amplitude_summary": [float(x) for x in last_vec.tolist()]
        }
        c.publish(OUT_TOPIC, json.dumps(out), qos=0, retain=False)

    def on_message(c, u, msg, p=None):
        try:
            vec = parse_payload_to_vec(msg.payload.decode("utf-8", errors="ignore"))
            if vec is None: return
            win.append(vec)
            if len(win) < PRED_WINDOW: return
            vec_mean = np.mean(np.stack(list(win), axis=0), axis=0)
            x = vec_mean - offset
            xs = scaler.transform(x.reshape(1, -1))
            y = int(clf.predict(xs)[0])
            publish_json(c, y == 1, vec)
            try:
                p1 = float(clf.predict_proba(xs)[0][1])
                print(f"[PRED] y={y} p1={p1:.3f}")
            except Exception:
                print(f"[PRED] y={y}")
        except Exception as e:
            print(f"[ERR] on_message: {e}")

    client = make_client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(DEF_BROKER, DEF_PORT, keepalive=60)
    client.loop_forever()

if __name__ == "__main__":
    main()
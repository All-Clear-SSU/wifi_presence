import os, time, json, argparse, numpy as np
import paho.mqtt.client as mqtt
from collections import deque
from datetime import datetime

DEF_BROKER = "allclear.sytes.net"
DEF_PORT   = 4341

SUBCARRIERS_RAW   = 43
INDICES_TO_REMOVE = list(range(26, 35))
STAB_WINDOW       = 100
STAB_CONSEC       = 10
RCV_TH            = 0.08
UNSTABLE_TOL      = 3
EMPTY_SECONDS     = 8
EMPTY_MIN_SAMPLES = 500

def make_client():
    try:
        from paho.mqtt.client import CallbackAPIVersion
        return mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2)
    except Exception:
        return mqtt.Client()

def load_config(path):
    if not path or not os.path.exists(path): return {}
    with open(path, "r", encoding="utf-8") as f:
        try: return json.load(f)
        except Exception: return {}

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

def robust_cv(window_np: np.ndarray) -> float:
    med = np.median(window_np, axis=0)
    mad = np.median(np.abs(window_np - med), axis=0)
    denom = np.abs(med) + 1e-6
    return float(np.median(mad / denom))

def collect_empty(c, topic, seconds, min_samples):
    stable_buf = deque(maxlen=STAB_WINDOW)
    collected = []
    state = "WAIT_STABLE"; consec = 0; unstable_hits = 0
    started = False; t0 = 0.0

    def on_msg(_c, _u, msg, *a, **k):
        nonlocal state, consec, unstable_hits, started, t0
        vec = parse_payload_to_vec(msg.payload.decode("utf-8", errors="ignore"))
        if vec is None: return
        stable_buf.append(vec)
        if len(stable_buf) < STAB_WINDOW:
            if len(stable_buf) % 20 == 0:
                print(f"[STAB] warming {len(stable_buf)}/{STAB_WINDOW}")
            return
        rcv = robust_cv(np.vstack(stable_buf))
        if state == "WAIT_STABLE":
            if rcv <= RCV_TH:
                consec += 1
                if consec % 5 == 0:
                    print(f"[STAB] stable {consec}/{STAB_CONSEC} (rcv={rcv:.4f})")
                if consec >= STAB_CONSEC:
                    state = "COLLECT"
                    started = True
                    t0 = time.time()
                    unstable_hits = 0
                    print(f"[EMPTY] start collect (rcv={rcv:.4f})")
            else:
                consec = 0
        elif state == "COLLECT":
            collected.append(vec)
            if rcv > RCV_TH:
                unstable_hits += 1
                print(f"[EMPTY] unstable {unstable_hits}/{UNSTABLE_TOL} (rcv={rcv:.4f})")
                if unstable_hits > UNSTABLE_TOL:
                    print("[EMPTY] reset to WAIT_STABLE")
                    state = "WAIT_STABLE"; consec = 0; collected.clear(); started=False; t0=0.0
                    unstable_hits = 0
                    return
            else:
                if unstable_hits: unstable_hits = 0
            if started and (time.time() - t0 >= seconds) and (len(collected) >= min_samples):
                c.loop_stop(); c.unsubscribe(topic)

    c.on_message = on_msg
    c.subscribe(topic)
    c.loop_start()
    while c._thread is not None: time.sleep(0.2)
    return np.vstack(collected) if collected else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="config.json")
    ap.add_argument("--broker", default=None)
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--topic", default=None)
    ap.add_argument("--out", default="offset.npz")
    ap.add_argument("--empty-seconds", type=int, default=EMPTY_SECONDS)
    ap.add_argument("--empty-min", type=int, default=EMPTY_MIN_SAMPLES)
    args = ap.parse_args()

    cfg = load_config(args.config)
    broker = args.broker or DEF_BROKER
    port   = args.port   or DEF_PORT
    topic  = args.topic  or cfg.get("publish_topic")
    if not topic: raise SystemExit("Missing in-topic: set --topic or config.publish_topic")

    client = make_client()
    client.on_connect = lambda c,u,f,rc,p=None: print(f"[MQTT] connect rc={rc} {broker}:{port}")
    client.connect(broker, port, keepalive=60)

    print("[STEP] Collecting EMPTY. Keep the area still.")
    empty_arr = collect_empty(client, topic, args.empty_seconds, args.empty_min)
    if empty_arr is None or empty_arr.shape[0] < args.empty_min:
        raise SystemExit("EMPTY collection failed or too few samples")

    baseline = np.median(empty_arr, axis=0).astype(np.float32)
    tmp = args.out + ".tmp"
    np.savez(tmp,
             baseline=baseline,
             timestamp=datetime.now().isoformat(),
             n_samples=int(empty_arr.shape[0]),
             subcarriers=int(baseline.size))
    os.replace(tmp, args.out)
    print(f"[SAVED] {args.out}  baseline_dim={baseline.size}  n={empty_arr.shape[0]}")

if __name__ == "__main__":
    main()
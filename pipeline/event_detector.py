# ==============================================================================
# EdgeVision-RK3588: Tripwire / Line-Crossing & Intrusion Event Detector
# ==============================================================================
import os
import json
from datetime import datetime


def ccw(A, B, C):
    return (C[1] - A[1]) * (B[0] - A[0]) > (B[1] - A[1]) * (C[0] - A[0])


def line_intersect(A, B, C, D):
    return ccw(A, C, D) != ccw(B, C, D) and ccw(A, B, C) != ccw(A, B, D)


class EventDetector:
    def __init__(self, log_path="outputs/events.log", tripwire_p1=(0, 280), tripwire_p2=(1280, 280)):
        self.log_path = log_path
        self.tripwire_p1 = tripwire_p1
        self.tripwire_p2 = tripwire_p2
        self.track_history = {}
        self.triggered_events = set()
        
        os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
        with open(self.log_path, "w", encoding="utf-8") as f:
            f.write(f"# EdgeVision Event Alert Log - Initialized {datetime.now().isoformat()}\n")

    def set_tripwire(self, p1, p2):
        self.tripwire_p1 = p1
        self.tripwire_p2 = p2

    def update(self, active_tracks, frame_id, timestamp=None):
        current_alerts = []
        if timestamp is None:
            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]

        current_track_ids = set()
        for track in active_tracks:
            tid = track.track_id
            current_track_ids.add(tid)
            tlbr = track.tlbr
            cx = (tlbr[0] + tlbr[2]) / 2.0
            cy = tlbr[3]
            curr_pos = (cx, cy)

            if tid in self.track_history:
                prev_pos = self.track_history[tid][-1]
                if tid not in self.triggered_events:
                    if line_intersect(prev_pos, curr_pos, self.tripwire_p1, self.tripwire_p2):
                        event = {
                            "timestamp": timestamp,
                            "frame_id": frame_id,
                            "event_type": "TRIPWIRE_CROSSING",
                            "track_id": tid,
                            "class_id": track.class_id,
                            "score": round(float(track.score), 3),
                            "direction": "FORWARD" if curr_pos[1] > prev_pos[1] else "BACKWARD",
                            "position": [round(cx, 1), round(cy, 1)]
                        }
                        current_alerts.append(event)
                        self.triggered_events.add(tid)
                        self._write_event_log(event)
                        print(f"\033[31m[EVENT DETECTED] Frame {frame_id}: Track #{tid} (Class {track.class_id}) crossed tripwire!\033[0m")

                self.track_history[tid].append(curr_pos)
            else:
                self.track_history[tid] = [curr_pos]

        for tid in list(self.track_history.keys()):
            if tid not in current_track_ids and len(self.track_history[tid]) > 100:
                del self.track_history[tid]

        return current_alerts

    def _write_event_log(self, event):
        with open(self.log_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(event) + "\n")

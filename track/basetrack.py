# ==============================================================================
# EdgeVision-RK3588: Track State and Base Class
# ==============================================================================
import numpy as np


class TrackState:
    New = 0
    Tracked = 1
    Lost = 2
    Removed = 3


class BaseTrack:
    _count = 0

    def __init__(self):
        self.track_id = 0
        self.is_activated = False
        self.state = TrackState.New
        self.history = []
        self.features = []
        self.curr_feature = None
        self.score = 0.0
        self.start_frame = 0
        self.frame_id = 0
        self.time_since_update = 0

    @classmethod
    def next_id(cls):
        cls._count += 1
        return cls._count

    @classmethod
    def reset_id_counter(cls):
        cls._count = 0

    def activate(self, *args):
        raise NotImplementedError

    def predict(self):
        raise NotImplementedError

    def update(self, *args, **kwargs):
        raise NotImplementedError

    def mark_lost(self):
        self.state = TrackState.Lost

    def mark_removed(self):
        self.state = TrackState.Removed

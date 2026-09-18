# ==============================================================================
# EdgeVision-RK3588: Pure NumPy ByteTrack Implementation
# Reference: ByteTrack: Multi-Object Tracking by Associating Every Detection Box (ECCV 2022)
# ==============================================================================
import numpy as np
from scipy.optimize import linear_sum_assignment
from .kalman_filter import KalmanFilter
from .basetrack import BaseTrack, TrackState


def bbox_iou(boxes1, boxes2):
    """
    Computes pairwise IoU between boxes1 [N, 4] and boxes2 [M, 4] (format: xyxy).
    """
    if len(boxes1) == 0 or len(boxes2) == 0:
        return np.zeros((len(boxes1), len(boxes2)), dtype=np.float32)

    boxes1 = np.expand_dims(boxes1, 1)  # [N, 1, 4]
    boxes2 = np.expand_dims(boxes2, 0)  # [1, M, 4]

    xx1 = np.maximum(boxes1[..., 0], boxes2[..., 0])
    yy1 = np.maximum(boxes1[..., 1], boxes2[..., 1])
    xx2 = np.minimum(boxes1[..., 2], boxes2[..., 2])
    yy2 = np.minimum(boxes1[..., 3], boxes2[..., 3])

    w = np.maximum(0.0, xx2 - xx1)
    h = np.maximum(0.0, yy2 - yy1)
    inter = w * h

    area1 = (boxes1[..., 2] - boxes1[..., 0]) * (boxes1[..., 3] - boxes1[..., 1])
    area2 = (boxes2[..., 2] - boxes2[..., 0]) * (boxes2[..., 3] - boxes2[..., 1])
    union = area1 + area2 - inter + 1e-6

    return inter / union


class STrack(BaseTrack):
    shared_kalman = KalmanFilter()

    def __init__(self, tlwh, score, class_id):
        super().__init__()
        self._tlwh = np.asarray(tlwh, dtype=np.float32)
        self.kalman_filter = None
        self.mean, self.covariance = None, None
        self.is_activated = False

        self.score = float(score)
        self.class_id = int(class_id)
        self.tracklet_len = 0

    def predict(self):
        mean_state = self.mean.copy()
        if self.state != TrackState.Tracked:
            mean_state[7] = 0
        self.mean, self.covariance = self.shared_kalman.predict(mean_state, self.covariance)

    def activate(self, kalman_filter, frame_id):
        self.kalman_filter = kalman_filter
        self.track_id = self.next_id()
        self.mean, self.covariance = self.kalman_filter.initiate(self.tlwh_to_xyah(self._tlwh))

        self.tracklet_len = 0
        self.state = TrackState.Tracked
        if frame_id == 1:
            self.is_activated = True
        self.frame_id = frame_id
        self.start_frame = frame_id

    def re_activate(self, new_track, frame_id, new_id=False):
        self.mean, self.covariance = self.kalman_filter.update(
            self.mean, self.covariance, self.tlwh_to_xyah(new_track.tlwh)
        )
        self.tracklet_len = 0
        self.state = TrackState.Tracked
        self.is_activated = True
        self.frame_id = frame_id
        if new_id:
            self.track_id = self.next_id()
        self.score = new_track.score
        self.class_id = new_track.class_id

    def update(self, new_track, frame_id):
        self.frame_id = frame_id
        self.tracklet_len += 1

        new_tlwh = new_track.tlwh
        self.mean, self.covariance = self.kalman_filter.update(
            self.mean, self.covariance, self.tlwh_to_xyah(new_tlwh)
        )
        self.state = TrackState.Tracked
        self.is_activated = True
        self.score = new_track.score
        self.class_id = new_track.class_id

    @property
    def tlwh(self):
        if self.mean is None:
            return self._tlwh.copy()
        ret = self.mean[:4].copy()
        ret[2] *= ret[3]
        ret[:2] -= ret[2:] / 2
        return ret

    @property
    def tlbr(self):
        ret = self.tlwh
        ret[2:] += ret[:2]
        return ret

    @staticmethod
    def tlwh_to_xyah(tlwh):
        ret = np.asarray(tlwh).copy()
        ret[:2] += ret[2:] / 2
        ret[2] /= ret[3]
        return ret

    @staticmethod
    def tlbr_to_tlwh(tlbr):
        ret = np.asarray(tlbr).copy()
        ret[2:] -= ret[:2]
        return ret


class BYTETracker:
    def __init__(self, track_thresh=0.45, high_thresh=0.6, match_thresh=0.8, max_time_lost=30):
        self.track_thresh = track_thresh
        self.high_thresh = high_thresh
        self.match_thresh = match_thresh
        self.max_time_lost = max_time_lost

        self.tracked_stracks = []  # active tracks
        self.lost_stracks = []     # temporarily lost tracks
        self.removed_stracks = []  # dead tracks

        self.frame_id = 0
        self.kalman_filter = KalmanFilter()

    def update(self, output_results):
        """
        output_results: numpy array of shape [N, 6] (x1, y1, x2, y2, score, class_id)
        Returns: list of active STrack instances
        """
        self.frame_id += 1
        activated_starcks = []
        refind_stracks = []
        lost_stracks = []
        removed_stracks = []

        if output_results is not None and len(output_results) > 0:
            scores = output_results[:, 4]
            bboxes = output_results[:, :4]
            classes = output_results[:, 5]

            # Stage 1: Split detections into High Conf and Low Conf
            remain_inds = scores >= self.track_thresh
            inds_low = scores < self.track_thresh
            inds_second = np.logical_and(inds_low, scores > 0.1)

            dets = bboxes[remain_inds]
            scores_keep = scores[remain_inds]
            classes_keep = classes[remain_inds]

            dets_second = bboxes[inds_second]
            scores_second = scores[inds_second]
            classes_second = classes[inds_second]

            # High confidence detections
            detections = [
                STrack(STrack.tlbr_to_tlwh(tlbr), s, c)
                for tlbr, s, c in zip(dets, scores_keep, classes_keep)
            ]
            # Low confidence detections
            detections_second = [
                STrack(STrack.tlbr_to_tlwh(tlbr), s, c)
                for tlbr, s, c in zip(dets_second, scores_second, classes_second)
            ]
        else:
            detections = []
            detections_second = []

        # Predict current locations for all existing tracks
        unconfirmed = []
        tracked_stracks = []
        for track in self.tracked_stracks:
            if not track.is_activated:
                unconfirmed.append(track)
            else:
                tracked_stracks.append(track)

        strack_pool = joint_stracks(tracked_stracks, self.lost_stracks)
        for strack in strack_pool:
            strack.predict()

        # Step 1: First association with high confidence detections
        dists = iou_distance(strack_pool, detections)
        matches, u_track, u_detection = linear_assignment(dists, thresh=self.match_thresh)

        for itracked, idet in matches:
            track = strack_pool[itracked]
            det = detections[idet]
            if track.state == TrackState.Tracked:
                track.update(det, self.frame_id)
                activated_starcks.append(track)
            else:
                track.re_activate(det, self.frame_id, new_id=False)
                refind_stracks.append(track)

        # Step 2: Second association with low confidence detections (recovering occlusions!)
        r_tracked_stracks = [strack_pool[i] for i in u_track if strack_pool[i].state == TrackState.Tracked]
        dists = iou_distance(r_tracked_stracks, detections_second)
        matches, u_track_second, _ = linear_assignment(dists, thresh=0.5)

        for itracked, idet in matches:
            track = r_tracked_stracks[itracked]
            det = detections_second[idet]
            if track.state == TrackState.Tracked:
                track.update(det, self.frame_id)
                activated_starcks.append(track)
            else:
                track.re_activate(det, self.frame_id, new_id=False)
                refind_stracks.append(track)

        # Unmatched tracks from step 2 mark as lost
        for it in u_track_second:
            track = r_tracked_stracks[it]
            if track.state != TrackState.Lost:
                track.mark_lost()
                lost_stracks.append(track)

        # Step 3: Deal with unconfirmed tracks
        detections = [detections[i] for i in u_detection]
        dists = iou_distance(unconfirmed, detections)
        matches, u_unconfirmed, u_detection = linear_assignment(dists, thresh=0.7)
        for itracked, idet in matches:
            unconfirmed[itracked].update(detections[idet], self.frame_id)
            activated_starcks.append(unconfirmed[itracked])
        for it in u_unconfirmed:
            track = unconfirmed[it]
            track.mark_removed()
            removed_stracks.append(track)

        # Step 4: Init new tracks from unmatched high confidence detections
        for inew in u_detection:
            track = detections[inew]
            if track.score < self.high_thresh:
                continue
            track.activate(self.kalman_filter, self.frame_id)
            activated_starcks.append(track)

        # Step 5: Update state and remove dead tracks
        for track in self.lost_stracks:
            if self.frame_id - track.frame_id > self.max_time_lost:
                track.mark_removed()
                removed_stracks.append(track)

        self.tracked_stracks = [t for t in self.tracked_stracks if t.state == TrackState.Tracked]
        self.tracked_stracks = joint_stracks(self.tracked_stracks, activated_starcks)
        self.tracked_stracks = joint_stracks(self.tracked_stracks, refind_stracks)
        self.lost_stracks = sub_stracks(self.lost_stracks, self.tracked_stracks)
        self.lost_stracks.extend(lost_stracks)
        self.lost_stracks = sub_stracks(self.lost_stracks, self.removed_stracks)
        self.removed_stracks.extend(removed_stracks)
        self.tracked_stracks, self.lost_stracks = remove_duplicate_stracks(self.tracked_stracks, self.lost_stracks)

        output_stracks = [track for track in self.tracked_stracks if track.is_activated]
        return output_stracks


def joint_stracks(tlista, tlistb):
    exists = {}
    res = []
    for t in tlista:
        exists[t.track_id] = 1
        res.append(t)
    for t in tlistb:
        tid = t.track_id
        if not exists.get(tid, 0):
            exists[tid] = 1
            res.append(t)
    return res


def sub_stracks(tlista, tlistb):
    stracks = {t.track_id: t for t in tlista}
    for t in tlistb:
        tid = t.track_id
        if stracks.get(tid, 0):
            del stracks[tid]
    return list(stracks.values())


def remove_duplicate_stracks(stracksa, stracksb):
    pdist = iou_distance(stracksa, stracksb)
    pairs = np.where(pdist < 0.15)
    dupa, dupb = list(), list()
    for p, q in zip(*pairs):
        timep = stracksa[p].frame_id - stracksa[p].start_frame
        timeq = stracksb[q].frame_id - stracksb[q].start_frame
        if timep > timeq:
            dupb.append(q)
        else:
            dupa.append(p)
    resa = [t for i, t in enumerate(stracksa) if not i in dupa]
    resb = [t for i, t in enumerate(stracksb) if not i in dupb]
    return resa, resb


def iou_distance(atracks, btracks):
    if (len(atracks) > 0 and isinstance(atracks[0], np.ndarray)) or (len(btracks) > 0 and isinstance(btracks[0], np.ndarray)):
        atlbrs = atracks
        btlbrs = btracks
    else:
        atlbrs = [track.tlbr for track in atracks]
        btlbrs = [track.tlbr for track in btracks]
    _ious = bbox_iou(np.asarray(atlbrs), np.asarray(btlbrs))
    cost_matrix = 1.0 - _ious
    return cost_matrix


def linear_assignment(cost_matrix, thresh):
    if cost_matrix.size == 0:
        return np.empty((0, 2), dtype=int), tuple(range(cost_matrix.shape[0])), tuple(range(cost_matrix.shape[1]))
    
    cost_matrix[cost_matrix > thresh] = thresh + 1e-4
    row_ind, col_ind = linear_sum_assignment(cost_matrix)
    
    matches, unmatched_a, unmatched_b = [], [], []
    for r, c in zip(row_ind, col_ind):
        if cost_matrix[r, c] > thresh:
            unmatched_a.append(r)
            unmatched_b.append(c)
        else:
            matches.append([r, c])
            
    for i in range(cost_matrix.shape[0]):
        if i not in row_ind:
            unmatched_a.append(i)
    for j in range(cost_matrix.shape[1]):
        if j not in col_ind:
            unmatched_b.append(j)
            
    return np.asarray(matches), tuple(unmatched_a), tuple(unmatched_b)

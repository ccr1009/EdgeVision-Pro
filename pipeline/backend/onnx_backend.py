# ==============================================================================
# EdgeVision-RK3588: x86 ONNXRuntime Inference Backend
# ==============================================================================
import os
import cv2
import numpy as np
import onnxruntime as ort
from .base import BaseBackend


def nms_boxes(boxes, scores, iou_thresh=0.45):
    if len(boxes) == 0:
        return []
    x1 = boxes[:, 0]
    y1 = boxes[:, 1]
    x2 = boxes[:, 2]
    y2 = boxes[:, 3]
    areas = (x2 - x1) * (y2 - y1)
    order = scores.argsort()[::-1]
    keep = []
    while order.size > 0:
        i = order[0]
        keep.append(i)
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        w = np.maximum(0.0, xx2 - xx1)
        h = np.maximum(0.0, yy2 - yy1)
        inter = w * h
        ovr = inter / (areas[i] + areas[order[1:]] - inter + 1e-6)
        inds = np.where(ovr <= iou_thresh)[0]
        order = order[inds + 1]
    return keep


class ONNXRuntimeBackend(BaseBackend):
    def __init__(self, model_path: str, conf_thresh=0.25, iou_thresh=0.45):
        self.conf_thresh = conf_thresh
        self.iou_thresh = iou_thresh
        self.load(model_path)

    def load(self, model_path: str):
        if not os.path.exists(model_path):
            raise FileNotFoundError(f"ONNX model not found: {model_path}")
        self.session = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
        self.input_name = self.session.get_inputs()[0].name
        self.output_name = self.session.get_outputs()[0].name
        shape = self.session.get_inputs()[0].shape
        self.input_h = shape[2] if isinstance(shape[2], int) else 640
        self.input_w = shape[3] if isinstance(shape[3], int) else 640

    def get_input_shape(self):
        return (self.input_w, self.input_h)

    def preprocess(self, img_bgr):
        h, w = img_bgr.shape[:2]
        scale = min(self.input_w / w, self.input_h / h)
        nw, nh = int(w * scale), int(h * scale)
        resized = cv2.resize(img_bgr, (nw, nh))

        padded = np.full((self.input_h, self.input_w, 3), 114, dtype=np.uint8)
        dw = (self.input_w - nw) // 2
        dh = (self.input_h - nh) // 2
        padded[dh:dh + nh, dw:dw + nw] = resized

        rgb = cv2.cvtColor(padded, cv2.COLOR_BGR2RGB)
        tensor = rgb.astype(np.float32) / 255.0
        tensor = np.transpose(tensor, (2, 0, 1))
        tensor = np.expand_dims(tensor, axis=0)
        return tensor, scale, dw, dh

    def postprocess(self, raw_out, scale, dw, dh, orig_shape):
        orig_h, orig_w = orig_shape[:2]
        preds = np.squeeze(raw_out).T
        boxes = preds[:, :4]
        scores = preds[:, 4:]

        max_scores = np.max(scores, axis=1)
        class_ids = np.argmax(scores, axis=1)

        mask = max_scores >= self.conf_thresh
        boxes = boxes[mask]
        max_scores = max_scores[mask]
        class_ids = class_ids[mask]

        if len(boxes) == 0:
            return np.empty((0, 6), dtype=np.float32)

        x1 = boxes[:, 0] - boxes[:, 2] / 2
        y1 = boxes[:, 1] - boxes[:, 3] / 2
        x2 = boxes[:, 0] + boxes[:, 2] / 2
        y2 = boxes[:, 1] + boxes[:, 3] / 2

        x1 = (x1 - dw) / scale
        y1 = (y1 - dh) / scale
        x2 = (x2 - dw) / scale
        y2 = (y2 - dh) / scale

        x1 = np.clip(x1, 0, orig_w)
        y1 = np.clip(y1, 0, orig_h)
        x2 = np.clip(x2, 0, orig_w)
        y2 = np.clip(y2, 0, orig_h)

        boxes_xyxy = np.stack([x1, y1, x2, y2], axis=1)

        final_dets = []
        for cls in np.unique(class_ids):
            cls_mask = class_ids == cls
            cls_boxes = boxes_xyxy[cls_mask]
            cls_scores = max_scores[cls_mask]
            keep = nms_boxes(cls_boxes, cls_scores, self.iou_thresh)
            for idx in keep:
                final_dets.append([
                    cls_boxes[idx, 0], cls_boxes[idx, 1],
                    cls_boxes[idx, 2], cls_boxes[idx, 3],
                    cls_scores[idx], cls
                ])

        if len(final_dets) == 0:
            return np.empty((0, 6), dtype=np.float32)
        return np.array(final_dets, dtype=np.float32)

    def infer(self, img_bgr: np.ndarray) -> np.ndarray:
        tensor, scale, dw, dh = self.preprocess(img_bgr)
        raw_out = self.session.run([self.output_name], {self.input_name: tensor})[0]
        dets = self.postprocess(raw_out, scale, dw, dh, img_bgr.shape)
        return dets

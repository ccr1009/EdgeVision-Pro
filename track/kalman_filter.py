# ==============================================================================
# EdgeVision-RK3588: Pure NumPy 2D Kalman Filter for Object Bounding Boxes
# State Vector (8D): [cx, cy, a, h, vx, vy, va, vh]
# - cx, cy: Center coordinates of the bounding box
# - a: Aspect ratio (width / height)
# - h: Height of the bounding box
# - vx, vy, va, vh: Velocity rates of change
# ==============================================================================
import numpy as np
import scipy.linalg


class KalmanFilter:
    def __init__(self):
        ndim = 4
        dt = 1.0

        # Motion Transition Matrix F (8x8)
        self._motion_mat = np.eye(2 * ndim, 2 * ndim)
        for i in range(ndim):
            self._motion_mat[i, ndim + i] = dt

        # Measurement Projection Matrix H (4x8)
        self._update_mat = np.eye(ndim, 2 * ndim)

        # Process and observation noise weights
        self._std_weight_position = 1.0 / 20
        self._std_weight_velocity = 1.0 / 160

    def initiate(self, measurement):
        """
        Initializes state mean and covariance from an initial bounding box measurement [cx, cy, a, h].
        """
        mean_pos = measurement
        mean_vel = np.zeros_like(mean_pos)
        mean = np.r_[mean_pos, mean_vel]

        std = [
            2 * self._std_weight_position * measurement[3],
            2 * self._std_weight_position * measurement[3],
            1e-2,
            2 * self._std_weight_position * measurement[3],
            10 * self._std_weight_velocity * measurement[3],
            10 * self._std_weight_velocity * measurement[3],
            1e-5,
            10 * self._std_weight_velocity * measurement[3]
        ]
        covariance = np.diag(np.square(std))
        return mean, covariance

    def predict(self, mean, covariance):
        """
        Predicts state distribution one step forward in time:
        x_k|k-1 = F * x_k-1|k-1
        P_k|k-1 = F * P_k-1|k-1 * F^T + Q
        """
        std_pos = [
            self._std_weight_position * mean[3],
            self._std_weight_position * mean[3],
            1e-2,
            self._std_weight_position * mean[3]
        ]
        std_vel = [
            self._std_weight_velocity * mean[3],
            self._std_weight_velocity * mean[3],
            1e-5,
            self._std_weight_velocity * mean[3]
        ]
        motion_cov = np.diag(np.square(np.r_[std_pos, std_vel]))

        mean = np.dot(self._motion_mat, mean)
        covariance = np.linalg.multi_dot((self._motion_mat, covariance, self._motion_mat.T)) + motion_cov
        return mean, covariance

    def project(self, mean, covariance):
        """
        Projects state distribution into measurement space (4D):
        z_proj = H * x
        S = H * P * H^T + R
        """
        std = [
            self._std_weight_position * mean[3],
            self._std_weight_position * mean[3],
            1e-1,
            self._std_weight_position * mean[3]
        ]
        innovation_cov = np.diag(np.square(std))

        mean = np.dot(self._update_mat, mean)
        covariance = np.linalg.multi_dot((self._update_mat, covariance, self._update_mat.T)) + innovation_cov
        return mean, covariance

    def update(self, mean, covariance, measurement):
        """
        Corrects predicted state using new observation:
        K = P * H^T * S^-1 (Kalman Gain)
        x_k|k = x_k|k-1 + K * (z - H * x_k|k-1)
        P_k|k = (I - K * H) * P_k|k-1
        """
        projected_mean, projected_cov = self.project(mean, covariance)

        chol_factor, lower = scipy.linalg.cho_factor(projected_cov, lower=True, check_finite=False)
        kalman_gain = scipy.linalg.cho_solve((chol_factor, lower), np.dot(covariance, self._update_mat.T).T, check_finite=False).T
        innovation = measurement - projected_mean

        new_mean = mean + np.dot(innovation, kalman_gain.T)
        new_covariance = covariance - np.linalg.multi_dot((kalman_gain, projected_cov, kalman_gain.T))
        return new_mean, new_covariance

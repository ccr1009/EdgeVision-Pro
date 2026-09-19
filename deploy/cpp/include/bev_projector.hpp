#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <iostream>
#include <algorithm>

namespace edgevision {

struct Point3D {
    float x; // fwd
    float y; // left
    float z; // up
};

struct Point2D {
    float u;
    float v;
};

struct Box3D {
    Point3D center;
    Point3D dims; // L, W, H
    std::vector<Point2D> corners_2d; // 8 projected corners
    float distance_m;
    float longitudinal_m;
    float lateral_m;
};

class BEVProjector {
public:
    BEVProjector(int img_w = 1280, int img_h = 720, float hfov_deg = 65.0f,
                 float mount_height_m = 1.6f, float pitch_deg = 8.0f)
        : img_w_(img_w), img_h_(img_h), h_(mount_height_m) {
        
        float hfov_rad = hfov_deg * 3.14159265f / 180.0f;
        fx_ = (img_w / 2.0f) / std::tan(hfov_rad / 2.0f);
        fy_ = fx_;
        cx_ = img_w / 2.0f;
        cy_ = img_h / 2.0f;

        pitch_rad_ = pitch_deg * 3.14159265f / 180.0f;
        cp_ = std::cos(pitch_rad_);
        sp_ = std::sin(pitch_rad_);
    }

    bool pixel_to_ground(float u, float v, Point3D& out_pt) const {
        // Ray in camera frame
        float x_c = (u - cx_) / fx_;
        float y_c = (v - cy_) / fy_;
        float z_c = 1.0f;

        // Transform ray to vehicle frame:
        // R_v_c = R_base^T * R_pitch^T
        // Vehicle forward x_v, left y_v, up z_v
        float r_vx = cp_ * z_c - sp_ * y_c;
        float r_vy = -x_c;
        float r_vz = -(sp_ * z_c + cp_ * y_c);

        if (r_vz >= -1e-5f) {
            return false; // Above horizon
        }

        float lambda = -h_ / r_vz;
        if (lambda <= 0.0f) return false;

        out_pt.x = lambda * r_vx;
        out_pt.y = lambda * r_vy;
        out_pt.z = 0.0f;
        return true;
    }

    bool point_to_pixel(const Point3D& pt_v, Point2D& out_pixel, float& out_depth) const {
        // Camera position at (0, 0, h)
        float dx = pt_v.x;
        float dy = pt_v.y;
        float dz = pt_v.z - h_;

        // R_c_v @ vec_v
        float x_c = -dy;
        float y_c = -cp_ * dz + sp_ * dx;
        float z_c =  sp_ * dz + cp_ * dx;

        if (z_c <= 0.1f) return false;

        out_pixel.u = (fx_ * x_c / z_c) + cx_;
        out_pixel.v = (fy_ * y_c / z_c) + cy_;
        out_depth = z_c;
        return true;
    }

    bool estimate_3d_box(float x1, float y1, float x2, float y2,
                         const std::string& cls, Box3D& out_box) const {
        float u_contact = (x1 + x2) * 0.5f;
        float v_contact = y2;

        Point3D ground_pt;
        if (!pixel_to_ground(u_contact, v_contact, ground_pt)) {
            return false;
        }

        if (ground_pt.x < 0.5f || ground_pt.x > 100.0f) return false;

        // Dimension priors
        float L = 4.5f, W = 1.8f, H = 1.5f;
        if (cls == "person") {
            L = 0.6f; W = 0.6f; H = 1.7f;
        } else if (cls == "bicycle" || cls == "motorcycle") {
            L = 1.8f; W = 0.6f; H = 1.2f;
        }

        out_box.center = {ground_pt.x, ground_pt.y, H * 0.5f};
        out_box.dims = {L, W, H};
        out_box.longitudinal_m = ground_pt.x;
        out_box.lateral_m = ground_pt.y;
        out_box.distance_m = std::sqrt(ground_pt.x * ground_pt.x + ground_pt.y * ground_pt.y);

        // 8 corners
        float dx = L * 0.5f, dy = W * 0.5f, dz = H * 0.5f;
        Point3D corners[8] = {
            {ground_pt.x + dx, ground_pt.y + dy, 0.0f},
            {ground_pt.x + dx, ground_pt.y - dy, 0.0f},
            {ground_pt.x - dx, ground_pt.y - dy, 0.0f},
            {ground_pt.x - dx, ground_pt.y + dy, 0.0f},
            {ground_pt.x + dx, ground_pt.y + dy, H},
            {ground_pt.x + dx, ground_pt.y - dy, H},
            {ground_pt.x - dx, ground_pt.y - dy, H},
            {ground_pt.x - dx, ground_pt.y + dy, H},
        };

        out_box.corners_2d.clear();
        for (int i = 0; i < 8; ++i) {
            Point2D px;
            float depth;
            if (!point_to_pixel(corners[i], px, depth)) return false;
            out_box.corners_2d.push_back(px);
        }
        return true;
    }

private:
    int img_w_, img_h_;
    float h_, fx_, fy_, cx_, cy_;
    float pitch_rad_, cp_, sp_;
};

} // namespace edgevision

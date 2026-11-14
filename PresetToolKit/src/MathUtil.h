#pragma once
#include <array>
#include <cmath>

namespace ptk {

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double X, double Y, double Z) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
};

inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) { double l = length(v); return l > 0 ? v / l : v; }

// Row-major 3x3
struct Mat3 {
    std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};
    double& operator()(int r, int c) { return m[r * 3 + c]; }
    double operator()(int r, int c) const { return m[r * 3 + c]; }
    Vec3 operator*(const Vec3& v) const {
        return {m[0] * v.x + m[1] * v.y + m[2] * v.z,
                m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z};
    }
    Mat3 operator*(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double s = 0;
                for (int k = 0; k < 3; ++k) s += (*this)(i, k) * o(k, j);
                r(i, j) = s;
            }
        return r;
    }
    double det() const {
        const auto& a = *this;
        return a(0,0)*(a(1,1)*a(2,2)-a(1,2)*a(2,1)) - a(0,1)*(a(1,0)*a(2,2)-a(1,2)*a(2,0)) +
               a(0,2)*(a(1,0)*a(2,1)-a(1,1)*a(2,0));
    }
    Mat3 inverse() const {
        const auto& a = *this;
        double d = det();
        Mat3 r;
        if (std::fabs(d) < 1e-20) return r; // identity fallback
        double id = 1.0 / d;
        r(0,0) =  (a(1,1)*a(2,2)-a(1,2)*a(2,1))*id;
        r(0,1) = -(a(0,1)*a(2,2)-a(0,2)*a(2,1))*id;
        r(0,2) =  (a(0,1)*a(1,2)-a(0,2)*a(1,1))*id;
        r(1,0) = -(a(1,0)*a(2,2)-a(1,2)*a(2,0))*id;
        r(1,1) =  (a(0,0)*a(2,2)-a(0,2)*a(2,0))*id;
        r(1,2) = -(a(0,0)*a(1,2)-a(0,2)*a(1,0))*id;
        r(2,0) =  (a(1,0)*a(2,1)-a(1,1)*a(2,0))*id;
        r(2,1) = -(a(0,0)*a(2,1)-a(0,1)*a(2,0))*id;
        r(2,2) =  (a(0,0)*a(1,1)-a(0,1)*a(1,0))*id;
        return r;
    }
};

inline Mat3 rotX(double a) { Mat3 r; double c = std::cos(a), s = std::sin(a); r(1,1)=c; r(1,2)=-s; r(2,1)=s; r(2,2)=c; return r; }
inline Mat3 rotY(double a) { Mat3 r; double c = std::cos(a), s = std::sin(a); r(0,0)=c; r(0,2)=s; r(2,0)=-s; r(2,2)=c; return r; }
inline Mat3 rotZ(double a) { Mat3 r; double c = std::cos(a), s = std::sin(a); r(0,0)=c; r(0,1)=-s; r(1,0)=s; r(1,1)=c; return r; }

// Affine transform: linear part + translation (enough for glTF node TRS / matrix)
struct Affine {
    Mat3 l;
    Vec3 t;
    Vec3 apply(const Vec3& p) const { return l * p + t; }
    Affine operator*(const Affine& o) const { return {l * o.l, l * o.t + t}; }
};

constexpr double kPi = 3.14159265358979323846;
inline double deg2rad(double d) { return d * kPi / 180.0; }

} // namespace ptk

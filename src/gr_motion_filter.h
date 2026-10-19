/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Orientation filter for the pad's motion sensors (shared by runtime_pad.c and its test).
 * q: body -> reference frame (identity at scePadResetOrientation), integrated as q' = q + q*w/2.
 * Gyro bias: learned slowly while the controller lies still, then subtracted (the yaw drift
 * the accelerometer cannot see). Tilt correction (scePadSetTiltCorrectionState): a Mahony-style
 * complementary filter keeps the predicted gravity (the reference gravity seen from the body)
 * on the measured one, so pitch and roll do not drift. */
#pragma once
#include <math.h>

typedef struct {
    float q[4];        /* x, y, z, w */
    float bias[3];     /* rad/s */
    float g_ref[3];    /* gravity direction in the reference frame (unit) */
    int have_ref;      /* g_ref captured (first good accelerometer sample after a reset) */
    float still_s;     /* seconds the controller has looked still */
    float gyro_lp[3];  /* low-passed raw gyro (0.2 s) */
    float accel_lp[3]; /* low-passed acceleration */
    int lp_ready;
} MotionFilter;

static void motion_filter_reset(MotionFilter *f) {
    f->q[0] = f->q[1] = f->q[2] = 0.0f; f->q[3] = 1.0f;
    f->have_ref = 0;
    f->still_s = 0.0f;
    f->lp_ready = 0;
}

static void motion_filter_init(MotionFilter *f) {
    motion_filter_reset(f);
    f->bias[0] = f->bias[1] = f->bias[2] = 0.0f;
}

/* v_body = q^-1 * v_ref * q */
static void motion_rotate_to_body(const float q[4], const float v[3], float out[3]) {
    const float x = -q[0], y = -q[1], z = -q[2], w = q[3];
    /* t = 2 * cross(qv, v); out = v + w * t + cross(qv, t), with qv the conjugate's vector */
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

/* gyro: raw angular velocity (rad/s); accel: acceleration in g (any scale, NULL when absent);
 * tilt: the game's tilt correction state. Returns the bias-corrected angular velocity in out_w. */
static void motion_filter_update(MotionFilter *f, const float gyro[3], const float *accel, int tilt,
                                 float dt, float out_w[3]) {
    float a[3] = {0, 0, 0}, an = 0.0f;
    if (accel) {
        an = sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
        if (an > 0.0f) for (int i = 0; i < 3; ++i) a[i] = accel[i] / an;
    }
    const int accel_ok = accel && an > 0.85f && an < 1.15f; /* no strong linear acceleration */
    /* Bias: the controller lies still when its readings stay steady (whatever the bias) for
     * 0.5 s, the gyro reads under 0.1 rad/s and the accelerometer sees only gravity; while it
     * does, the bias follows the readings slowly. */
    if (!f->lp_ready) {
        for (int i = 0; i < 3; ++i) { f->gyro_lp[i] = gyro[i]; f->accel_lp[i] = accel ? accel[i] : 0.0f; }
        f->lp_ready = 1;
    }
    const float klp = fminf(1.0f, dt / 0.2f);
    float dev = 0.0f, adev = 0.0f, lpmag = 0.0f;
    for (int i = 0; i < 3; ++i) {
        f->gyro_lp[i] += klp * (gyro[i] - f->gyro_lp[i]);
        dev = fmaxf(dev, fabsf(gyro[i] - f->gyro_lp[i]));
        lpmag = fmaxf(lpmag, fabsf(f->gyro_lp[i]));
        if (accel) {
            f->accel_lp[i] += klp * (accel[i] - f->accel_lp[i]);
            adev = fmaxf(adev, fabsf(accel[i] - f->accel_lp[i]));
        }
    }
    if (dev < 0.02f && lpmag < 0.1f && adev < 0.05f && (!accel || accel_ok)) {
        f->still_s += dt;
        if (f->still_s > 0.5f) {
            const float k = fminf(1.0f, dt / 2.0f); /* ~2 s time constant */
            for (int i = 0; i < 3; ++i) {
                f->bias[i] += k * (f->gyro_lp[i] - f->bias[i]);
                if (f->bias[i] > 0.08f) f->bias[i] = 0.08f;
                if (f->bias[i] < -0.08f) f->bias[i] = -0.08f;
            }
        }
    } else {
        f->still_s = 0.0f;
    }
    float w[3];
    for (int i = 0; i < 3; ++i) w[i] = gyro[i] - f->bias[i];
    for (int i = 0; i < 3; ++i) out_w[i] = w[i];

    if (accel_ok && !f->have_ref) {
        /* The reference gravity: the measured one seen from the reference frame now. */
        float qi[4] = {-f->q[0], -f->q[1], -f->q[2], f->q[3]};
        motion_rotate_to_body(qi, a, f->g_ref);
        f->have_ref = 1;
    }
    float wc[3] = {w[0], w[1], w[2]};
    if (tilt && accel_ok && f->have_ref) {
        float v[3];
        motion_rotate_to_body(f->q, f->g_ref, v); /* predicted gravity in the body frame */
        const float kp = 1.0f;                    /* ~1 s to pull pitch and roll back */
        wc[0] += kp * (a[1] * v[2] - a[2] * v[1]);
        wc[1] += kp * (a[2] * v[0] - a[0] * v[2]);
        wc[2] += kp * (a[0] * v[1] - a[1] * v[0]);
    }
    if (dt > 0.0f && dt < 1.0f) {
        float *q = f->q;
        const float dq[4] = {q[3] * wc[0] + q[1] * wc[2] - q[2] * wc[1],
                             q[3] * wc[1] + q[2] * wc[0] - q[0] * wc[2],
                             q[3] * wc[2] + q[0] * wc[1] - q[1] * wc[0],
                             -q[0] * wc[0] - q[1] * wc[1] - q[2] * wc[2]};
        for (int i = 0; i < 4; ++i) q[i] += 0.5f * dq[i] * dt;
        const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (n > 0.0f) for (int i = 0; i < 4; ++i) q[i] /= n;
    }
}

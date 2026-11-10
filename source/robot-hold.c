#include <math.h>

#include "robot-hold.h"

// Wheel speeds are percent of full speed either way
#define WHEEL_FULL 100

static float dot(const float a[3], const float b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The sine of the angle to turn, clockwise seen from above positive, or
// plus or minus 1 for a heading behind. East is up x north in the board's
// axes, which are left-handed.
static float error(const Hold *h, const float up[3], const float north[3]) {
  float east[3] = {
    up[1] * north[2] - up[2] * north[1],
    up[2] * north[0] - up[0] * north[2],
    up[0] * north[1] - up[1] * north[0],
  };
  float c = dot(h->f, north), s = dot(h->f, east);
  float sine = h->s * c - h->c * s;
  if (h->c * c + h->s * s >= 0) return sine;
  return sine < 0 ? -1 : 1;
}

int hold_start(Hold *h, const float up[3], const float forward[3],
               float c, float s, float speed, int steps,
               const float north[3]) {
  float along = dot(forward, up), f[3];
  for (int i = 0; i < 3; i++) f[i] = forward[i] - along * up[i];
  float n = sqrtf(dot(f, f));
  if (n == 0) return 0;
  for (int i = 0; i < 3; i++) h->f[i] = f[i] / n;
  h->c = c;
  h->s = s;
  h->speed = speed;
  h->steps = steps;
  h->sum = 0;
  h->last = error(h, up, north);
  return 1;
}

static float clamp(float x) {
  return x > WHEEL_FULL ? WHEEL_FULL : x < -WHEEL_FULL ? -WHEEL_FULL : x;
}

int hold_step(Hold *h, const float up[3], const float north[3], float dt,
              float wheels[2]) {
  if (h->steps == 0) return 0;
  if (h->steps > 0) h->steps--;
  float e = error(h, up, north);
  h->sum += e * dt;
  float u = h->kp * e + h->ki * h->sum + h->kd * (e - h->last) / dt;
  h->last = e;
  wheels[0] = clamp(h->speed + u);
  wheels[1] = clamp(h->speed - u);
  return 1;
}

#include <math.h>
#include <string.h>

#include "compass-track.h"

// The rate at which the noise estimate follows the samples, and how many
// noise variances tell a quantity from the noise (25: five standard
// deviations)
#define NOISE_RATE 0.01f
#define SPREAD 25

static float dot(const float a[3], const float b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Seek the circle anew, from the samples to come; the field is taken as
// it comes until then
static void seek(Track *t) {
  t->n = t->sx = t->sy = 0;
  t->sxx = t->sxy = t->syy = 0;
  t->sxr = t->syr = t->sr = t->srr = 0;
  t->seen = 0;
  t->found = 0;
  t->away = 0;
  t->alpha = 1;
}

// u: a board axis made perpendicular to up; v: perpendicular to both
void track_start(Track *t, const float up[3], float eps) {
  memset(t, 0, sizeof *t);
  memcpy(t->up, up, sizeof t->up);
  t->eps = eps;
  t->u[fabsf(up[0]) < 0.5f ? 0 : 1] = 1;
  float along = dot(t->u, up);
  for (int i = 0; i < 3; i++) t->u[i] -= along * up[i];
  float n = sqrtf(dot(t->u, t->u));
  for (int i = 0; i < 3; i++) t->u[i] /= n;
  t->v[0] = up[1] * t->u[2] - up[2] * t->u[1];
  t->v[1] = up[2] * t->u[0] - up[0] * t->u[2];
  t->v[2] = up[0] * t->u[1] - up[1] * t->u[0];
  seek(t);
}

// Noise variance of a sample on one axis, from third differences in the
// plane: white noise gives each 20 times its variance per axis, a turn
// adds little. Their mean at first, then a slow average.
static void measure(Track *t, const float h[2]) {
  if (t->samples >= 3) {
    float (*l)[2] = t->last;
    float dx = h[0] - 3 * l[0][0] + 3 * l[1][0] - l[2][0];
    float dy = h[1] - 3 * l[0][1] + 3 * l[1][1] - l[2][1];
    float d = (dx * dx + dy * dy) / 40;
    float rate = fmaxf(1.0f / (t->samples - 2), NOISE_RATE);
    t->noise += rate * (d - t->noise);
  }
  memmove(t->last[1], t->last[0], 2 * sizeof t->last[0]);
  memcpy(t->last[0], h, sizeof t->last[0]);
  t->samples++;
}

// Add a sample to the sums, taken from the first one for precision
static void gather(Track *t, const float h[2]) {
  if (t->n == 0) memcpy(t->origin, h, sizeof t->origin);
  double x = h[0] - t->origin[0], y = h[1] - t->origin[1];
  double r = x * x + y * y;
  t->n++;
  t->sx += x; t->sy += y;
  t->sxx += x * x; t->sxy += x * y; t->syy += y * y;
  t->sxr += x * r; t->syr += y * r; t->sr += r; t->srr += r * r;
}

// Least-squares (Kasa) circle r = 2ax + 2by + c through the samples, by
// the inverse of the normal equations' matrix, which also tells how well
// the centre and the radius are known. 0 while the samples lie on a line,
// as those of a board that has not turned may.
static int fit(Track *t) {
  double c00 = t->syy * t->n - t->sy * t->sy;
  double c01 = t->sx * t->sy - t->sxy * t->n;
  double c02 = t->sxy * t->sy - t->syy * t->sx;
  double c11 = t->sxx * t->n - t->sx * t->sx;
  double c12 = t->sxy * t->sx - t->sxx * t->sy;
  double c22 = t->sxx * t->syy - t->sxy * t->sxy;
  double det = t->sxx * c00 + t->sxy * c01 + t->sx * c02;
  if (det <= 0) return 0;
  double p0 = (c00 * t->sxr + c01 * t->syr + c02 * t->sr) / det;
  double p1 = (c01 * t->sxr + c11 * t->syr + c12 * t->sr) / det;
  double p2 = (c02 * t->sxr + c12 * t->syr + c22 * t->sr) / det;
  double a = p0 / 2, b = p1 / 2, rr = p2 + a * a + b * b;
  double off = t->srr - p0 * t->sxr - p1 * t->syr - p2 * t->sr;
  t->cx = t->origin[0] + a;
  t->cy = t->origin[1] + b;
  t->radius = sqrt(rr);
  double rvar = a * a * c00 + b * b * c11 + c22
      + 2 * (a * b * c01 + a * c02 + b * c12);
  t->play = (rr * (c00 + c11) + rvar) / det;
  t->misfit = off / (4 * rr) / t->n;
  return 1;
}

// Octant of a point round the origin
static int octant(float x, float y) {
  int o = 0;
  if (y < 0) { x = -x; y = -y; o = 4; }
  if (x < 0) { float s = x; x = y; y = -s; o += 2; }
  return y > x ? o + 1 : o;
}

// Squared offset from the circle of a field moved by eps radians of
// heading: a field moved by d turns the heading by about d / radius
static float slack(const Track *t) {
  return t->eps * t->eps * t->radius * t->radius;
}

// Whether the circle stands out of the noise, and its centre and radius
// are known SPREAD times better than the slack
static int certain(const Track *t) {
  float r2 = t->radius * t->radius;
  return r2 > SPREAD * t->noise && SPREAD * t->noise * t->play < slack(t);
}

// Whether the samples lie off the circle, beyond the noise across it, by
// more than the slack
static int scattered(const Track *t) {
  return t->misfit - t->noise > slack(t);
}

// Squared offset of the field from the circle, filtered until its noise
// is SPREAD times below the slack
static float offset(Track *t, const float h[2]) {
  float dr = hypotf(h[0] - t->cx, h[1] - t->cy) - t->radius;
  t->dr += t->beta * (dr - t->dr);
  return t->dr * t->dr;
}

// Filter weight that leaves 1/k of the noise variance: the filter keeps
// alpha / (2 - alpha) of it
static float weight(float k) {
  return k <= 1 ? 1 : 2 / (1 + k);
}

// Weights once the circle is found: the field's leaves noise that moves
// the heading by about eps; the offset's leaves SPREAD times less
static void tune(Track *t) {
  float k = t->noise / slack(t);
  t->alpha = weight(k);
  t->beta = weight(SPREAD * k);
}

// Filter the sample and add it to the sums. Nothing is fitted before the
// noise is measured, from the fourth sample. A circle known well enough is
// found once the field has passed every octant round it, unless the
// samples scatter from it. A found circle is lost while the field is off
// it by more than the slack, and found again once the field is back within
// half of it; the samples off it stay out of the sums.
int track_sample(Track *t, const float raw[3]) {
  float h[2] = { dot(raw, t->u), dot(raw, t->v) };
  measure(t, h);
  for (int i = 0; i < 3; i++)
    t->field[i] += t->alpha * (raw[i] - t->field[i]);
  if (t->found) {
    float d = offset(t, h);
    if (t->away) {
      if (4 * d < slack(t)) {
        t->away = 0;
        return TRACK_FOUND;
      }
      if (++t->away > TRACK_RETURN / TRACK_PERIOD) seek(t);
      return 0;
    }
    if (d > slack(t)) {
      t->away = 1;
      return TRACK_LOST;
    }
  }
  gather(t, h);
  if (t->samples <= 3 || !fit(t)) return 0;
  if (t->found) {
    tune(t);
    return 0;
  }
  if (!certain(t)) {
    t->seen = 0;
    return 0;
  }
  t->seen |= 1u << octant(h[0] - t->cx, h[1] - t->cy);
  if (t->seen != 0xff) return 0;
  if (scattered(t)) {
    seek(t);
    return 0;
  }
  tune(t);
  t->dr = 0;
  t->found = 1;
  return TRACK_FOUND;
}

int track_north(const Track *t, float north[3]) {
  if (!t->found || t->away) return 0;
  float x = dot(t->field, t->u) - t->cx, y = dot(t->field, t->v) - t->cy;
  float n = sqrtf(x * x + y * y);
  for (int i = 0; i < 3; i++) north[i] = (x * t->u[i] + y * t->v[i]) / n;
  return 1;
}

#ifndef COMPASS_TRACK_H
#define COMPASS_TRACK_H

#ifdef __cplusplus
extern "C" {
#endif

// As the board turns about up, the field runs round a circle centred on
// the field the board itself adds. From that centre the field points
// north. Every sample goes into the sums of a least-squares circle, found
// once the centre is known well enough for the heading noise wanted and
// the field has since gone round it once: a fit to part of a turn takes
// the field's unevenness for the circle's shape. Turning keeps the field
// on the circle; a field that leaves it, disturbed, loses it until it
// comes back, and the circle is sought anew if it does not come back soon.
// Only the field across up counts: the heading does not depend on the
// field along up, which on a floor changes from place to place.

// What a sample reports: the circle found, or found again, or lost
#define TRACK_FOUND 1
#define TRACK_LOST 2

// Milliseconds between samples, and how long a field off the circle has
// to come back before the circle is sought anew
#define TRACK_PERIOD 50
#define TRACK_RETURN 3000

typedef struct {
  float up[3], u[3], v[3];      // up and the plane across it
  float eps;                    // heading noise wanted, in radians
  float alpha;                  // filter weight of a new sample
  float beta;                   // filter weight of its offset
  float field[3];               // the filtered field
  float last[3][2];             // the three samples before
  int samples;
  float noise;                  // variance of a sample on one axis
  float origin[2];              // the sums' first sample
  double n, sx, sy, sxx, sxy, syy, sxr, syr, sr, srr;
  float cx, cy, radius;
  unsigned seen;                // octants passed since the centre was known
  float play;                   // variance of centre and radius per noise
  float misfit;                 // mean square off the circle
  float dr;                     // filtered offset from the circle
  int found;
  int away;                     // samples since the field left it, or 0
} Track;

// A new track across up, a unit vector, for heading noise eps
void track_start(Track *t, const float up[3], float eps);

// One sample of the field; TRACK_FOUND or TRACK_LOST when that happens,
// otherwise 0
int track_sample(Track *t, const float raw[3]);

// North, a unit vector across up; 0 while there is no circle or the field
// is off it
int track_north(const Track *t, float north[3]);

#ifdef __cplusplus
}
#endif

#endif

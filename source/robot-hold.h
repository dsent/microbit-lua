#ifndef ROBOT_HOLD_H
#define ROBOT_HOLD_H

#ifdef __cplusplus
extern "C" {
#endif

// Holding a heading: a PID on the angle between the robot's forward and
// the heading wanted, both across up, steering the wheels apart around
// the speed wanted. A heading is a vector, cosine and sine of its angle
// from magnetic north towards east; no angle is ever computed. A heading
// behind the robot turns it at full steer.

typedef struct {
  float kp, ki, kd;
  float f[3];                   // forward across up, a unit vector
  float c, s;                   // the heading wanted
  float speed;                  // mean wheel speed
  float sum, last;              // integral and last value of the error
  int steps;                    // samples left, or -1 to hold until stopped
} Hold;

// A new hold of the heading (c, s) at speed for steps samples, -1 for no
// end, with forward in the board's axes and north where it is now; the
// gains stay as they are. 0, and the hold as it was, when forward has
// nothing across up.
int hold_start(Hold *h, const float up[3], const float forward[3],
               float c, float s, float speed, int steps,
               const float north[3]);

// One step of dt seconds: the wheels' speeds, left and right, within
// plus or minus 100; 0 once the time is up
int hold_step(Hold *h, const float up[3], const float north[3], float dt,
              float wheels[2]);

#ifdef __cplusplus
}
#endif

#endif

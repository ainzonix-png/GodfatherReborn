/* gf_ragdoll_jolt_stub.c -- v1056A: used when GF_USE_JOLT=OFF (or Jolt is missing): no rigid-body core, gf_ragdoll.c
 * then runs its particle solver (exactly as GF_RAG_ENGINE=pbd). */
#include <stddef.h>
void* jr_create(const float* p0, const float* pv, float dt, float ground, float kn_sign, float el_sign)
{ (void)p0; (void)pv; (void)dt; (void)ground; (void)kn_sign; (void)el_sign; return NULL; }
void jr_tone(void* h, const float* anim, float a, float b, float c, int d) { (void)h; (void)anim; (void)a; (void)b; (void)c; (void)d; }
void jr_pull(void* h, int part, const float* p, const float* t, float k) { (void)h; (void)part; (void)p; (void)t; (void)k; }
void jr_step(void* h, float dt) { (void)h; (void)dt; }
int jr_get(void* h, float* Q, float* X, float* p) { (void)h; (void)Q; (void)X; (void)p; return 0; }
void jr_destroy(void* h) { (void)h; }
void jr_add_mesh(void* h, const float* tri, int n, float drop) { (void)h; (void)tri; (void)n; (void)drop; }
/* v1069H hit reactions: no Jolt -> none (gf_ragdoll.c also turns them off) */
void* jr_hr_create(const float* p0, float ground) { (void)p0; (void)ground; return NULL; }
void jr_hr_target(void* h, const float* a, const float* t, const float* s, const float* p) { (void)h; (void)a; (void)t; (void)s; (void)p; }
void jr_hr_step(void* h, float dt) { (void)h; (void)dt; }
int jr_hr_get(void* h, float* C, float* Xp, float* Xa, float* p, float* m) { (void)h; (void)C; (void)Xp; (void)Xa; (void)p; (void)m; return 0; }
int jr_impulse(void* h, int part, const float* pt, const float* J) { (void)h; (void)part; (void)pt; (void)J; return 0; }
void jr_hr_extra(void* h, const float* e, const float* r) { (void)h; (void)e; (void)r; }
void jr_hr_base(void* h, const float* b) { (void)h; (void)b; }
void jr_car_add(void* h, const float* r, const float* p, float oy) { (void)h; (void)r; (void)p; (void)oy; }
void jr_car_move(void* h, const float* r, const float* p) { (void)h; (void)r; (void)p; }
void jr_car_remove(void* h) { (void)h; }
/* v1075R balance assist (gf_ragdoll_bhv.c) */
void jr_assist(void* h, int part, const float* p, const float* t, float kp, float kd, const float* ff) { (void)h; (void)part; (void)p; (void)t; (void)kp; (void)kd; (void)ff; }
void jr_part_vel(void* h, int part, const float* p, float* v) { (void)h; (void)part; (void)p; v[0] = v[1] = v[2] = 0.0f; }
void jr_yaw_damp(void* h, float k) { (void)h; (void)k; }
void jr_torque(void* h, int part, const float* tq) { (void)h; (void)part; (void)tq; }
float jr_yaw_rate(void* h, int part) { (void)h; (void)part; return 0.0f; }

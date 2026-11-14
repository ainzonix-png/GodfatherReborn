/* v95 differential harness: run every lifted function of one SPU module in its original form and
 * in its register-cached (spu_recomp_rc.c) form from identical random state, and compare a hash of
 * every externally visible event (channel ops, calls into the runtime, with the FULL ctx + LS at that
 * moment) and the final ctx + LS. */
#define _GNU_SOURCE
#include "spu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
static long TMO=100000;
static double nowns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e9+t.tv_nsec; }

typedef void (*fn_t)(spu_context*);
extern fn_t T_orig[], T_rc[]; extern const char* T_name[]; extern int T_n;

__thread void (*g_spu_trampoline_fn)(spu_context*);
__thread int g_spu_fast_tail = 0;
int g_spu_ls_watch_n = 0;
int g_spu_xfloat = 1; uint32_t g_spu_xflim[8] = { 0xFF7FFFFFu, 0xFF7FFFFFu, 0xFF7FFFFFu, 0xFF7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu };
void spu_ls_watch_slow(uint32_t a, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {(void)a;(void)w;(void)p;(void)pc;(void)lr;}
void spu_register_function(uint32_t a, void (*f)(spu_context*)) {(void)a;(void)f;}

static uint64_t H, K, SEED; static int EV; static sigjmp_buf JB; static int VAR;
static uint8_t* LS; static spu_context* C;
static uint64_t sm(uint64_t* s){ uint64_t z=(*s+=0x9E3779B97F4A7C15ull); z=(z^(z>>30))*0xBF58476D1CE4E5B9ull; z=(z^(z>>27))*0x94D049BB133111EBull; return z^(z>>31); }
static void mix(const void* p, size_t n){ const uint8_t* b=p; uint64_t h=H; size_t i=0;
  for (; i+8<=n; i+=8){ uint64_t w; memcpy(&w,b+i,8); h=(h^w)*0x100000001B3ull; h^=h>>29; }
  for (; i<n; i++){ h=(h^b[i])*0x100000001B3ull; } H=h; }
static long fidx(fn_t f){ if(!f) return -1; fn_t* T = VAR? T_rc : T_orig; for (int i=0;i<T_n;i++) if (T[i]==f) return i; return -2; }
static void snap(void){ /* full observable state */
  fn_t save = g_spu_trampoline_fn; long ti = fidx(save);
  mix(&ti, sizeof ti); mix(C, sizeof *C); mix(LS, SPU_LS_SIZE); }
static void ev(uint64_t kind, uint64_t a){ EV++; mix(&kind,8); mix(&a,8); snap();
  if (EV > 400) siglongjmp(JB, 3); }
static u128 rnd128(void){ u128 v; uint64_t a=sm(&K), b=sm(&K); memcpy(&v,&a,8); memcpy((char*)&v+8,&b,8); return v; }
static void perturb(void){ /* a callee/runtime may change any register: make sure the cached copy reloads */
  for (int r=2;r<8;r++) C->gpr[r]=rnd128(); C->gpr[0]._u32[0]=(uint32_t)sm(&K)&0x3FFF0; }

void spu_stop(spu_context* c){ ev(1,c->stop_code); siglongjmp(JB,1); }
void spu_halt(spu_context* c){ (void)c; ev(2,0); siglongjmp(JB,1); }
u128 spu_rdch(spu_context* c, uint32_t ch){ (void)c; ev(3,ch); u128 v=rnd128(); if (sm(&K)&1) { memset(&v,0,16); v._u32[0]=(uint32_t)sm(&K)&0xFF; } return v; }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch){ (void)c; ev(4,ch); return (uint32_t)sm(&K)&3; }
void spu_wrch(spu_context* c, uint32_t ch, u128 v){ (void)c; ev(5,ch); mix(&v,16); }
void spu_drain_call(spu_context* c, uint32_t rp){ (void)c; ev(6,rp); g_spu_trampoline_fn=0; perturb(); }
void spu_img_restore(spu_context* c, int32_t si){ (void)c; ev(7,(uint32_t)si); }
void spu_indirect_branch(spu_context* c){ ev(8,c->pc); perturb(); }
void spu_indirect_branch_mt(spu_context* c){ ev(9,c->pc); perturb(); }

static void on_sig(int s){ siglongjmp(JB, s==SIGALRM ? 4 : 5); }

static void init_state(uint64_t seed, int fi){
  K = seed*1000003ull + (uint64_t)fi*7919ull + 1;
  uint64_t* w=(uint64_t*)LS; for (int i=0;i<SPU_LS_SIZE/8;i++) w[i]=sm(&K);
  /* half the LS as small plausible LS addresses/counters so loads chase real pointers */
  if (seed & 1) for (int i=0;i<SPU_LS_SIZE/4;i+=2) ((uint32_t*)LS)[i] = __builtin_bswap32((uint32_t)sm(&K) & 0x3FFF0);
  memset(C,0,sizeof *C); C->ls=LS;
  for (int r=0;r<128;r++){ C->gpr[r]=rnd128();
    uint64_t m = seed % 4;
    if (m==1 || (m==2 && (r&1))) for (int s=0;s<4;s++) C->gpr[r]._u32[s] &= 0x3FFF0;
    if (m==3) for (int s=0;s<4;s++) C->gpr[r]._u32[s] &= 0xFF; }
  C->gpr[1]._u32[0] = 0x3E000;  /* stack */
  C->pc = 0; g_spu_trampoline_fn = 0; H = 0xcbf29ce484222325ull; EV = 0; }

static int run1(int fi, uint64_t seed, int var, uint64_t* out, long tmo){
  int fd[2]; if (pipe(fd)) return -1;
  pid_t p = fork();
  if (p==0){ close(fd[0]);
    static char alt[1<<16]; stack_t ss={.ss_sp=alt,.ss_size=sizeof alt}; sigaltstack(&ss,0);
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=on_sig; sa.sa_flags=SA_ONSTACK; sigaction(SIGALRM,&sa,0); sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0); sigaction(SIGFPE,&sa,0); sigaction(SIGILL,&sa,0);
    VAR=var; init_state(seed, fi);
    int how = sigsetjmp(JB,1);
    double t0=nowns(); struct itimerval it={{0,0},{tmo/1000000, tmo%1000000}};
    if (!how){ setitimer(ITIMER_REAL,&it,0); (var?T_rc:T_orig)[fi](C); how = 0; }
    struct itimerval z={{0,0},{0,0}}; setitimer(ITIMER_REAL,&z,0); double dt=nowns()-t0;
    uint64_t res[4]; if (how==0||how==1||how==3) snap(); res[0]=H; res[1]=(uint64_t)how; res[2]=(uint64_t)EV; res[3]=(uint64_t)dt;
    if (write(fd[1],res,sizeof res)<0){} _exit(0); }
  close(fd[1]); uint64_t res[4]={0,99,0,0}; ssize_t n=read(fd[0],res,sizeof res); close(fd[0]); int st; waitpid(p,&st,0);
  if (n != sizeof res) { res[1]=98; }
  out[0]=res[0]; out[1]=res[1]; out[2]=res[2]; out[3]=res[3]; return 0; }

int main(int argc, char** argv){
  int seeds = argc>1 ? atoi(argv[1]) : 4; int only = argc>2 ? atoi(argv[2]) : -1;
  LS = aligned_alloc(64, SPU_LS_SIZE); C = aligned_alloc(64, (sizeof(spu_context)+63)/64*64);
  long same=0, diff=0, skipped=0, normal=0, retried=0; double ta=0, tb=0;
  if (getenv("TMO_US")) TMO = atol(getenv("TMO_US"));
  for (int fi=0; fi<T_n; fi++){ if (only>=0 && fi!=only) continue;
    for (int s=0;s<seeds;s++){ uint64_t a[4], b[4]; run1(fi,s,0,a,TMO); run1(fi,s,1,b,TMO);
      if ((a[1]==4) != (b[1]==4)) { retried++; run1(fi,s,0,a,TMO*30); run1(fi,s,1,b,TMO*30); }
      if (a[1]>=4 && b[1]>=4) { skipped++; continue; }   /* both hit timeout/crash: nothing comparable */
      if (a[0]==b[0] && a[1]==b[1] && a[2]==b[2]) { same++; if (a[1]<=1) { normal++; if (a[2]==0) { ta+=a[3]; tb+=b[3]; } } }
      else { diff++; if (diff<=20) printf("DIFF %s seed %d: orig how=%lu ev=%lu  rc how=%lu ev=%lu\n", T_name[fi], s, a[1],a[2],b[1],b[2]); } } }
  printf("RESULT funcs=%d seeds=%d same=%ld (normal-exit %ld) diff=%ld skipped=%ld retried=%ld time orig %.1f ms rc %.1f ms\n", T_n, seeds, same, normal, diff, skipped, retried, ta/1e6, tb/1e6);
  return diff?1:0; }

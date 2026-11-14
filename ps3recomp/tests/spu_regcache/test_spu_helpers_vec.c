#include "spu_context.h"
#include "spu_helpers.h"
#include <stdio.h>
static u128 ref_shl(u128 a,u128 b){return spu_shl(a,b);}
static u128 ref_rotm(u128 a,u128 b){return spu_rotm(a,b);}
static u128 ref_rotma(u128 a,u128 b){return spu_rotma(a,b);}
static u128 ref_rot(u128 a,u128 b){return spu_rot(a,b);}
static u128 ref_splat_u32(uint32_t i){return spu_splat_u32(i);}
static u128 ref_il(int32_t i){return spu_il(i);}
static u128 ref_ila(uint32_t i){return spu_ila(i);}
static u128 ref_ilhu(uint16_t i){return spu_ilhu(i);}
static u128 ref_ilh(uint16_t i){return spu_ilh(i);}
static u128 ref_link(uint32_t i){return spu_link(i);}
static u128 ref_pref_u32(uint32_t i){return spu_pref_u32(i);}
static u128 ref_iohl(u128 a,uint16_t i){return spu_iohl(a,i);}
static u128 ref_a(u128 a,u128 b){return spu_a(a,b);}
static u128 ref_sf(u128 a,u128 b){return spu_sf(a,b);}
static u128 ref_ah(u128 a,u128 b){return spu_ah(a,b);}
static u128 ref_sfh(u128 a,u128 b){return spu_sfh(a,b);}
static u128 ref_ai(u128 a,int32_t i){return spu_ai(a,i);}
static u128 ref_sfi(u128 a,int32_t i){return spu_sfi(a,i);}
static u128 ref_ahi(u128 a,int32_t i){return spu_ahi(a,i);}
static u128 ref_sfhi(u128 a,int32_t i){return spu_sfhi(a,i);}
static u128 ref_addx(u128 a,u128 b,u128 c){return spu_addx(a,b,c);}
static u128 ref_sfx(u128 a,u128 b,u128 c){return spu_sfx(a,b,c);}
static u128 ref_cg(u128 a,u128 b){return spu_cg(a,b);}
static u128 ref_bg(u128 a,u128 b){return spu_bg(a,b);}
static u128 ref_and(u128 a,u128 b){return spu_and(a,b);}
static u128 ref_or(u128 a,u128 b){return spu_or(a,b);}
static u128 ref_xor(u128 a,u128 b){return spu_xor(a,b);}
static u128 ref_andc(u128 a,u128 b){return spu_andc(a,b);}
static u128 ref_orc(u128 a,u128 b){return spu_orc(a,b);}
static u128 ref_nor(u128 a,u128 b){return spu_nor(a,b);}
static u128 ref_nand(u128 a,u128 b){return spu_nand(a,b);}
static u128 ref_eqv(u128 a,u128 b){return spu_eqv(a,b);}
static u128 ref_selb(u128 a,u128 b,u128 c){return spu_selb(a,b,c);}
static u128 ref_andi(u128 a,int32_t i){return spu_andi(a,i);}
static u128 ref_ori(u128 a,int32_t i){return spu_ori(a,i);}
static u128 ref_xori(u128 a,int32_t i){return spu_xori(a,i);}
static u128 ref_andhi(u128 a,int32_t i){return spu_andhi(a,i);}
static u128 ref_orhi(u128 a,int32_t i){return spu_orhi(a,i);}
static u128 ref_xorhi(u128 a,int32_t i){return spu_xorhi(a,i);}
static u128 ref_andbi(u128 a,int32_t i){return spu_andbi(a,i);}
static u128 ref_orbi(u128 a,int32_t i){return spu_orbi(a,i);}
static u128 ref_xorbi(u128 a,int32_t i){return spu_xorbi(a,i);}
static u128 ref_ceq(u128 a,u128 b){return spu_ceq(a,b);}
static u128 ref_ceqh(u128 a,u128 b){return spu_ceqh(a,b);}
static u128 ref_ceqb(u128 a,u128 b){return spu_ceqb(a,b);}
static u128 ref_cgt(u128 a,u128 b){return spu_cgt(a,b);}
static u128 ref_clgt(u128 a,u128 b){return spu_clgt(a,b);}
static u128 ref_clgth(u128 a,u128 b){return spu_clgth(a,b);}
static u128 ref_ceqi(u128 a,int32_t i){return spu_ceqi(a,i);}
static u128 ref_ceqhi(u128 a,int32_t i){return spu_ceqhi(a,i);}
static u128 ref_ceqbi(u128 a,int32_t i){return spu_ceqbi(a,i);}
static u128 ref_cgti(u128 a,int32_t i){return spu_cgti(a,i);}
static u128 ref_cgthi(u128 a,int32_t i){return spu_cgthi(a,i);}
static u128 ref_cgtbi(u128 a,int32_t i){return spu_cgtbi(a,i);}
static u128 ref_clgti(u128 a,int32_t i){return spu_clgti(a,i);}
static u128 ref_clgthi(u128 a,int32_t i){return spu_clgthi(a,i);}
static u128 ref_clgtbi(u128 a,int32_t i){return spu_clgtbi(a,i);}
static u128 ref_shli(u128 a,int i){return spu_shli(a,i);}
static u128 ref_shlhi(u128 a,int i){return spu_shlhi(a,i);}
static u128 ref_rotmi(u128 a,int i){return spu_rotmi(a,i);}
static u128 ref_rotmai(u128 a,int i){return spu_rotmai(a,i);}
static u128 ref_rothmi(u128 a,int i){return spu_rothmi(a,i);}
static u128 ref_rotmhi(u128 a,int i){return spu_rotmhi(a,i);}
static u128 ref_rotmahi(u128 a,int i){return spu_rotmahi(a,i);}
static u128 ref_roti(u128 a,int i){return spu_roti(a,i);}
static u128 ref_xsbh(u128 a){return spu_xsbh(a);}
static u128 ref_xshw(u128 a){return spu_xshw(a);}
static u128 ref_mpy(u128 a,u128 b){return spu_mpy(a,b);}
static u128 ref_mpyu(u128 a,u128 b){return spu_mpyu(a,b);}
static u128 ref_mpyh(u128 a,u128 b){return spu_mpyh(a,b);}
static u128 ref_mpyhh(u128 a,u128 b){return spu_mpyhh(a,b);}
static u128 ref_mpys(u128 a,u128 b){return spu_mpys(a,b);}
static u128 ref_mpya(u128 a,u128 b,u128 c){return spu_mpya(a,b,c);}
static u128 ref_mpyi(u128 a,int32_t i){return spu_mpyi(a,i);}
static u128 ref_mpyui(u128 a,int32_t i){return spu_mpyui(a,i);}
static u128 ref_fsm(u128 a){return spu_fsm(a);}
#include "spu_helpers_vec.h"
static uint64_t S=88172645463325252ull; static uint64_t xr(void){ S^=S<<13; S^=S>>7; S^=S<<17; return S; }
static const uint32_t E[]={0,1,2,0x7FFFFFFF,0x80000000,0xFFFFFFFF,0x8000,0x7FFF,0xFFFF,0x10000,0xFFFF8000,0x80,0x7F,0xFF,31,32,33,63,64,0xFFFFFFE0,0xFFFFFFC1,15,16,17};
static uint32_t rw(void){ uint64_t r=xr(); switch(r&7){case 0:return E[(r>>8)%(sizeof E/4)]; case 1:return (uint32_t)(r>>32)&0x7F; case 2:return (uint32_t)-(int)((r>>32)&0x7F); case 3:{uint32_t lo=E[(r>>8)%(sizeof E/4)]&0xFFFF, hi=E[(r>>20)%(sizeof E/4)]&0xFFFF; return hi<<16|lo;} default:return (uint32_t)(r>>16);} }
static u128 rv(void){ u128 v; for(int i=0;i<4;i++) v._u32[i]=rw(); return v; }
static long bad=0, tot=0;
static void chk(const char* n, u128 x, u128 y){ tot++; if(memcmp(&x,&y,16)){ if(bad++<20) printf("MISMATCH %s %08X %08X %08X %08X vs %08X %08X %08X %08X\n",n,x._u32[0],x._u32[1],x._u32[2],x._u32[3],y._u32[0],y._u32[1],y._u32[2],y._u32[3]); } }
int g_spu_xfloat=1; uint32_t g_spu_xflim[8] = { 0xFF7FFFFFu, 0xFF7FFFFFu, 0xFF7FFFFFu, 0xFF7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu }; int g_spu_ls_watch_n=0; void spu_ls_watch_slow(uint32_t a,int w,const uint8_t*p,uint32_t pc,uint32_t lr){}
int main(void){ const int N=300000;
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("shl",ref_shl(a,b),spu__v_shl(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("rotm",ref_rotm(a,b),spu__v_rotm(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("rotma",ref_rotma(a,b),spu__v_rotma(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("rot",ref_rot(a,b),spu__v_rot(a,b));}
for(int j=0;j<N;j++){uint32_t i=(uint32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("splat_u32",ref_splat_u32(i),spu__v_splat_u32(i));}
for(int j=0;j<N;j++){int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("il",ref_il(i),spu__v_il(i));}
for(int j=0;j<N;j++){uint32_t i=(uint32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("ila",ref_ila(i),spu__v_ila(i));}
for(int j=0;j<N;j++){uint16_t i=(uint16_t)((j<4096)?(j-2048):(int64_t)rw()); chk("ilhu",ref_ilhu(i),spu__v_ilhu(i));}
for(int j=0;j<N;j++){uint16_t i=(uint16_t)((j<4096)?(j-2048):(int64_t)rw()); chk("ilh",ref_ilh(i),spu__v_ilh(i));}
for(int j=0;j<N;j++){uint32_t i=(uint32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("link",ref_link(i),spu__v_link(i));}
for(int j=0;j<N;j++){uint32_t i=(uint32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("pref_u32",ref_pref_u32(i),spu__v_pref_u32(i));}
for(int j=0;j<N;j++){u128 a=rv(); uint16_t i=(uint16_t)((j<4096)?(j-2048):(int64_t)rw()); chk("iohl",ref_iohl(a,i),spu__v_iohl(a,i));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("a",ref_a(a,b),spu__v_a(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("sf",ref_sf(a,b),spu__v_sf(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("ah",ref_ah(a,b),spu__v_ah(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("sfh",ref_sfh(a,b),spu__v_sfh(a,b));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("ai",ref_ai(a,i),spu__v_ai(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("sfi",ref_sfi(a,i),spu__v_sfi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("ahi",ref_ahi(a,i),spu__v_ahi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("sfhi",ref_sfhi(a,i),spu__v_sfhi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(),c=rv(); chk("addx",ref_addx(a,b,c),spu__v_addx(a,b,c));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(),c=rv(); chk("sfx",ref_sfx(a,b,c),spu__v_sfx(a,b,c));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("cg",ref_cg(a,b),spu__v_cg(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("bg",ref_bg(a,b),spu__v_bg(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("and",ref_and(a,b),spu__v_and(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("or",ref_or(a,b),spu__v_or(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("xor",ref_xor(a,b),spu__v_xor(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("andc",ref_andc(a,b),spu__v_andc(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("orc",ref_orc(a,b),spu__v_orc(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("nor",ref_nor(a,b),spu__v_nor(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("nand",ref_nand(a,b),spu__v_nand(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("eqv",ref_eqv(a,b),spu__v_eqv(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(),c=rv(); chk("selb",ref_selb(a,b,c),spu__v_selb(a,b,c));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("andi",ref_andi(a,i),spu__v_andi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("ori",ref_ori(a,i),spu__v_ori(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("xori",ref_xori(a,i),spu__v_xori(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("andhi",ref_andhi(a,i),spu__v_andhi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("orhi",ref_orhi(a,i),spu__v_orhi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("xorhi",ref_xorhi(a,i),spu__v_xorhi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("andbi",ref_andbi(a,i),spu__v_andbi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("orbi",ref_orbi(a,i),spu__v_orbi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("xorbi",ref_xorbi(a,i),spu__v_xorbi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("ceq",ref_ceq(a,b),spu__v_ceq(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("ceqh",ref_ceqh(a,b),spu__v_ceqh(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("ceqb",ref_ceqb(a,b),spu__v_ceqb(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("cgt",ref_cgt(a,b),spu__v_cgt(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("clgt",ref_clgt(a,b),spu__v_clgt(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("clgth",ref_clgth(a,b),spu__v_clgth(a,b));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("ceqi",ref_ceqi(a,i),spu__v_ceqi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("ceqhi",ref_ceqhi(a,i),spu__v_ceqhi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("ceqbi",ref_ceqbi(a,i),spu__v_ceqbi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("cgti",ref_cgti(a,i),spu__v_cgti(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("cgthi",ref_cgthi(a,i),spu__v_cgthi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("cgtbi",ref_cgtbi(a,i),spu__v_cgtbi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("clgti",ref_clgti(a,i),spu__v_clgti(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("clgthi",ref_clgthi(a,i),spu__v_clgthi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("clgtbi",ref_clgtbi(a,i),spu__v_clgtbi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int i=(int)((j<4096)?(j-2048):(int64_t)rw()); chk("shli",ref_shli(a,i),spu__v_shli(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int i=(int)((j<4096)?(j-2048):(int64_t)rw()); chk("shlhi",ref_shlhi(a,i),spu__v_shlhi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int i=(int)((j<4096)?(j-2048):(int64_t)rw()); chk("rotmi",ref_rotmi(a,i),spu__v_rotmi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int i=(int)((j<4096)?(j-2048):(int64_t)rw()); chk("rotmai",ref_rotmai(a,i),spu__v_rotmai(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int i=(int)((j<4096)?(j-2048):(int64_t)rw()); chk("rothmi",ref_rothmi(a,i),spu__v_rothmi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int i=(int)((j<4096)?(j-2048):(int64_t)rw()); chk("rotmhi",ref_rotmhi(a,i),spu__v_rotmhi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int i=(int)((j<4096)?(j-2048):(int64_t)rw()); chk("rotmahi",ref_rotmahi(a,i),spu__v_rotmahi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int i=(int)((j<4096)?(j-2048):(int64_t)rw()); chk("roti",ref_roti(a,i),spu__v_roti(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); chk("xsbh",ref_xsbh(a),spu__v_xsbh(a));}
for(int j=0;j<N;j++){u128 a=rv(); chk("xshw",ref_xshw(a),spu__v_xshw(a));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("mpy",ref_mpy(a,b),spu__v_mpy(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("mpyu",ref_mpyu(a,b),spu__v_mpyu(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("mpyh",ref_mpyh(a,b),spu__v_mpyh(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("mpyhh",ref_mpyhh(a,b),spu__v_mpyhh(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(); chk("mpys",ref_mpys(a,b),spu__v_mpys(a,b));}
for(int j=0;j<N;j++){u128 a=rv(),b=rv(),c=rv(); chk("mpya",ref_mpya(a,b,c),spu__v_mpya(a,b,c));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("mpyi",ref_mpyi(a,i),spu__v_mpyi(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); int32_t i=(int32_t)((j<4096)?(j-2048):(int64_t)rw()); chk("mpyui",ref_mpyui(a,i),spu__v_mpyui(a,i));}
for(int j=0;j<N;j++){u128 a=rv(); chk("fsm",ref_fsm(a),spu__v_fsm(a));}
printf("helpers=%d checks=%ld mismatches=%ld\n", 76, tot, bad); return bad?1:0; }

import struct, numpy as np
from PIL import Image
def c565(c):
    return np.array([((c>>11)&31)*255//31, ((c>>5)&63)*255//63, (c&31)*255//31],dtype=np.int32)
def dxt_color(blk, img, x, y, has_alpha_block, alpha):
    c0,c1=struct.unpack('<HH',blk[0:2]+blk[2:4]); bits=struct.unpack('<I',blk[4:8])[0]
    a=c565(c0); b=c565(c1)
    if c0>c1 or has_alpha_block: pal=[a,b,(2*a+b)//3,(a+2*b)//3]; tr=False
    else: pal=[a,b,(a+b)//2,np.array([0,0,0])]; tr=True
    for i in range(16):
        k=(bits>>(2*i))&3
        px=img[y+i//4, x+i%4]; px[:3]=pal[k]
        px[3]= alpha[i] if alpha is not None else (0 if (tr and k==3) else 255)
def decode(path):
    d=open(path,'rb').read(); assert d[:6]==b'GFTEX1'
    w,h,fmt,nb,cs=struct.unpack('<5I',d[8:28]); data=d[28:28+nb]
    base=fmt&0x9F
    img=np.zeros((h,w,4),dtype=np.int32)
    bw,bh=(w+3)//4,(h+3)//4; p=0
    for by in range(bh):
        for bx in range(bw):
            if base==0x86:
                dxt_color(data[p:p+8],img,bx*4,by*4,False,None); p+=8
            elif base==0x87:
                ab=data[p:p+8]; al=[]
                for i in range(16):
                    v=(ab[i//2]>>(4*(i&1)))&15; al.append(v*17)
                dxt_color(data[p+8:p+16],img,bx*4,by*4,True,al); p+=16
            elif base==0x88:
                a0,a1=data[p],data[p+1]; bits=int.from_bytes(data[p+2:p+8],'little')
                if a0>a1: pal=[a0,a1]+[((6-i)*a0+(i+1)*a1)//7 for i in range(6)]
                else: pal=[a0,a1]+[((4-i)*a0+(i+1)*a1)//5 for i in range(4)]+[0,255]
                al=[pal[(bits>>(3*i))&7] for i in range(16)]
                dxt_color(data[p+8:p+16],img,bx*4,by*4,True,al); p+=16
    return w,h,fmt,cs,np.clip(img,0,255).astype(np.uint8)

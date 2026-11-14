def unrefpack(d, p=0):
    hdr = d[p]<<8 | d[p+1]; p+=2
    big = hdr & 0x8000
    if hdr & 0x0100: p += 4 if big else 3
    n = 4 if big else 3
    size = int.from_bytes(d[p:p+n],'big'); p+=n
    out = bytearray()
    while True:
        b0 = d[p]
        if b0 < 0x80:
            b1=d[p+1]; p+=2
            lit = b0&3; out+=d[p:p+lit]; p+=lit
            ln=((b0&0x1c)>>2)+3; off=((b0&0x60)<<3)+b1+1
        elif b0 < 0xC0:
            b1,b2=d[p+1],d[p+2]; p+=3
            lit=b1>>6; out+=d[p:p+lit]; p+=lit
            ln=(b0&0x3f)+4; off=((b1&0x3f)<<8)+b2+1
        elif b0 < 0xE0:
            b1,b2,b3=d[p+1],d[p+2],d[p+3]; p+=4
            lit=b0&3; out+=d[p:p+lit]; p+=lit
            ln=((b0&0x0c)<<6)+b3+5; off=((b0&0x10)<<12)+(b1<<8)+b2+1
        elif b0 < 0xFC:
            lit=((b0&0x1f)<<2)+4; p+=1; out+=d[p:p+lit]; p+=lit; continue
        else:
            lit=b0&3; p+=1; out+=d[p:p+lit]; p+=lit; break
        for i in range(ln): out.append(out[-off])
    return bytes(out), size, p

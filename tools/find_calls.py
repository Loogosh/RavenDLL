import struct, sys
b=open(r'F:\mm_work\WowClassic_dumpimage.exe','rb').read()
e=struct.unpack_from('<I',b,0x3C)[0]; n=struct.unpack_from('<H',b,e+6)[0]; o=e+24; optsz=struct.unpack_from('<H',b,e+20)[0]
sec=o+optsz; text=None
for i in range(n):
    p=sec+i*40; name=b[p:p+8].rstrip(b'\0'); vs,va,rs,ptr=struct.unpack_from('<IIII',b,p+8)
    if name==b'.text': text=(va,vs,ptr)
va,vs,ptr=text; data=b[ptr:ptr+vs]
target=int(sys.argv[1],0)
hits=[]
i=0
while True:
    j=data.find(b'\xe8',i)
    if j<0: break
    if j+5<=len(data):
        rel=struct.unpack_from('<i',data,j+1)[0]
        tgt=va+j+5+rel
        if tgt==target: hits.append(va+j)
    i=j+1
print(f"target 0x{target:X} calls {len(hits)}")
for a in hits: print(f"  0x{a:X}")

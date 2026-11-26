import sys,re,struct,bisect
sys.path.insert(0,'/mnt/project-files/sims4vr/re/exe/tools')
import gen_addrs as g
pe=g.PE(open('/mnt/user-data/uploads/Games--The Sims 4/Game/Bin/TS4_x64.exe','rb').read())
md=g.decoder()
pd=[s for s in pe.secs if s[0]=='.pdata'][0]
ents=[]
for i in range(0,pd[3]-11,12):
    b,e,u=struct.unpack_from('<III',pe.d,pd[4]+i); ents.append((b,e))
ents.sort(); starts=[x[0] for x in ents]
def func_of(rva):
    i=bisect.bisect_right(starts,rva)-1
    return ents[i] if i>=0 and ents[i][0]<=rva<ents[i][1] else None
T=pe.text; base=pe.text_rva
rx=re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]',re.S)
targets={'SubMix':0x22dcea0,'Send':0x22dcf28,'Dac':0x22da5d0,'GainFader':0x22da9b8,'HighPassIir2':0x22daaa8}
refs={k:[] for k in targets}
inv={v:k for k,v in targets.items()}
for m in rx.finditer(T):
    p=m.start()
    if p+7>len(T):continue
    disp=struct.unpack_from('<i',T,p+3)[0]
    t=base+p+7+disp
    if t in inv: refs[inv[t]].append(base+p)
for k,v in refs.items():
    print(k,hex(targets[k]),[hex(x) for x in v])
    best=None
    for r in v:
        f=func_of(r)
        if not f: continue
        size=f[1]-f[0]
        sig,uniq=g.build_sig(md,pe,f[0])
        # find disp offset of lea
        code=pe.read(f[0],0x300); 
        for b,mk,tgt,doff,sz,at in g.masked_insns(md,code,f[0]):
            if tgt==targets[k]:
                print('  fn',hex(f[0]),'size',size,'uniq',uniq,'disp',at+doff,'end',at+sz); print('   ',sig); break
print('---hp')
f=0x1ad3790
code=pe.read(f,400); ins=g.masked_insns(md,code,f)
for n in range(10,len(ins)):
    s=g.sig_text(ins,n)
    h=g.find_all(pe.text,s,5)
    if len(h)==1: print(n,len(s.split()),s);break
else: print('none', len(g.find_all(pe.text,g.sig_text(ins,len(ins)),9)))
print('---proc')
for nm,f,slot in (('LowPass',0x1aef230,0x2859b30),('HighShelf',0x1aee640,0x2859660)):
    s,u=g.build_sig(md,pe,f); print(nm,u,s)
    q=struct.pack('<Q',0x140000000+f)
    for sn,vsz,va,rsz,raw in pe.secs:
        d=pe.d[raw:raw+rsz]
        hs=[hex(va+m.start()) for m in re.finditer(re.escape(q),d)]
        if hs: print('  ',sn,hs)
print('---lp')
f=0x1aef230
code=pe.read(f,600); ins=g.masked_insns(md,code,f)
for n in range(10,len(ins)):
    s=g.sig_text(ins,n)
    if len(g.find_all(pe.text,s,5))==1: print(n,len(s.split()),s);break
print('---all')
cls=re.findall(r'\{"(\w+)", 0x([0-9a-f]+)\}',open('/mnt/project-files/sound/src/audio_classes.h').read())
inv={int(v,16):n for n,v in cls}
refs={n:[] for n,_ in cls}
for m in rx.finditer(T):
    p=m.start()
    disp=struct.unpack_from('<i',T,p+3)[0]
    t=base+p+7+disp
    if t in inv: refs[inv[t]].append(base+p)
def bigsig(f,cc=False):
    code=pe.read(f,700); ins=g.masked_insns(md,code,f)
    for k,i in enumerate(ins):
        if i[0]==b'\xcc' and not cc: ins=ins[:k];break
    tot=0
    for n in range(1,len(ins)+1):
        tot+=ins[n-1][4]
        if tot<24: continue
        s=g.sig_text(ins,n)
        if len(g.find_all(pe.text,s,2))==1: return s
    return None
rows=[];miss=[]
for name,v in cls:
    v=int(v,16); best=None
    for r in refs[name]:
        f=func_of(r)
        if not f: continue
        for b,mk,tgt,doff,sz,at in g.masked_insns(md,pe.read(f[0],0x300),f[0]):
            if tgt==v:
                s=bigsig(f[0]) or bigsig(f[0],True)
                if s and (best is None or f[1]-f[0] < best[0]): best=(f[1]-f[0],s,at+doff,at+sz)
                break
    if best: rows.append((name,v,best))
    else: miss.append(name)
print('ok',len(rows),'miss',miss)
out=['// Generated from the 1.128.90.1030 exe (stamp 0x6aa08454) by sound/tools/gen_audio_sigs.py. Do not edit.',
 '// Per EA::Audio::Core class: a function that loads the class vtable with a rip-relative lea (a constructor),',
 '// its masked signature (?? = wildcard), and the lea displacement offset / instruction end inside that function.',
 'struct AudioSig { const char* name; uint32_t vtable128; const char* sig; uint32_t dispOff, endOff; };',
 'constexpr AudioSig kAudioSigs[] = {']
for name,v,(sz,s,d,e) in rows:
    out.append('    {"%s", 0x%x, "%s", %d, %d},'%(name,v,s,d,e))
out.append('};')
open('/tmp/claude-0/s/audio_sigs.inc','w').write('\n'.join(out)+'\n')
for name,v,(sz,s,d,e) in rows: print(name,len(s.split()))
print('---slots')
def qa(rva):
    return struct.unpack_from('<Q',pe.read(rva,8))[0]
srows=[]; smiss=[]
need=['HighPassIir2','Compressor1','Expander','HighPassButterworth','HighPassFir64','LowPassButterworth','LowPassFir64','LowPassIir2','Rechannel','UserMusicArbiter']
for name in need:
    v=int(dict(cls)[name],16); done=False
    for k in range(0,12):
        x=qa(v+8*k)
        f=x-0x140000000
        if not (base<=f<base+len(T)): break
        s=bigsig(f)
        if s and len(s.split())>=24:
            # slot must be present in rdata only once at this position: count qword occurrences
            srows.append((name,v,k,s)); done=True;break
    if not done: smiss.append(name)
print(len(srows),smiss)
for r in srows: print(r[0],r[2],len(r[3].split()))
o=open('audio_sigs.inc','a')
o.write('struct AudioSlotSig { const char* name; uint32_t vtable128; uint32_t slot; const char* sig; };\nconstexpr AudioSlotSig kAudioSlotSigs[] = {\n')
for name,v,k,s in srows: o.write('    {"%s", 0x%x, %d, "%s"},\n'%(name,v,k,s))
o.write('};\n')
# filter-process rows
o.write('// Filter process functions the engine calls through plug-in descriptors in .data (Clarity swaps those slots).\n')
o.write('struct ProcSig { const char* name; uint32_t fn128, slot128; const char* sig; };\nconstexpr ProcSig kProcSigs[] = {\n')
for nm,f,slot in (('LowPassIir2',0x1aef230,0x2859b30),('HighShelfIir2',0x1aee640,0x2859660)):
    s=bigsig(f); print(nm,bool(s)); o.write('    {"%s", 0x%x, 0x%x, "%s"},\n'%(nm,f,slot,s))
o.write('};\n')

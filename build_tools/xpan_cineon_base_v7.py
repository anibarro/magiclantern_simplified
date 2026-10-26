#!/usr/bin/env python3
"""XPan EOS M Cineon base for the v7 render (JPEG_OBWB -2 EV + Canon HTP tone/colour set).
Inverse = parametric model (common curve T, matrix M, level saturation k; fit on chart
bracket IMG_9422..9426) x smooth 9^3 log-correction field fitted on all pixels of
IMG_9421..9426 (chart + warm scene; leave-one-out colour bias mostly <0.01 EV).
Then X<=1 (RAW green white), ForwardMatrix2 -> XYZ D50 -> Bradford -> Rec.709, Cineon log.
Usage: xpan_cineon_base_v7.py model7.npy corr7.npy OUT.CUBE [stops] [stepcomp.npy]
stops = JPEG_OBWB gain reduction used by the camera (default 2; the model was
fitted at 2). A linear pre-gain shifts the render exactly (Test68), so at
stops s the scene value is inv(J) / 2**(2-s)."""
import sys, numpy as np
M,kn,T,ks,k=[np.asarray(a,float) for a in np.load(sys.argv[1],allow_pickle=True)]
Cc=np.load(sys.argv[2]); G=round(Cc.shape[0]**(1/3))
wl=np.array([0.2126,0.7152,0.0722]); Minv=np.linalg.inv(M)
FM2=np.array([7397,1199,1047,2650,9355,-2005,193,-2113,10171]).reshape(3,3)/10000
BRAD=np.array([[0.9555766,-0.0230393,0.0631636],[-0.0282895,1.0099416,0.0210077],[0.0122982,-0.0204830,1.3299098]])
XYZ709=np.array([[3.2404542,-1.5371385,-0.4985314],[-0.9692660,1.8760108,0.0415560],[0.0556434,-0.2040259,1.0572252]])
C=XYZ709@BRAD@FM2
def inv0(J):
    Zp=2**np.interp(J,T+np.arange(len(T))*1e-6,kn); L=np.clip(Zp@wl,1e-9,None); kk=np.interp(np.log2(L),ks,k)
    return np.clip((L[:,None]+(Zp-L[:,None])/kk[:,None])@Minv.T,1e-9,None)
def corr(J):
    idx=np.clip(J/255*(G-1),0,G-1-1e-6); i0=np.floor(idx).astype(int); f=idx-i0; out=np.zeros((len(J),3))
    for dr in (0,1):
        for dg in (0,1):
            for db in (0,1):
                w=(f[:,0] if dr else 1-f[:,0])*(f[:,1] if dg else 1-f[:,1])*(f[:,2] if db else 1-f[:,2])
                out+=w[:,None]*Cc[((i0[:,0]+dr)*G+(i0[:,1]+dg))*G+(i0[:,2]+db)]
    return out
gain=0.18*2**3.5; b=10**((95-685)*0.002/0.6)
def cineon(l): return np.clip((685+300*np.log10(np.clip(l,0,None)*(1-b)+b))/1023,0,1)
N=33; g=np.linspace(0,255,N); Bg,Gg,Rg=np.meshgrid(g,g,g,indexing='ij')
J=np.stack([Rg.ravel(),Gg.ravel(),Bg.ravel()],-1)
stops=int(sys.argv[4]) if len(sys.argv)>4 else 2
X=np.clip(inv0(J)*2**corr(J)/2.0**(2-stops),0,1.0); X[(J<0.5).all(1)]=0
out=cineon((X@C.T)*gain)
if len(sys.argv)>5:
    sk,so=np.load(sys.argv[5],allow_pickle=True)
    for ch in range(3): out[:,ch]=np.clip(out[:,ch]+np.interp(J[:,ch],sk,so[ch])/255,0,1)
L=['TITLE "XPan EOS M Cineon log base v7 (full HTP render, pixel-calibrated, FM2 Rec709, JPEG_OBWB -%d EV%s)"'%(stops,', table-step compensation' if len(sys.argv)>5 else ''),
   'LUT_3D_SIZE %d'%N,'DOMAIN_MIN 0 0 0','DOMAIN_MAX 1 1 1']+['%.6f %.6f %.6f'%tuple(o) for o in out]
open(sys.argv[3],'w').write('\n'.join(L)+'\n'); print('written',sys.argv[3])

#!/usr/bin/env python3
"""Send the same random sequences to xterm and to bssh_emulator and compare
every cell and the cursor (project.md sec 7). Needs Xvfb, xterm and the built
test binary. Stops at TIME_LIMIT seconds a set; a difference is shrunk to the
shortest sequence that shows it, and BASE and MODES must have none."""
import os,random,re,shutil,signal,socket,subprocess,sys,tempfile,time
COLS,ROWS=20,8
here=tempfile.mkdtemp(prefix="check-xterm-")
root=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSI='\x1b['
def n(r,a=0,b=None): return str(r.randint(a,COLS+2 if b is None else b))
def rowp(r): return str(r.randint(1,ROWS+1))
def colp(r): return str(r.randint(1,COLS+1))
BASE={
	'text':lambda r:''.join(r.choice('abcdefgh') for _ in range(r.randint(1,COLS+3))),
	'fill':lambda r:CSI+str(COLS-2)+'G'+'abc','cr':lambda r:'\r','lf':lambda r:'\n','bs':lambda r:'\b','ht':lambda r:'\t','vt':lambda r:'\x0b','ff':lambda r:'\x0c',
	'cuu':lambda r:CSI+n(r,0,ROWS+1)+'A','cud':lambda r:CSI+n(r,0,ROWS+1)+'B','cuf':lambda r:CSI+n(r)+'C','cub':lambda r:CSI+n(r)+'D',
	'cnl':lambda r:CSI+n(r,0,ROWS+1)+'E','cpl':lambda r:CSI+n(r,0,ROWS+1)+'F','cha':lambda r:CSI+colp(r)+'G',
	'cup':lambda r:CSI+rowp(r)+';'+colp(r)+'H','hvp':lambda r:CSI+rowp(r)+';'+colp(r)+'f',
	'vpa':lambda r:CSI+rowp(r)+'d','vpr':lambda r:CSI+n(r,0,ROWS)+'e','hpa':lambda r:CSI+colp(r)+'`','hpr':lambda r:CSI+n(r)+'a',
	'ed':lambda r:CSI+r.choice('012')+'J','el':lambda r:CSI+r.choice('012')+'K',
	'ich':lambda r:CSI+n(r)+'@','dch':lambda r:CSI+n(r)+'P','il':lambda r:CSI+n(r,0,ROWS+1)+'L','dl':lambda r:CSI+n(r,0,ROWS+1)+'M',
	'ech':lambda r:CSI+n(r)+'X','su':lambda r:CSI+n(r,0,ROWS+1)+'S','sd':lambda r:CSI+n(r,1,ROWS+1)+'T',
	'rep':lambda r:'x'+CSI+n(r)+'b','cbt':lambda r:CSI+n(r,0,4)+'Z','cht':lambda r:CSI+n(r,0,4)+'I',
	'ind':lambda r:'\x1bD','ri':lambda r:'\x1bM','nel':lambda r:'\x1bE','hts':lambda r:'\x1bH','tbc':lambda r:CSI+r.choice('03')+'g',
	'decsc':lambda r:'\x1b7','decrc':lambda r:'\x1b8','scosc':lambda r:CSI+'s','scorc':lambda r:CSI+'u','decaln':lambda r:'\x1b#8',
}
MODES={
	'stbm':lambda r:CSI+rowp(r)+';'+rowp(r)+'r','stbm0':lambda r:CSI+'r',
	'om':lambda r:CSI+'?6'+r.choice('hl'),'awm':lambda r:CSI+'?7'+r.choice('hl'),
	'irm':lambda r:CSI+'4'+r.choice('hl'),'lnm':lambda r:CSI+'20'+r.choice('hl'),
	'decstr':lambda r:CSI+'!p',
}
LR={
	'lrmm':lambda r:CSI+'?69'+r.choice('hl'),'slrm':lambda r:CSI+colp(r)+';'+colp(r)+'s',
	'decbi':lambda r:'\x1b6','decfi':lambda r:'\x1b9',
	'decic':lambda r:CSI+n(r)+"'}",'decdc':lambda r:CSI+n(r)+"'~",'sl':lambda r:CSI+n(r)+' @','sr':lambda r:CSI+n(r)+' A',
}
ALT={'alt47':lambda r:CSI+'?47'+r.choice('hl'),'alt1047':lambda r:CSI+'?1047'+r.choice('hl'),'alt1048':lambda r:CSI+'?1048'+r.choice('hl'),'alt1049':lambda r:CSI+'?1049'+r.choice('hl')}
WIDE={'wide':lambda r:''.join(r.choice('日本語') for _ in range(r.randint(1,COLS//2+2))),
      'mix':lambda r:r.choice(['a日b','日a','ab本'])}
CHARSET={'g0':lambda r:'\x1b('+r.choice('0B'),'g1':lambda r:'\x1b)'+r.choice('0B'),'so':lambda r:'\x0e','si':lambda r:'\x0f',
	'gl':lambda r:'lqkx`a','ls2':lambda r:'\x1bn','ls3':lambda r:'\x1bo','g2':lambda r:'\x1b*0','g3':lambda r:'\x1b+0','ss2':lambda r:'\x1bNq','ss3':lambda r:'\x1bOq'}
SGR={'sgr':lambda r:CSI+r.choice(['0','1','4','5','7','22','24','25','27','1;4','7;5',''])+'m'}
SETS={'sgr':{**BASE,**MODES,**SGR},'base':BASE,'modes':{**BASE,**MODES},'lr':{**BASE,**MODES,**LR},'alt':{**BASE,**MODES,**ALT},'wide':{**BASE,**MODES,**WIDE},'charset':{**BASE,**CHARSET}}

BRIDGE = """
import os, select, socket, sys, tty
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
tty.setraw(0)
while True:
	r, _, _ = select.select([0, s], [], [])
	if 0 in r:
		d = os.read(0, 65536)
		if not d:
			break
		s.sendall(d)
	if s in r:
		d = s.recv(65536)
		if not d:
			break
		os.write(1, d)
"""

class term:
	def __init__(s,name,argv,env):
		path=os.path.join(here,name+'.sock')
		if os.path.exists(path): os.unlink(path)
		l=socket.socket(socket.AF_UNIX); l.bind(path); l.listen(1); l.settimeout(20)
		s.proc=subprocess.Popen(argv+['python3','-c',BRIDGE,path],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
		s.sock,_=l.accept(); s.sock.settimeout(5); s.buf=b''; s.name=name
	def send(s,b): s.sock.sendall(b)
	def until(s,pat):
		end=time.time()+5
		while True:
			m=pat.search(s.buf)
			if m: s.buf=s.buf[m.end():]; return m
			if time.time()>end: raise TimeoutError(s.name+' '+repr(s.buf[-60:]))
			s.buf+=s.sock.recv(65536)
	def read(s,data):
		s.send(data.encode('utf-8'))
		s.send(b'\x1b[6n')
		m=s.until(re.compile(rb'\x1b\[(\d+);(\d+)R')); cur=(int(m[1]),int(m[2]))
		req=b''.join(b'\x1b[%d;0;%d;%d;%d;%d*y'%(1000+y*COLS+x,y+1,x+1,y+1,x+1) for y in range(ROWS) for x in range(COLS))
		s.send(b'\x1b[?6l'+req)
		grid=[[0]*COLS for _ in range(ROWS)]
		pat=re.compile(rb'\x1bP(\d+)!~([0-9A-Fa-f]{4,})\x1b\\')
		for _ in range(ROWS*COLS):
			m=s.until(pat); i=int(m[1])-1000; v=int(m[2],16)
			grid[i//COLS][i%COLS]=32 if v==0 else v
		return cur,grid

RESET='\x1b[?1049l\x1b[?47h\x1b[2J\x1b[H\x1b7\x1b[?47l\x1b[?69l\x1bc\x1b[r\x1b[?6l\x1b[?7h\x1b[4l\x1b[20l\x1b[3g'+''.join('\x1b[1;%dH\x1bH'%c for c in range(9,COLS+1,8))+'\x1b(B\x1b)B\x0f\x1b[2J\x1b[H'
def show(g): return '\n'.join(''.join(chr(c) if c<0x110000 and c>=32 else '?' for c in row) for row in g)
def run(terms,ops):
	return [t.read(RESET+''.join(ops)) for t in terms]
def differs(terms,ops):
	a,b=run(terms,ops); return a!=b
def minimise(terms,ops):
	cur=list(ops); k=max(1,len(cur)//2)
	while k>=1:
		i=0
		while i<len(cur):
			trial=cur[:i]+cur[i+k:]
			if trial and differs(terms,trial): cur=trial
			else: i+=k
		k//=2
	return cur
TIME_LIMIT = 60
MUST_BE_CLEAN = ('sgr', 'base', 'modes')

def run_set(which, seed, terms):
	found = {}
	done = 0
	table = SETS[which]
	names = list(table)
	deadline = time.time() + TIME_LIMIT
	while time.time() < deadline and done < 2000:
		r = random.Random(seed * 100003 + done)
		ops = [table[r.choice(names)](r) for _ in range(r.randint(2, 14))]
		done += 1
		if differs(terms, ops):
			m = minimise(terms, ops)
			a, c = run(terms, m)
			key = tuple(re.sub(r'[0-9]+', 'N', re.sub('[a-h]+', 'T', o)) for o in m)
			if key not in found:
				found[key] = m
				print('--', repr(m))
				print('xterm', a[0]); print(show(a[1]))
				print('bssh ', c[0]); print(show(c[1]), flush=True)
	print('%s: %d sequences, %d differences' % (which, done, len(found)), flush=True)
	return len(found)

def main():
	seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
	display = ':%d' % (7000 + os.getpid() % 2000)
	server = subprocess.Popen(['Xvfb', display, '-screen', '0', '800x600x24'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
	terms = []
	failed = 0
	try:
		time.sleep(1)
		env = dict(os.environ, LC_ALL='C.UTF-8', DISPLAY=display, TERM='xterm-256color',
		           QT_QPA_PLATFORM='offscreen', BSSH_TEST_PTY_HOST='%dx%d' % (COLS, ROWS))
		terms.append(term('xterm', ['xterm', '-u8', '-ti', 'vt420', '-tn', 'xterm-256color', '-geometry', '%dx%d' % (COLS, ROWS),
		                            '-sl', '0', '-xrm', 'XTerm*checksumExtension: 53', '-xrm', 'XTerm*allowWindowOps: true',
		                            '-xrm', 'XTerm*decTerminalID: 420', '-e'], env))
		terms.append(term('bssh', [os.path.join(root, 'build-tests', 'beerssh_test')], env))
		for which in (sys.argv[2:] or SETS):
			count = run_set(which, seed, terms)
			if which in MUST_BE_CLEAN:
				failed += count
	finally:
		for t in terms:
			try:
				os.killpg(t.proc.pid, signal.SIGTERM)
			except OSError:
				pass
		server.terminate()
		server.wait()
		shutil.rmtree(here, ignore_errors=True)
	return 1 if failed else 0

sys.exit(main())

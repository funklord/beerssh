#!/usr/bin/python3 -I
"""The client half of check-session.sh: the session protocol spoken over a real
ssh process. Every read has a deadline and the sequence is fixed."""
import os
import select
import signal
import subprocess
import sys
import time

key, port = sys.argv[1], sys.argv[2]
SSH = ['ssh', '-i', key, '-p', port, '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null',
	   '-o', 'LogLevel=ERROR', '-o', 'BatchMode=yes', 'fold@127.0.0.1']


def varint(n):
	out = bytearray()
	while True:
		b = n & 0x7f
		n >>= 7
		if n:
			out.append(b | 0x80)
		else:
			out.append(b)
			return bytes(out)


def frame(kind, body):
	return bytes([kind]) + varint(len(body)) + body


def read_varint(b, at):
	n = shift = 0
	while True:
		c = b[at]
		at += 1
		n |= (c & 0x7f) << shift
		shift += 7
		if not c & 0x80:
			return n, at


class link:
	def __init__(self, offset):
		self.p = subprocess.Popen(SSH + ['/tmp/beerssh-session', 'attach', 'e2e'], stdin=subprocess.PIPE,
								  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
		self.buf = b''
		self.text = b''
		self.next = offset
		self.jumps = 0
		self.p.stdin.write(frame(1, varint(offset) + bytes([0, 80, 0, 24])))

	def type(self, text):
		self.p.stdin.write(frame(3, text))

	def wait(self, needle, seconds=15):
		end = time.time() + seconds
		while needle not in self.text:
			left = end - time.time()
			if left <= 0:
				return False
			r, _, _ = select.select([self.p.stdout], [], [], left)
			if not r:
				continue
			chunk = os.read(self.p.stdout.fileno(), 65536)
			if not chunk:
				return needle in self.text
			self.buf += chunk
			self.take()
		return True

	def take(self):
		while len(self.buf) >= 2:
			try:
				length, at = read_varint(self.buf, 1)
			except IndexError:
				return
			if len(self.buf) < at + length:
				return
			kind, body = self.buf[0], self.buf[at:at + length]
			self.buf = self.buf[at + length:]
			if kind == 2:
				off, b = read_varint(body, 0)
				if off != self.next:
					self.jumps += 1
				self.text += body[b:]
				self.next = off + len(body) - b

	def kill(self):
		self.p.send_signal(signal.SIGKILL)
		self.p.wait()


probe = subprocess.run(SSH + ['/tmp/beerssh-session', 'version'], capture_output=True, text=True, timeout=20)
print('probe:', probe.stdout.strip(), 'rc', probe.returncode)
missing = subprocess.run(SSH + ['/tmp/no-such-program', 'version'], capture_output=True, text=True, timeout=20)
print('probe where absent: rc', missing.returncode)

a = link(0)
a.type(b'export MARK=$$; echo "shell=$MARK sta""rted"\n')
assert a.wait(b' started'), a.text
start = a.text.rindex(b'shell=') + 6
pid = a.text[start:a.text.index(b' ', start)].decode()
print('first connection: shell pid', pid)

# A long job, then the connection dies with no goodbye.
a.type(b'(sleep 2; seq 1 5000; echo job-"done":$MARK) &\n')
a.wait(b'never arrives', 1)
held = a.next
a.kill()
print('connection killed at offset', held)
time.sleep(4)

b = link(held)
ok = b.wait(('job-done:' + pid).encode())
print('second connection: output from while away arrived:', ok, 'lines:', b.text.count(b'\n'), 'jumps:', b.jumps)
b.type(b'echo "sa""me=$$"\n')
ok2 = b.wait(('same=' + pid).encode())
print('same shell answers:', ok2)
print('complete:', all(str(i).encode() in b.text for i in (1, 2500, 5000)))
b.type(b'exit\n')
time.sleep(1)
b.kill()
sys.exit(0 if ok and ok2 and not b.jumps else 1)

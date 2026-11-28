import socket,time,json,urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
base='http://127.0.0.1:9494'
for _ in range(30):
 try:
  health=json.load(urllib.request.urlopen(base+'/health',timeout=3));break
 except Exception:time.sleep(1)
else:raise RuntimeError('HTTP not ready')
def request(wire):
 with socket.create_connection(('127.0.0.1',9494),timeout=20) as s:
  s.sendall(wire);return s.recv(4096).split(b'\r\n',1)[0].decode()
results={'health':health}
for name,wire in [('bad-version',b'GET /health HTTP/9.0\r\n\r\n'),('transfer-encoding',b'POST /api/rpc HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n'),('duplicate-length',b'POST /api/rpc HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n')]:
 status=request(wire);assert '400' in status,status;results[name]=status
idle=socket.create_connection(('127.0.0.1',9494),timeout=20);idle.sendall(b'GET /health HTTP/1.1\r\nX-Test: ')
start=time.monotonic()
with ThreadPoolExecutor(max_workers=8) as pool:
 assert all(pool.map(lambda _:json.load(urllib.request.urlopen(base+'/health',timeout=4))['ok'],range(40)))
results['concurrent_health']=40
assert idle.recv(1)==b'';results['idle_closed_seconds']=time.monotonic()-start;assert 13<results['idle_closed_seconds']<18
idle.close()
Path('/home/void/reverse/Linux-android-arm64/reports/upstream-20261128/http-device.json').write_text(json.dumps(results,indent=2))
print(results)

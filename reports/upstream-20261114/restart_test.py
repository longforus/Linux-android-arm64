import json,subprocess,time,urllib.request,re
from pathlib import Path
r=Path('/tmp/lsdriver-fullrom-20261114');a=['adb','-s','21082FDF6007UJ'];receiver='com.android.xsettings/com.longforus.xsettings.XstCommandReceiver'
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
def shell(cmd):return subprocess.check_output(a+['shell',cmd],text=True,timeout=30)
def broadcast(action):return shell('am broadcast -a com.longforus.xsettings.action.'+action+' -n '+receiver+' --include-stopped-packages')
def ready():
 end=time.monotonic()+40
 while time.monotonic()<end:
  try:
   with opener.open('http://127.0.0.1:9494/health',timeout=2) as x:
    health=json.load(x)
   if health.get('ok'):return health
  except Exception:pass
  time.sleep(.5)
 raise RuntimeError('HTTP restart timeout')
result={'before_pid':re.search(r'app=ProcessRecord\{[^}]* (\d+):com\.android\.xsettings/1000',shell('dumpsys activity services com.android.xsettings')).group(1)}
print(broadcast('HARD_STOP'),flush=True);time.sleep(1)
start=time.monotonic();print(broadcast('START_HTTP'),flush=True);result['health']=ready();result['elapsed_seconds']=time.monotonic()-start
result['after_pid']=re.search(r'app=ProcessRecord\{[^}]* (\d+):com\.android\.xsettings/1000',shell('dumpsys activity services com.android.xsettings')).group(1);assert result['before_pid']!=result['after_pid']
request=urllib.request.Request('http://127.0.0.1:9494/api/rpc',json.dumps({'operation':'target.get'}).encode(),{'Content-Type':'application/json'})
with opener.open(request,timeout=10) as x:result['target']=json.load(x)
assert result['target']['data']['pid']==0
result['rotation_restored']=shell('settings get system accelerometer_rotation; settings get system user_rotation').strip().splitlines();assert result['rotation_restored']==['1','0']
f=json.loads((r/'fixture.json').read_text());pid=f['pid'];cmdline=shell('cat /proc/'+str(pid)+'/cmdline').replace('\0',' ')
assert 'lsdriver_integration_fixture' in cmdline;result['fixture_stop']=shell('kill -TERM '+str(pid))
result['passed']=True;(r/'restart-results.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
(r/'aftertest-dmesg.txt').write_text(shell('su -c dmesg'))

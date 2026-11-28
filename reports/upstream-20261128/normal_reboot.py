import json,subprocess,time,urllib.request
from pathlib import Path
r=Path('/home/void/reverse/Linux-android-arm64/reports/upstream-20261128');a=['adb','-s','21082FDF6007UJ'];opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
def run(*args,timeout=30):return subprocess.check_output(a+list(args),text=True,timeout=timeout)
rotation_before=run('shell','settings get system accelerometer_rotation; settings get system user_rotation').strip().splitlines()
run('reboot');print('Ordinary reboot requested',flush=True)
run('wait-for-device',timeout=600)
end=time.monotonic()+600
while time.monotonic()<end:
 try:
  if run('shell','getprop','sys.boot_completed',timeout=10).strip()=='1':break
 except Exception:pass
 time.sleep(3)
else:raise RuntimeError('normal boot timeout')
result={'version':run('shell','getprop','ro.lineage.version').strip(),'slot':run('shell','getprop','ro.boot.slot_suffix').strip(),'selinux':run('shell','getenforce').strip(),'root':run('shell','su','-c','id').strip()}
assert result['version']=='20.0-20261128-RELEASE-oriole' and result['selinux']=='Enforcing' and 'uid=0' in result['root']
end=time.monotonic()+60
while time.monotonic()<end:
 try:
  result['kpm_version']=run('shell','su','-c','/data/adb/ksu/bin/ksud kpm version').strip()
  if '3337' in result['kpm_version']:break
 except Exception:pass
 time.sleep(2)
else:raise RuntimeError('KPM autoload timeout')
run('forward','tcp:9494','tcp:9494')
end=time.monotonic()+60
while time.monotonic()<end:
 try:
  run('shell','am','broadcast','-a','com.longforus.xsettings.action.START_HTTP','-n','com.android.xsettings/com.longforus.xsettings.XstCommandReceiver','--include-stopped-packages')
  time.sleep(1)
  with opener.open('http://127.0.0.1:9494/health',timeout=3) as x:result['health']=json.load(x)
  if result['health'].get('ok'):break
 except Exception:pass
 time.sleep(2)
else:raise RuntimeError('HTTP after reboot timeout')
request=urllib.request.Request('http://127.0.0.1:9494/api/rpc',b'{"operation":"target.get"}',{'Content-Type':'application/json'})
with opener.open(request,timeout=5) as x:result['target']=json.load(x)
assert result['target']['data']['pid']==0
result['rotation']=run('shell','settings get system accelerometer_rotation; settings get system user_rotation').strip().splitlines();assert result['rotation']==rotation_before
result['kpm_list']=run('shell','su','-c','/data/adb/ksu/bin/ksud kpm list').strip()
result['kpm_hash']=run('shell','su','-c','sha256sum /vendor/lib/modules/kernelpatch.ko').strip()
log=run('shell','su','-c','dmesg',timeout=30);(r/'normal-reboot-dmesg.txt').write_text(log)
assert 'Kernel panic - not syncing' not in log and 'Oops:' not in log
result['passed']=True;(r/'normal-reboot-results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2));print(json.dumps(result,ensure_ascii=False),flush=True)

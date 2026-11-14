import subprocess,time,json,urllib.request
from pathlib import Path
r=Path('/tmp/lsdriver-fullrom-20261114');a=['adb','-s','21082FDF6007UJ']
def shell(s):return subprocess.check_output(a+['shell',s],text=True,timeout=30)
old_auto=shell('settings get system accelerometer_rotation').strip();old_rotation=shell('settings get system user_rotation').strip()
result={'original_auto':old_auto,'original_rotation':old_rotation,'orientations':{}}
try:
 shell('settings put system accelerometer_rotation 0')
 for rotation,name in [(0,'portrait'),(1,'landscape')]:
  shell('settings put system user_rotation '+str(rotation));time.sleep(2)
  layers=shell('dumpsys SurfaceFlinger --list')
  detail=shell('dumpsys SurfaceFlinger')
  (r/(name+'-surface.txt')).write_text(detail)
  (r/(name+'.png')).write_bytes(subprocess.check_output(a+['exec-out','screencap','-p'],timeout=30))
  result['orientations'][name]={'lark_layer':any('Lark' in x for x in layers.splitlines()),'layers':[x for x in layers.splitlines() if 'Lark' in x]}
  print(name,result['orientations'][name],flush=True)
finally:
 shell('settings put system user_rotation '+old_rotation)
 shell('settings put system accelerometer_rotation '+old_auto)
 (r/'rotation-results.json').write_text(json.dumps(result,indent=2))

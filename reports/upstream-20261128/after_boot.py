import json,subprocess,time,hashlib,shlex
from pathlib import Path
root=Path('/home/void/reverse/Linux-android-arm64/reports/upstream-20261128');adb=['adb','-s','21082FDF6007UJ']
def run(*args,timeout=60):
    p=subprocess.run(adb+list(args),capture_output=True,text=True,timeout=timeout)
    if p.returncode:raise RuntimeError((args,p.returncode,p.stdout,p.stderr))
    return p.stdout
print('Waiting for Android ADB',flush=True)
run('wait-for-device',timeout=600)
end=time.monotonic()+600
while time.monotonic()<end:
    try:
        if run('shell','getprop','sys.boot_completed',timeout=10).strip()=='1':break
    except Exception:pass
    time.sleep(3)
else:raise RuntimeError('boot completion timeout')
version=run('shell','getprop','ro.lineage.version').strip();print('Booted',version,flush=True)
assert version=='20.0-20261128-RELEASE-oriole',version
boot=run('shell','getprop ro.lineage.version; getprop ro.boot.slot_suffix; getprop sys.boot_completed; getenforce; su -c id; getprop init.svc.vendor.kpm_loader; su -c "/data/adb/ksu/bin/ksud kpm version"; su -c "/data/adb/ksu/bin/ksud kpm list"; su -c "sha256sum /vendor/lib/modules/kernelpatch.ko"; dumpsys battery')
(root/'postboot.txt').write_text(boot)
apk=Path('/home/void/AsfpProjects/xsettings/app/build/outputs/apk/release/app-release.apk')
print(run('install','-r',str(apk),timeout=180),flush=True)
installed=run('shell','pm','path','com.android.xsettings').strip().splitlines()[0].removeprefix('package:')
actual=run('shell','su','-c','sha256sum '+shlex.quote(installed)).strip().split()[0]
expected=hashlib.file_digest(apk.open('rb'),'sha256').hexdigest();assert actual==expected,(actual,expected)
(root/'installed-apk.json').write_text(json.dumps({'path':installed,'sha256':actual},indent=2))
print(run('shell','am','broadcast','-a','com.longforus.xsettings.action.START_HTTP','-n','com.android.xsettings/com.longforus.xsettings.XstCommandReceiver','--include-stopped-packages'),flush=True)
print(run('forward','tcp:9494','tcp:9494'),flush=True)
print(run('push','/tmp/lsdriver_integration_fixture','/data/local/tmp/lsdriver_integration_fixture'),flush=True)
run('shell','chmod','755','/data/local/tmp/lsdriver_integration_fixture')
run('shell','nohup /data/local/tmp/lsdriver_integration_fixture > /data/local/tmp/lsdriver_integration_fixture.log 2>&1 < /dev/null &')
time.sleep(2)
fixture=run('shell','head','-1','/data/local/tmp/lsdriver_integration_fixture.log');json.loads(fixture)
(root/'fixture.json').write_text(fixture);print('Controlled fixture',fixture.strip(),flush=True)
(root/'boot-dmesg.txt').write_text(run('shell','su','-c','dmesg',timeout=30))

import json, struct, subprocess, time, urllib.request, urllib.error
from pathlib import Path
root=Path('/home/void/reverse/Linux-android-arm64/reports/upstream-20261128')
fixture=json.loads((root/'fixture.json').read_text().splitlines()[0])
result={'fixture':fixture,'checks':{},'calls':[]}; counter=0
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
def save(): (root/'device-results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2))
def http(path,body=None,headers=None,method=None):
    req=urllib.request.Request('http://127.0.0.1:9494'+path,body,headers or {},method=method)
    try:
        with opener.open(req,timeout=35) as r:return r.status,r.read()
    except urllib.error.HTTPError as e:return e.code,e.read()
def rpc(op,params=None,ok=True):
    st,raw=http('/api/rpc',json.dumps({'operation':op,'params':params or {}}).encode(),{'Content-Type':'application/json'})
    value=json.loads(raw);result['calls'].append({'rpc':op,'params':params,'status':st,'response':value});save()
    if ok:assert st==200 and value.get('ok'),(op,value)
    return value.get('data') if ok else value
def mcp(method,params=None):
    global counter
    counter+=1
    st,raw=http('/mcp',json.dumps({'jsonrpc':'2.0','id':counter,'method':method,'params':params or {}}).encode(),{'Content-Type':'Application/JSON; charset=utf-8','MCP-Protocol-Version':'2025-11-25'})
    v=json.loads(raw);result['calls'].append({'mcp':method,'params':params,'status':st,'response':v});save()
    assert st==200 and 'error' not in v,v
    return v['result']
def tool(name,args=None):
    v=mcp('tools/call',{'name':name,'arguments':args or {}})
    assert not v.get('isError'),v
    response=v['structuredContent'];assert response['ok'],response
    return response['data'] if 'data' in response else response
def check(name,value=True):
    assert value,name
    result['checks'][name]=True;save();print(name,'PASS',flush=True)
def wait_scan():
    end=time.monotonic()+90
    while time.monotonic()<end:
        s=rpc('scan.get')
        if not s['scanning']:return s
        time.sleep(.15)
    raise AssertionError('scan timeout')
def addresses(value_type):
    items=rpc('scan.results',{'start':0,'count':200,'value_type':value_type})['items']
    return {int(x['addr'],0) for x in items}
def addr(name):return int(fixture[name],16)&0x00ffffffffffffff
def hx(name):return hex(addr(name))
selected=False
try:
    status,health=http('/health');check('health',status==200 and json.loads(health)['ok'])
    init=mcp('initialize',{'protocolVersion':'2025-11-25','capabilities':{},'clientInfo':{'name':'device-regression','version':'1'}})
    check('protocol_negotiation',init['protocolVersion']=='2025-11-25')
    catalog=mcp('tools/list')['tools'];scan=next(x for x in catalog if x['name']=='android_memory_scan_start')
    check('62_tools_new_scan_schema',len(catalog)==62 and {'alignment','rounding'}<=scan['inputSchema']['properties'].keys())
    check('media_type_rejection',http('/mcp',b'{}',{'Content-Type':'application/json-bogus'})[0]==415)
    check('browser_origin_rejection',http('/mcp',b'{}',{'Origin':'https://example.com','Content-Type':'application/json'})[0]==403)
    result['capabilities']=tool('android_bridge_capabilities')
    result['previous_target']=rpc('target.get')
    assert not rpc('breakpoint.get')['mode'],'existing breakpoint'
    tool('android_target_set_pid',{'pid':fixture['pid']});selected=True
    check('rpc_mcp_shared_target',rpc('target.get')['pid']==fixture['pid'])
    before=tool('android_memory_read',{'address':hx('payload'),'size':4})
    original=before['data_hex'];check('exact_memory_read',original.lower()==struct.pack('<I',0x13579bdf).hex() and before['complete'])
    written=tool('android_memory_write',{'address':hx('payload'),'data_hex':struct.pack('<I',0x2468ace0).hex()})
    check('memory_write_readback',written['verified'])
    time.sleep(1.1)
    observed=subprocess.check_output(['adb','-s','21082FDF6007UJ','shell','tail','-4','/data/local/tmp/lsdriver_integration_fixture.log'],text=True)
    result['fixture_observed_write']=observed;check('independent_write_observation','value=610839776' in observed)
    tool('android_memory_write',{'address':hx('payload'),'data_hex':original})
    from concurrent.futures import ThreadPoolExecutor
    def concurrent_read(index):
        st, raw=http('/api/rpc',json.dumps({'operation':'memory.read','params':{'address':hx('payload'),'size':4}}).encode(),{'Content-Type':'application/json'})
        value=json.loads(raw)
        return st==200 and value.get('ok') and value['data']['data_hex'].lower()==original.lower()
    with ThreadPoolExecutor(max_workers=8) as pool:
        check('concurrent_driver_requests',all(pool.map(concurrent_read,range(64))))
    # PROT_NONE is still physically mapped, so the driver may legitimately read it.
    # Unmapped memory at address 0 is an unambiguous read-integrity failure.
    partial=tool('android_memory_read',{'address':'0x0','size':32,'allow_partial':True})
    check('unmapped_read_integrity',not partial['complete'] and partial['read_bytes']==0 and partial['data_hex']=='')
    bad=rpc('memory.read',{'address':'0x0','size':32},ok=False);check('strict_unmapped_read_rejected',not bad['ok'])
    tool('android_memory_scan_start',{'mode':'equal','value_type':'i32','value':str(0x13579bdf),'alignment':1})
    state=wait_scan();values=addresses('i32')
    check('unaligned_integer_scan',state['alignment']==1 and {addr('payload'),addr('unaligned_int')}<=values)
    rpc('memory.write',{'address':hx('unaligned_int'),'data_hex':struct.pack('<I',0x2468ace0).hex()})
    tool('android_memory_scan_refine',{'mode':'equal','value_type':'i32','value':str(0x2468ace0)})
    state=wait_scan();values=addresses('i32');check('scan_refine_keeps_alignment',state['alignment']==1 and addr('unaligned_int') in values and addr('payload') not in values)
    rpc('memory.write',{'address':hx('unaligned_int'),'data_hex':original});rpc('scan.clear')
    tool('android_memory_scan_start',{'mode':'equal','value_type':'f32','value':'1.2','alignment':1,'rounding':'rounded'})
    wait_scan();values=addresses('f32');check('float_rounded_scan',addr('near') in values and addr('exact') in values and addr('outside') not in values)
    tool('android_memory_scan_refine',{'mode':'equal','value_type':'f32','value':'1.2','rounding':'exact'})
    wait_scan();values=addresses('f32');check('float_exact_refine',addr('exact') in values and addr('near') not in values);rpc('scan.clear')
    # A value spanning a page boundary must still be found by byte-aligned scanning.
    tool('android_memory_scan_start',{'mode':'equal','value_type':'f32','value':'1.26','alignment':1,'rounding':'exact'})
    wait_scan();check('cross_page_float_scan',addr('outside') in addresses('f32'));rpc('scan.clear')
    rpc('viewer.open',{'address':hx('hot'),'view_format':'disasm'});check('disassembly_viewer')
    result['breakpoints']={}
    for mode in ('hwbp','ptebp','stepbp'):
        try:
            rpc('breakpoint.set',{'mode':mode,'points':[{'address':hx('hot'),'bp_type':'execute','bp_scope':'main','length':4}]})
            time.sleep(.4);info=rpc('breakpoint.get')
            hits=sum(int(rec.get('hit_count',0)) for p in info['bp_info']['points'] for rec in p.get('records',[]))
            result['breakpoints'][mode]={'hits':hits};check(mode+'_hits',hits>0)
        finally:rpc('breakpoint.clear')
    stream=rpc('breakpoint.events.start')['stream']
    try:
        rpc('breakpoint.set',{'mode':'hwbp','points':[{'address':hx('hot'),'bp_type':'execute','bp_scope':'main','length':4}]})
        time.sleep(.2)
        events=rpc('breakpoint.events.read',{'stream':stream,'after':'0','count':8})
        check('breakpoint_event_stream',len(events['events'])>0)
        event=events['events'][0]
        back=rpc('breakpoint.backtrace',{'stream':stream,'event_seq':event['seq']})
        check('captured_fp_backtrace',back['event']['seq']==event['seq'])
    finally:rpc('breakpoint.clear');rpc('breakpoint.events.stop')
    rpc('trace.configure',{'directory':'/data/user/0/com.android.xsettings/files/lsdriver-integration-traces'})
    started=rpc('trace.start',{'start_pc':hx('hot'),'max_steps':64,'duration_ms':2000})
    time.sleep(2.3);rpc('trace.stop');trace=rpc('trace.status');result['trace']=trace
    check('bounded_trace_cleanup',not trace['active'] and not trace.get('cleanup_pending') and int(trace['saved_events'])>0)
    saved=rpc('trace.read',{'session_id':trace['session_id'],'after':'0','count':100})
    check('trace_file_readback',len(saved['events'])>0)
    check('service_still_healthy',http('/health')[0]==200)
    result['passed']=True
except Exception as e:
    result['passed']=False;result['error']=repr(e);print('FAILED',repr(e),flush=True)
finally:
    if selected:
        for op,params in [('trace.stop',{}),('breakpoint.clear',{}),('breakpoint.events.stop',{}),('scan.clear',{}),('memory.write',{'address':hx('payload'),'data_hex':struct.pack('<I',0x13579bdf).hex()}),('memory.write',{'address':hx('unaligned_int'),'data_hex':struct.pack('<I',0x13579bdf).hex()})]:
            try:rpc(op,params)
            except Exception as e:result.setdefault('cleanup_errors',[]).append({'op':op,'error':repr(e)})
    save()
raise SystemExit(0 if result.get('passed') else 1)

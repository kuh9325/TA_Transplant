#!/usr/bin/env python3
"""Sequential, alternating optimized benchmark processes; retains all raw logs.
Usage: run_benchmarks.py BEFORE AFTER IDLE_SAVE LOADED_SAVE DATA OUTPUT
Runs eight workloads five times per binary with one full hash every tick.
No build or regression process should overlap this runner.
"""
import hashlib,json,os,pathlib,re,statistics,subprocess,sys,time
before,after,idle,loaded,data,out=sys.argv[1:]
out=pathlib.Path(out);out.mkdir(parents=True,exist_ok=False)
bins={'8B':before,'8C':after}
(out/'binaries.json').write_text(json.dumps({k:{'path':v,'sha256':hashlib.sha256(pathlib.Path(v).read_bytes()).hexdigest()} for k,v in bins.items()},indent=2))
jobs=[('near-empty',['veteran-none','0','Greenhaven','12000'],2000),
 ('idle-100',['veteran-none','100','Greenhaven','10000'],2000),
 ('idle-1000',['save',idle,'30000'],2000),
 ('dense-combat',['veteran-combat','500','Greenhaven','900'],0),
 ('factory-recovery',['recovery','18000'],0),
 ('group-movement',['move','1000','Greenhaven','900','--type','mixed','--dest','long'],0),
 ('loaded-save',['save',loaded,'6000'],0),
 ('veteran-combat',['veteran-combat','1000','Greenhaven','900'],0)]
rows=[]
for scenario,args,warm in jobs:
 for rep in range(5):
  for label in (['8B','8C'] if rep%2==0 else ['8C','8B']):
   cmd=[bins[label],*args,'--data-path',data]
   env=dict(os.environ,RWE_BENCH_TICK_METRICS='1',RWE_BENCH_HASH_TICKS='1',RWE_BENCH_PHASE_TIMING='0',RWE_BENCH_WARMUP=str(warm))
   env.pop('RWE_HASH_DIAGNOSTICS',None)
   start=time.monotonic();name=f'{scenario}-{rep}-{label}';print('START',name,flush=True)
   # Read-only process/thermal snapshots, available when run with local permissions.
   with (out/(name+'-host.txt')).open('w') as f:
    subprocess.run(['ps','-Ao','pid,pcpu,comm'],stdout=f,stderr=f)
    subprocess.run(['pmset','-g','therm'],stdout=f,stderr=f)
   with (out/(name+'.log')).open('w') as f:
    try:code=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=180).returncode
    except subprocess.TimeoutExpired:code=None
   row=dict(scenario=scenario,label=label,repeat=rep,command=cmd,warmup=warm,exit=code,seconds=time.monotonic()-start)
   for k,v in re.findall(r'(?<![\w_])(\w+)=([0-9.eE+-]+)',(out/(name+'.log')).read_text()):
    try:row[k]=float(v)
    except ValueError:pass
   if scenario=='group-movement':
    for key in ['p50','p95','p99','max']:row['tick_'+key+'_ms']=row.get(key)
   row['status']='PASS' if code==0 and row.get('hash_samples',0)>0 else 'FAIL'
   rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2));print(json.dumps(row),flush=True)
   if row['status']!='PASS':sys.exit('incomplete scenario retained; stopping benchmark set')
summary={}
keys=['tick_mean_ms','tick_p50_ms','tick_p95_ms','tick_p99_ms','tick_max_ms','hash_mean_us','hash_work_percent','rss_after']
for scenario,_,_ in jobs:
 d={}
 for label in bins:
  rr=[x for x in rows if x['scenario']==scenario and x['label']==label]
  d[label]={k:statistics.median(x[k] for x in rr) for k in keys}
  d[label]['ranges']={k:[min(x[k] for x in rr),max(x[k] for x in rr)] for k in ['tick_mean_ms','hash_mean_us']}
  d[label]['hashes']=sorted(set(int(x['state_hash']) for x in rr))
  d[label]['hash_tick_checksums']=sorted(set(int(x['hash_tick_checksum']) for x in rr))
 d['equal_final_hashes']=d['8B']['hashes']==d['8C']['hashes']
 d['equal_hash_stream_checksums']=d['8B']['hash_tick_checksums']==d['8C']['hash_tick_checksums']
 d['percent']={k:100*(d['8C'][k]/d['8B'][k]-1) for k in ['tick_mean_ms','hash_mean_us']}
 summary[scenario]=d
(out/'summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary),flush=True)

#!/usr/bin/env python3
"""Execute gates and retain raw output. A failed command cannot create a passing job."""
import argparse, hashlib, json, os, pathlib, platform, resource, subprocess, sys, time
ROOT=pathlib.Path(__file__).resolve().parents[1]
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def implementation_files():
    names=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines()
    return sorted(n for n in set(names) if n.startswith(('src/','include/','tests/','apps/','cmake/','.github/workflows/','tools/','asm/','fuzz/','bench/')) and pathlib.Path(n).suffix not in ('.json','.md','.txt','.csv','.log') or n in ('CMakeLists.txt','Makefile','.clang-format','.gitignore'))
def source_manifest(): return {n:digest(ROOT/n) for n in implementation_files()}
def dump(p,d): p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(d,indent=2,sort_keys=True)+'\n')
def semantic(d): return {k:d[k] for k in ('columns','rows','rows_scanned','rows_matched')}
def semhash(d): return hashlib.sha256(json.dumps(d,sort_keys=True,separators=(',',':')).encode()).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--build',default='build-final');p.add_argument('--output',default='results/verified/local');p.add_argument('--build-type',default='Release',choices=['Release','Debug']);p.add_argument('--compiler',default=os.environ.get('CXX','c++'));p.add_argument('--sanitizers',action='store_true');p.add_argument('--tsan',action='store_true');p.add_argument('--require-arch',default='aarch64' if platform.machine() in ('arm64','aarch64') else 'x86_64');p.add_argument('--job-name',default='local-aarch64-release');a=p.parse_args()
 out=(ROOT/a.output).resolve();out.mkdir(parents=True,exist_ok=True);build=(ROOT/a.build).resolve();revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
 cpu_model=platform.processor()
 if platform.system()=='Darwin':
  cpu_model=subprocess.check_output(['sysctl','-n','machdep.cpu.brand_string'],text=True).strip()
 elif pathlib.Path('/proc/cpuinfo').exists():
  cpu_model=next((line.split(':',1)[1].strip() for line in pathlib.Path('/proc/cpuinfo').read_text().splitlines() if line.startswith(('model name','Hardware'))),platform.machine())
 job={'schema':2,'revision':revision,'job_name':a.job_name,'platform':platform.platform(),'machine':platform.machine(),'cpu_model':cpu_model,'logical_cpus':os.cpu_count(),'build_type':a.build_type,'compiler':a.compiler,'sanitizers':a.sanitizers,'tsan':a.tsan,'status':'RUNNING','source_manifest':source_manifest(),'sanitizer_options':{k:os.environ.get(k,'') for k in ('ASAN_OPTIONS','UBSAN_OPTIONS','TSAN_OPTIONS')},'commands':[]}
 def run(name,args,expected=0,cwd=ROOT):
  log=out/(name+'.log');start=time.monotonic_ns()
  with log.open('w') as f:
   f.write(json.dumps([str(x) for x in args])+'\n');f.flush();r=subprocess.run([str(x) for x in args],cwd=cwd,stdout=f,stderr=subprocess.STDOUT)
  entry={'name':name,'argv':[str(x) for x in args],'exit_code':r.returncode,'expected_exit':expected,'wall_ns':time.monotonic_ns()-start,'peak_child_rss':resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,'log':log.name};job['commands'].append(entry);dump(out/'job.json',job)
  if r.returncode!=expected: raise RuntimeError(name+' failed: '+str(r.returncode))
  if any(marker in log.read_text() for marker in ('ERROR: AddressSanitizer','runtime error:','WARNING: ThreadSanitizer','LeakSanitizer: detected')): raise RuntimeError(name+' emitted a sanitizer diagnostic')
  return entry
 try:
  configure=['cmake','-S',ROOT,'-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE='+a.build_type,'-DCMAKE_CXX_COMPILER='+a.compiler]
  if a.sanitizers:configure+=['-DVT_ENABLE_ASAN=ON','-DVT_ENABLE_UBSAN=ON']
  if a.tsan:configure+=['-DVT_ENABLE_TSAN=ON']
  run('configure',configure);run('build',['cmake','--build',build,'--parallel','4'])
  (out/'compile_commands.json').write_bytes((build/'compile_commands.json').read_bytes())
  (out/'CMakeCache.txt').write_bytes((build/'CMakeCache.txt').read_bytes())
  run('compiler-version',[a.compiler,'--version'])
  run('test-cases',[build/'bin/vectortick_tests','--list'],cwd=build)
  run('headers',[sys.executable,ROOT/'tools/check_headers.py','--compiler',a.compiler,'--json',out/'headers.json'])
  if a.tsan:
   run('crc-tsan',[build/'bin/vectortick_tests','--suite=crc_concurrency_tests'],cwd=build)
  else:
   # Discovery is retained separately from execution so a zero-case filter cannot pass.
   raw=subprocess.check_output(['ctest','--test-dir',build,'--show-only=json-v1'],text=True);(out/'discovery.json').write_text(raw);discovery=json.loads(raw)
   if len(discovery['tests'])<22:raise RuntimeError('Missing required discovered tests')
   run('ctest',['ctest','--test-dir',build,'--verbose','--output-on-failure','--no-tests=error','--output-junit',out/'ctest.xml'])
   for name in ('native-campaign.json','sql-campaign.json'):(out/name).write_bytes((build/name).read_bytes())
   run('empty-filter',[build/'bin/vectortick_tests','--suite=does_not_exist'],expected=1,cwd=build)
   # CTest already executes both full campaigns. Preserve those actual results.
   (out/'native-full.json').write_bytes((out/'native-campaign.json').read_bytes())
   (out/'sql-full.json').write_bytes((out/'sql-campaign.json').read_bytes())
   job['campaign_execution']='ctest'
   native=json.loads((out/'native-full.json').read_text())
   if native['arch']!=a.require_arch:raise RuntimeError('Wrong native campaign architecture')
   run('demo-default',[build/'bin/vectortick_demo'],cwd=build)
   run('demo',[build/'bin/vectortick_demo',out/'demo.vts'],cwd=build)
   workload_rows=2000 if a.sanitizers else 50000
   run('generate-pcap',[build/'bin/vectortick_verification_bench','--pcap',out/'input.pcap',out/'workload.json',str(workload_rows)])
   ingest=[]
   repetitions=1 if a.sanitizers else 5
   for rep in range(repetitions):
    r=run('ingest-'+str(rep),[build/'bin/vectortick_ingest',out/'input.pcap','-o',out/('ingest-'+str(rep)+'.vts')]);ingest.append(r)
   fixture=out/'ingest-0.vts';cli=[]
   sqls=['SELECT sequence, instrument_id, price_ticks, quantity FROM events WHERE price_ticks >= 10500 ORDER BY sequence DESC LIMIT 19','SELECT instrument_id, COUNT(*), SUM(quantity), MIN(price_ticks), MAX(price_ticks), AVG(quantity) FROM events WHERE quantity > 20 GROUP BY instrument_id ORDER BY instrument_id','SELECT COUNT(*), SUM(quantity), MIN(price_ticks), MAX(price_ticks) FROM events WHERE 0']
   for target in ('demo.vts','ingest-0.vts'):
    for i,sql in enumerate(sqls):
     expected_hash=None
     for engine in ('interpreter','vector','jit'):
      name=target+'-'+str(i)+'-'+engine;run(name,[build/'bin/vectortick_query','-e',engine,out/target,sql,'--json']);text=(out/(name+'.log')).read_text().split('\n',1)[1];data=json.loads(text);dump(out/(name+'.json'),data);h=semhash(semantic(data))
      if expected_hash is None:expected_hash=h
      if h!=expected_hash:raise RuntimeError('CLI result mismatch')
      obs=data['observability']
      if engine=='jit' and (obs['native_invocations']<data['rows_scanned'] or obs['fallback_count']):raise RuntimeError('CLI did not execute native code')
      cli.append({'file':name+'.json','semantic_digest':h,'input_sha256':digest(out/target)})
   for i,args in enumerate([['-e','unknown',fixture,'SELECT * FROM events'],[fixture,'SELECT +'],[fixture,'SELECT missing FROM events'],['-l','0',fixture,'SELECT * FROM events'],['-l','nope',fixture,'SELECT * FROM events'],['-e'],[fixture,'SELECT * FROM events LIMIT 0']]):run('invalid-'+str(i),[build/'bin/vectortick_query']+args,expected=1)
   # Corrupt the frozen input directly. Never rebuild a negative fixture in a reader.
   raw=(out/'input.pcap').read_bytes()
   corruptions={'pcap-short-header':raw[:23],'pcap-short-packet':raw[:-1],'pcap-excessive-snaplen':raw[:16]+b'\xff'*4+raw[20:],'pcap-inconsistent-packet':raw[:24]+raw[24:32]+b'\xff'*4+raw[36:],'vtp-checksum':raw[:-1]+bytes([raw[-1]^1])}
   for name,b in corruptions.items():
    bad=out/(name+'.pcap');bad.write_bytes(b);run(name,[build/'bin/vectortick_ingest',bad,'-o',out/(name+'.vts')],expected=1)
   run('benchmark',[build/'bin/vectortick_verification_bench','--query',fixture,out/'benchmark-raw.json',str(repetitions)])
   bench=json.loads((out/'benchmark-raw.json').read_text());hashes=set()
   for rep in bench['repetitions']:
    rep['input_sha256']=digest(fixture);rep['result_sha256']=semhash(rep['result']);hashes.add(rep['result_sha256'])
    if rep['result']['rows_scanned']!=workload_rows or not rep['wall_ns']:raise RuntimeError('Benchmark made no progress')
    if rep['engine']=='jit' and (rep['native_invocations']<workload_rows or rep['fallback_count'] or (rep['cache']=='warm' and rep['functions_compiled']!=0) or (rep['cache']=='cold' and rep['functions_compiled']==0)):raise RuntimeError('Invalid JIT benchmark')
   if len(hashes)!=1:raise RuntimeError('Benchmark result digests differ')
   bench['ingestion_repetitions']=ingest;bench['workload_pcap_sha256']=digest(out/'input.pcap');bench['input_vts_sha256']=digest(fixture);bench['cli_agreement']=cli
   # Ingestion repeats must decode to identical complete typed records.
   all_fields='SELECT exchange_ts_ns, receive_ts_ns, sequence, instrument_id, event_type, side, flags, price_ticks, quantity, venue_id, source_id, trade_or_order_id FROM events'
   ingest_hash=None
   for rep in range(repetitions):
    name='ingestion-result-'+str(rep);run(name,[build/'bin/vectortick_query','-e','vector',out/('ingest-'+str(rep)+'.vts'),all_fields,'--json']);data=json.loads((out/(name+'.log')).read_text().split('\n',1)[1]);h=semhash(semantic(data))
    if data['rows_scanned']!=workload_rows or len(data['rows'])!=workload_rows:raise RuntimeError('Ingestion lost rows')
    if ingest_hash is None:ingest_hash=h
    if h!=ingest_hash:raise RuntimeError('Ingestion digest mismatch')
    bench['ingestion_repetitions'][rep]['result_sha256']=h;bench['ingestion_repetitions'][rep]['rows_scanned']=workload_rows
   dump(out/'benchmarks.json',bench)
  run('diff-check',['git','diff','--check'])
  if source_manifest()!=job['source_manifest']:raise RuntimeError('Source changed during verification')
  job['status']='PASSED'
 except Exception as e:
  job['status']='FAILED';job['error']=str(e);print(str(e),file=sys.stderr)
 finally:
  job['artifact_manifest']={str(x.relative_to(out)):digest(x) for x in sorted(out.rglob('*')) if x.is_file() and x.name!='job.json'};dump(out/'job.json',job)
 print('Verification '+job['status']+': '+str(out));return 0 if job['status']=='PASSED' else 1
if __name__=='__main__':sys.exit(main())
